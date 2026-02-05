#include "MeshletBuilder.h"
#include "pch.h"
#include <unordered_map>
#include <numeric>
#include <cassert>
#include <cfloat>
#include <execution> 
#include "../Core/Utility.h"
#include "Model.h"
#include "meshoptimizer.h"

using namespace Renderer;

static constexpr float kInfinity = std::numeric_limits<float>::infinity();
static constexpr float kValidateEpsilon = 1e-4f;

// Decode DXGI_FORMAT_R10G10B10A2_UNORM to float3 normals ([-1,1])
inline Math::Vector3 DecodeNormal_R10G10B10A2(uint32_t packed)
{
	constexpr float inv10 = 1.0f / 1023.0f;
	const uint32_t r = (packed) & 0x3FFu;
	const uint32_t g = (packed >> 10) & 0x3FFu;
	const uint32_t b = (packed >> 20) & 0x3FFu;

	float x = float(r) * inv10;
	float y = float(g) * inv10;
	float z = float(b) * inv10;

	x = x * 2.0f - 1.0f;
	y = y * 2.0f - 1.0f;
	z = z * 2.0f - 1.0f;

	const float len2 = x * x + y * y + z * z;
	if (len2 > 0.0f)
	{
		const float invLen = 1.0f / std::sqrt(len2);
		x *= invLen; y *= invLen; z *= invLen;
	}
	return Math::Vector3(x, y, z);
}

inline Math::Vector4 DecodeR10G10B10A2UNORMToFloat4(uint32_t packed)
{
	const uint32_t r = (packed >> 0) & 0x3FF; // 10 bits
	const uint32_t g = (packed >> 10) & 0x3FF;
	const uint32_t b = (packed >> 20) & 0x3FF;
	const uint32_t a = (packed >> 30) & 0x3;
	return Math::Vector4(r / 1023.f, g / 1023.f, b / 1023.f, a / 3.f) * 2.f - Math::Vector4(1.0);
}

inline std::tuple<float, float> DecodeR16G16FLOATToFloat2(uint32_t packed)
{
	return { F16ToF32(packed & 0xFFFF), F16ToF32(packed >> 16) };
}

struct LocalIndexCache {
	short cache[1024]; // 2KB, fits perfectly in L1 cache
	LocalIndexCache() { memset(cache, -1, sizeof(cache)); }

	uint32_t Get(uint32_t globalIdx, const std::vector<uint32_t>& usedIndices) {
		// Modulo (since 1024, the compiler optimizes to bitwise & 1023)
		uint32_t h = globalIdx & 1023;
		short slot = cache[h];

		// Check cache hit: slot stores the index into usedIndices
		if (slot >= 0 && usedIndices[slot] == globalIdx) {
			return (uint32_t)slot;
		}

		// Cache miss (hash collision or first access): do a linear search
		// Tip: a group's vertices are usually only a few hundred to a few thousand,
		// and on modern CPUs, linear search (sequential access) is often much faster than binary search (random access).
		for (size_t i = 0; i < usedIndices.size(); ++i) {
			if (usedIndices[i] == globalIdx) {
				cache[h] = (short)i; // Update cache
				return (uint32_t)i;
			}
		}
		return 0xFFFFFFFF;
	}
};

static bool VertexAttributesDiffer(const MeshletBuildArgs& buildArgs, uint32_t a, uint32_t b)
{
	const unsigned char* va = buildArgs.VBData + size_t(a) * buildArgs.vertexStride;
	const unsigned char* vb = buildArgs.VBData + size_t(b) * buildArgs.vertexStride;

	size_t offset = 12; // position

	// Normal (R10G10B10A2)
	const uint32_t* na = reinterpret_cast<const uint32_t*>(va + offset);
	const uint32_t* nb = reinterpret_cast<const uint32_t*>(vb + offset);
	if (*na != *nb)
		return true;
	offset += 4;

	if (buildArgs.psoFlags & PSOFlags::kHasTangent)
	{
		const uint32_t* ta = reinterpret_cast<const uint32_t*>(va + offset);
		const uint32_t* tb = reinterpret_cast<const uint32_t*>(vb + offset);
		if (*ta != *tb)
			return true;
		offset += 4;
	}

	if (buildArgs.psoFlags & PSOFlags::kHasUV0)
	{
		const uint32_t* uva = reinterpret_cast<const uint32_t*>(va + offset);
		const uint32_t* uvb = reinterpret_cast<const uint32_t*>(vb + offset);
		if (*uva != *uvb)
			return true;
		offset += 4;
	}

	return false;
}

static void BuildAttributeProtectLocks(const MeshletBuildArgs& buildArgs, std::span<const uint32_t> posRemap, std::vector<unsigned char>& outVertexLock)
{
	if (!buildArgs.settings.bUseSimplifyPermissive)
		return;

	const size_t vertexCount = posRemap.size();
	for (size_t i = 0; i < vertexCount; ++i)
	{
		uint32_t r = posRemap[i];
		if (r == i)
			continue;

		if (VertexAttributesDiffer(buildArgs, static_cast<uint32_t>(i), r))
		{
			outVertexLock[i] |= meshopt_SimplifyVertex_Protect;
			outVertexLock[r] |= meshopt_SimplifyVertex_Protect;
		}
	}
}

MeshletBuildProducts MeshletBuilder::Build(
	const MeshletBuildArgs& buildArgs)
{
	if (buildArgs.IBData == nullptr || buildArgs.VBData == nullptr || 
		buildArgs.indexCount == 0 || buildArgs.vertexCount == 0)
		return {};

	if (buildArgs.settings.bOutputDebugInfo)
		Utility::Printf(L"[MeshletBuilder] Start building meshlets for meshBufferIndex=%u, materialBufferIndex=%u\n",
			buildArgs.meshBufferIndex, buildArgs.materialBufferIndex);

	// Generate position-only remap (for later boundary locking)
	std::vector<uint32_t> posRemap(buildArgs.vertexCount);
	GeneratePositionRemap(buildArgs, posRemap);

	// LOD0: full mesh -> meshlets
	std::vector<Meshlet> current = BuildLOD0Meshlets(buildArgs);

	// LOD0: initialize simplification queue
	std::vector<uint32_t> activeIds(current.size());
	std::iota(activeIds.begin(), activeIds.end(), 0u);

	uint32_t lastTriCount = 0;
	for (const auto& m : current) lastTriCount += static_cast<uint32_t>(m.Triangles.size() / 3);

	// Print LOD0 stats
	if (buildArgs.settings.bOutputDebugInfo)
		Utility::Printf(L"[MeshletBuilder] LOD %u: meshlets=%u, triangles=%u\n",
			0u, static_cast<uint32_t>(current.size()), lastTriCount);

	std::vector<Group> currentGroups;
	std::vector<unsigned char> baseVertexLocks(buildArgs.vertexCount, 0);
	BuildAttributeProtectLocks(buildArgs, posRemap, baseVertexLocks);

	// Iterative simplification
	uint32_t lodLevel = 1;
	while (true)
	{
		// Group by shared vertices / spatial proximity
		auto groupIds= GroupMeshlets(buildArgs, current, activeIds, posRemap, lodLevel-1, currentGroups);
		if (groupIds.empty())
			break;

		for (auto gid : groupIds)
		{
			for (auto mid : currentGroups[gid].MeshletIDs)
			{
				// Set meshlet GroupID
				current[mid].GroupID = gid;
				current[mid].GroupChildIndex = mid;
			}
		}

		// Build vertex locks (lock all position-only vertices shared across groups)
		std::vector<unsigned char> vertexLock = baseVertexLocks;
		BuildVertexLocksByGroups(currentGroups, groupIds, current, posRemap, buildArgs.vertexCount, vertexLock);

		// Try simplifying each group -> generate next-level meshlets
		struct GroupResult
		{
			bool simplified = false;
			float err = 0.0f;                     // Error returned by SimplifyGroup
			float groupSphere[4]{};
			std::vector<Meshlet> generated;   // New meshlets generated for this group
			std::vector<uint32_t> originalIds;    // Original meshlet IDs for this group (for ParentError/Bounds writeback)
			uint32_t groupID = 0;				  // Original group id
		};

		std::vector<GroupResult> results(groupIds.size());
		std::vector<size_t> gi(groupIds.size());
		std::iota(gi.begin(), gi.end(), size_t(0));

		// Run simplification attempts per group in parallel (local results only, no shared writes)
		std::for_each(std::execution::seq, gi.begin(), gi.end(),
			[&](size_t idx)
			{
				const Group& g = currentGroups[groupIds[idx]];
				GroupResult r;
				r.groupID = g.GroupID;
				std::copy(g.GroupSphere, g.GroupSphere + 4, r.groupSphere);
				r.originalIds = g.MeshletIDs;

				// Single-meshlet group needs no simplification
				if (g.MeshletIDs.size() <= 1)
				{
					r.simplified = false;
					results[idx] = std::move(r);
					return;
				}

				float err = 0.0f;
				std::vector<Meshlet> generated;
				if (SimplifyGroup(buildArgs, g, current, vertexLock, generated, err))
				{
					r.simplified = true;
					r.err = err;
					r.generated = std::move(generated);
				}
				else
				{
					r.simplified = false;
				}

				results[idx] = std::move(r);
			});

		std::vector<Meshlet> newMeshlets;
		std::vector<uint32_t> nextActive;
		nextActive.reserve(activeIds.size());

		// Serial: set error, bounds, LODLevel, update parent error, build nextActive
		for (const auto& r : results)
		{
			if (!r.simplified)
			{
				// Simplification failed, keep original meshlet
				currentGroups[r.groupID].ParrentError = kInfinity;
				continue;
			}

			const size_t baseOffset = newMeshlets.size();
			// Write bounding sphere
			for (auto& nm : const_cast<std::vector<Meshlet>&>(r.generated))
			{
				Meshlet out = std::move(nm);
                std::memcpy(out.BoundSphere, r.groupSphere, sizeof(float) * 4);
				newMeshlets.emplace_back(std::move(out));
			}

			// Global indices for new meshlets: current.size() + baseOffset + j
			for (size_t j = 0; j < r.generated.size(); ++j)
				nextActive.push_back(static_cast<uint32_t>(current.size() + baseOffset + j));

			// Write back parent error/bounds (only when simplification succeeds)
			float parrentErr = r.err;
			for (uint32_t id : r.originalIds)
			{
				if (current[id].RefineGroupID != 0xFFFFFFFF)
				{
					parrentErr = std::max(parrentErr, currentGroups[current[id].RefineGroupID].ParrentError * buildArgs.settings.LODErrorMergePrevious);
					
				}
			}
			currentGroups[r.groupID].ParrentError = parrentErr;
		}

		if (newMeshlets.empty())
			break;

		// Print stats for this level
		{
			uint32_t triCount = 0;
			for (const auto& m : newMeshlets) triCount += static_cast<uint32_t>(m.Triangles.size() / 3);
			float reduction = 1.0f - float(triCount) / float(lastTriCount);
			if (buildArgs.settings.bOutputDebugInfo)
				Utility::Printf(L"[MeshletBuilder] LOD %u: meshlets=%u, triangles=%u\n",
					lodLevel, static_cast<uint32_t>(newMeshlets.size()), triCount);
			lastTriCount = triCount;
			if (reduction < buildArgs.settings.minReductionRatio)
			{
				if (buildArgs.settings.bOutputDebugInfo)
					Utility::Printf(L"[MeshletBuilder]  Simplification reduction %.2f%% below threshold %.2f%%, stopping.\n",
						reduction * 100.0f, buildArgs.settings.minReductionRatio * 100.0f);
				break;
			}
		}

		// Append to current, record LOD level and meshlets visible at this level
		current.insert(current.end(),
			std::make_move_iterator(newMeshlets.begin()),
			std::make_move_iterator(newMeshlets.end()));

		++lodLevel;
		activeIds.swap(nextActive);
	}

	MeshletBuildProducts products = BuildStreamingData(buildArgs, currentGroups, current);
	if (buildArgs.settings.bValidateBuild)
	{
		const bool ok = ValidateBuild(buildArgs, currentGroups, current);
		if (!ok)
		{
			Utility::Printf(L"[MeshletBuilder] Validation failed for meshBufferIndex=%u, materialBufferIndex=%u\n",
				buildArgs.meshBufferIndex, buildArgs.materialBufferIndex);
		}
	}
	return products;
}

std::vector<MeshletBuilder::Meshlet>
MeshletBuilder::BuildLOD0Meshlets(
	const MeshletBuildArgs& buildArgs)
{
	std::vector<Meshlet> out;
	BuildMeshletsFromIndices(buildArgs, reinterpret_cast<uint32_t*>(buildArgs.IBData), buildArgs.indexCount, out);

	std::for_each(std::execution::seq, out.begin(), out.end(),
		[&buildArgs](Meshlet& m)
		{
			ComputeMeshletSphere(buildArgs, m, m.BoundSphere);
		});

	return out;
}

void MeshletBuilder::BuildMeshletsFromIndices(
	const MeshletBuildArgs& buildArgs,
	const uint32_t* indices, size_t indexCount,
	std::vector<Meshlet>& out)
{
	const float* pos = reinterpret_cast<const float*>(buildArgs.VBData);
	const size_t stride = buildArgs.vertexStride;

	size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, buildArgs.settings.MaxMeshletVertices, buildArgs.settings.MinMeshletTriangles);
	std::vector<meshopt_Meshlet> mlets(maxMeshlets);
	std::vector<uint32_t> mlVertices(indexCount); 
	std::vector<unsigned char> mlTriangles(indexCount);

	size_t mlCount = meshopt_buildMeshletsFlex(
		mlets.data(), mlVertices.data(), mlTriangles.data(),
		indices, indexCount,
		pos, buildArgs.vertexCount, stride,
		buildArgs.settings.MaxMeshletVertices, buildArgs.settings.MinMeshletTriangles, buildArgs.settings.MaxMeshletTriangles, 0.0f, buildArgs.settings.ClusterSplitFactor);

	// Convert to TempMeshlet
	out.reserve(out.size() + mlCount);
	for (size_t i = 0; i < mlCount; ++i)
	{
		const auto& ml = mlets[i];

		Meshlet tm;

		tm.Vertices.resize(ml.vertex_count);
		std::copy_n(mlVertices.data() + ml.vertex_offset, ml.vertex_count, tm.Vertices.begin());

		tm.Triangles.resize(ml.triangle_count * 3);
		std::copy_n(mlTriangles.data() + ml.triangle_offset, ml.triangle_count * 3, tm.Triangles.begin());

		// Optimize locality
		meshopt_optimizeMeshlet(
			tm.Vertices.data(), tm.Triangles.data(),
			ml.triangle_count, ml.vertex_count);

		ComputeMeshletAABB(buildArgs, tm, tm.BBoxMin, tm.BBoxMax);

		out.emplace_back(std::move(tm));
	}
}

void MeshletBuilder::ComputeMeshletSphere(
	const MeshletBuildArgs& buildArgs,
	const Meshlet& m,
	float outSphere[4])
{
	const float* pos = reinterpret_cast<const float*>(buildArgs.VBData);

	meshopt_Bounds b = meshopt_computeMeshletBounds(
		m.Vertices.data(), m.Triangles.data(),
		m.Triangles.size() / 3,
		pos, buildArgs.vertexCount, buildArgs.vertexStride);

	outSphere[0] = b.center[0];
	outSphere[1] = b.center[1];
	outSphere[2] = b.center[2];
	outSphere[3] = b.radius;
}

void MeshletBuilder::ComputeMeshletAABB(
	const MeshletBuildArgs& buildArgs,
    const Meshlet& m,
	float outMin[3],
	float outMax[3])
{
	if (m.Vertices.empty())
	{
		outMin[0] = outMin[1] = outMin[2] = 0.0f;
		outMax[0] = outMax[1] = outMax[2] = 0.0f;
		return;
	}

	float minX = kInfinity, minY = kInfinity, minZ = kInfinity;
	float maxX = -kInfinity, maxY = -kInfinity, maxZ = -kInfinity;

	for (uint32_t idx : m.Vertices)
	{
		const unsigned char* vPtr = buildArgs.VBData + size_t(idx) * buildArgs.vertexStride;
		const float* pos = reinterpret_cast<const float*>(vPtr);

		minX = std::min(minX, pos[0]); maxX = std::max(maxX, pos[0]);
		minY = std::min(minY, pos[1]); maxY = std::max(maxY, pos[1]);
		minZ = std::min(minZ, pos[2]); maxZ = std::max(maxZ, pos[2]);
	}

	outMin[0] = minX; outMin[1] = minY; outMin[2] = minZ;
	outMax[0] = maxX; outMax[1] = maxY; outMax[2] = maxZ;
}

void MeshletBuilder::MergeSphere(const float a[4], const float b[4], float out[4])
{
	Math::Vector3 ca(a[0], a[1], a[2]);
	Math::Vector3 cb(b[0], b[1], b[2]);
	float ra = a[3], rb = b[3];

	Math::Vector3 diff = cb - ca;
	float dist = Length(diff);

	if (ra >= dist + rb) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; out[3] = ra; return; }
	if (rb >= dist + ra) { out[0] = b[0]; out[1] = b[1]; out[2] = b[2]; out[3] = rb; return; }

	Math::Vector3 dir = dist > 1e-6f ? diff / dist : Math::Vector3(0.0f);
	float newR = 0.5f * (ra + rb + dist);
	Math::Vector3 newC = ca + dir * (newR - ra);
	out[0] = newC.GetX(); out[1] = newC.GetY(); out[2] = newC.GetZ(); out[3] = newR;
}

static void MergeAABB(
	const float aMin[3], const float aMax[3],
	const float bMin[3], const float bMax[3],
	float outMin[3], float outMax[3])
{
	outMin[0] = std::min(aMin[0], bMin[0]);
	outMin[1] = std::min(aMin[1], bMin[1]);
	outMin[2] = std::min(aMin[2], bMin[2]);

	outMax[0] = std::max(aMax[0], bMax[0]);
	outMax[1] = std::max(aMax[1], bMax[1]);
	outMax[2] = std::max(aMax[2], bMax[2]);
}

static bool SphereContains(const float outer[4], const float inner[4], float eps)
{
	const float dx = inner[0] - outer[0];
	const float dy = inner[1] - outer[1];
	const float dz = inner[2] - outer[2];
	const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	return dist + inner[3] <= outer[3] + eps;
}

void MeshletBuilder::GeneratePositionRemap(
	const MeshletBuildArgs& buildArgs,
	std::vector<uint32_t>& outPosRemap)
{
	meshopt_generatePositionRemap(outPosRemap.data(), reinterpret_cast<const float*>(buildArgs.VBData), buildArgs.vertexCount, buildArgs.vertexStride);
}

std::vector<uint32_t> MeshletBuilder::GroupMeshlets(
	const MeshletBuildArgs& buildArgs,
	const std::vector<Meshlet>& current,
	std::span<const uint32_t> subsetIds,
	std::span<const uint32_t> posRemap,
	uint32_t LODLevel,
	std::vector<Group>& groups)
{
	const size_t clusterCount = subsetIds.size();
	if (clusterCount == 0) return {};

	// Prepare input for partitionClusters
	// Concatenate unique vertex lists of each meshlet
	// Count total indices after expansion
	size_t totalIndexCount = 0;
	for (uint32_t id : subsetIds)
		totalIndexCount += current[id].Triangles.size();

	std::vector<uint32_t> clusterIndices;
	clusterIndices.reserve(totalIndexCount);
	std::vector<unsigned int> clusterCounts(clusterCount);

	// Expand each meshlet's micro-triangles into original vertex indices
	for (size_t i = 0; i < clusterCount; ++i)
	{
		const auto& m = current[subsetIds[i]];
		const size_t triIdxCount = m.Triangles.size(); // 3 * triangle_count
		clusterCounts[i] = static_cast<unsigned int>(triIdxCount);

		for (size_t t = 0; t < triIdxCount; ++t)
		{
			uint8_t local = m.Triangles[t];           // Local 0..N-1
			uint32_t original = m.Vertices[local];    // Original vertex index
			clusterIndices.push_back(posRemap[original]);
		}
	}

	std::vector<unsigned int> partition(clusterCount);
	const float* pos = reinterpret_cast<const float*>(buildArgs.VBData);

	size_t partCount = meshopt_partitionClusters(
		partition.data(),
		clusterIndices.data(), clusterIndices.size(),
		clusterCounts.data(), clusterCount,
		pos, buildArgs.vertexCount, buildArgs.vertexStride,
		buildArgs.settings.TargetMeshletsPerGroup);

	// Aggregate into groups
	std::vector<Group> newGroups(partCount);
	for (size_t i = 0; i < clusterCount; ++i)
		newGroups[partition[i]].MeshletIDs.push_back(subsetIds[i]);

	std::vector<uint32_t> groupIds(partCount);
	// Compute group sphere (merge meshlet spheres)
	for (size_t groupIndex = 0; groupIndex < newGroups.size(); groupIndex++)
	{
		auto& g = newGroups[groupIndex];
		bool first = true;
		float acc[4]{};
		float bboxMin[3]{};
		float bboxMax[3]{};
		for (uint32_t mid : g.MeshletIDs)
		{
			if (first)
			{
				std::memcpy(acc, current[mid].BoundSphere, sizeof(float) * 4);
				std::memcpy(bboxMin, current[mid].BBoxMin, sizeof(float) * 3);
				std::memcpy(bboxMax, current[mid].BBoxMax, sizeof(float) * 3);
				first = false;
			}
			else
			{
				MergeSphere(acc, current[mid].BoundSphere, acc);
				MergeAABB(bboxMin, bboxMax, current[mid].BBoxMin, current[mid].BBoxMax, bboxMin, bboxMax);
			}
		}
		std::copy(acc, acc + 4, g.GroupSphere);
		std::copy(bboxMin, bboxMin + 3, g.GroupBBoxMin);
		std::copy(bboxMax, bboxMax + 3, g.GroupBBoxMax);

		g.GroupID = static_cast<uint32_t>(groupIndex + groups.size());
		g.LODLevel = static_cast<uint8_t>(LODLevel);
		groupIds[groupIndex] = static_cast<uint32_t>(groupIndex + groups.size());
	}

	groups.insert(groups.end(),
		std::make_move_iterator(newGroups.begin()),
		std::make_move_iterator(newGroups.end()));

	return groupIds;
}

void MeshletBuilder::BuildVertexLocksByGroups(
	const std::vector<Group>& groups,
	const std::vector<uint32_t>& groupIds,
	const std::vector<Meshlet>& current,
	const std::vector<uint32_t>& posRemap,
	size_t vertexCount,
	std::vector<unsigned char>& outVertexLock)
{
	// Mark which group first owns each position-only vertex
	// If the same position vertex is owned by different groups, lock that position
	// Finally set all original vertices with that position id to lock=1
	uint32_t maxPosId = 0;
	for (size_t v = 0; v < vertexCount; ++v) maxPosId = std::max(maxPosId, posRemap[v]);
	std::vector<int32_t> owner(maxPosId + 1, -1);
	std::vector<uint8_t> lockedPos(maxPosId + 1, 0);

	for (size_t gi = 0; gi < groupIds.size(); ++gi)
	{
		uint32_t gid = static_cast<uint32_t>(groupIds[gi]);
		for (uint32_t mid : groups[gid].MeshletIDs)
		{
			const auto& m = current[mid];
			for (uint32_t v : m.Vertices)
			{
				uint32_t pid = posRemap[v];
				if (owner[pid] == -1) owner[pid] = gid;
				else if (owner[pid] != static_cast<int32_t>(gid)) lockedPos[pid] = 1;
			}
		}
	}

	// Write back to original vertex locks
	for (size_t v = 0; v < vertexCount; ++v)
	{
		if (lockedPos[posRemap[v]])
			outVertexLock[v] |= meshopt_SimplifyVertex_Lock;
	}
}

bool MeshletBuilder::SimplifyGroup(
	const MeshletBuildArgs& buildArgs,
	const Group& g,
	const std::vector<Meshlet>& current,
	const std::vector<unsigned char>& vertexLock,
	std::vector<Meshlet>& outNewMeshlets,
	float& outError)
{
	// Collect all unique vertex indices used by the group and build a Global->Local mapping
	// Estimated max vertex count = meshlet count * MaxVerts
	// Using vector + sort + unique is faster and more memory-efficient than unordered_map
	std::vector<uint32_t> usedGlobalIndices;
	usedGlobalIndices.reserve(g.MeshletIDs.size() * buildArgs.settings.MaxMeshletVertices);

	for (uint32_t mid : g.MeshletIDs)
	{
		const auto& m = current[mid];
		usedGlobalIndices.insert(usedGlobalIndices.end(), m.Vertices.begin(), m.Vertices.end());
	}
	std::sort(usedGlobalIndices.begin(), usedGlobalIndices.end());
	auto last = std::unique(usedGlobalIndices.begin(), usedGlobalIndices.end());
	usedGlobalIndices.erase(last, usedGlobalIndices.end());

	if (usedGlobalIndices.empty())
		return false;

	// Build local vertex/index data
	// Concatenate group triangles (using local indices)
	std::vector<uint32_t> localIndices;
	localIndices.reserve(g.MeshletIDs.size() * buildArgs.settings.MaxMeshletTriangles * 3);

	LocalIndexCache fastCache;

	for (uint32_t mid : g.MeshletIDs)
	{
		const auto& m = current[mid];
		for (size_t t = 0; t < m.Triangles.size(); t += 3)
		{
			uint32_t g0 = m.Vertices[m.Triangles[t + 0]];
			uint32_t g1 = m.Vertices[m.Triangles[t + 1]];
			uint32_t g2 = m.Vertices[m.Triangles[t + 2]];
			localIndices.push_back(fastCache.Get(g0, usedGlobalIndices));
			localIndices.push_back(fastCache.Get(g1, usedGlobalIndices));
			localIndices.push_back(fastCache.Get(g2, usedGlobalIndices));
		}
	}

	if (localIndices.empty())
		return false;

	std::vector<float> localPositions;
	std::vector<float> localAttrs;

	// Prepare local attribute buffers (position + attributes)
	size_t localVertexCount = usedGlobalIndices.size();
	localPositions.resize(localVertexCount * 3);

	uint32_t attributeValueCount = 3; // Normal
	if (buildArgs.psoFlags & PSOFlags::kHasTangent) attributeValueCount += 4;
	if (buildArgs.psoFlags & PSOFlags::kHasUV0) attributeValueCount += 2;

	localAttrs.resize(localVertexCount * attributeValueCount);

	for (size_t i = 0; i < localVertexCount; ++i)
	{
		uint32_t globalIdx = usedGlobalIndices[i];
		const unsigned char* vPtr = buildArgs.VBData + ((size_t)globalIdx) * buildArgs.vertexStride;

		// Position
		memcpy(&localPositions[i * 3], vPtr, 12);

		// Attributes
		size_t attrBase = i * attributeValueCount;
		size_t attrOffset = 0;
		uint32_t vertexByteOffset = 12;

		const uint32_t* nPacked = reinterpret_cast<const uint32_t*>(vPtr + vertexByteOffset);
		Math::Vector3 norm = DecodeNormal_R10G10B10A2(*nPacked);
		localAttrs[attrBase + attrOffset++] = norm.GetX();
		localAttrs[attrBase + attrOffset++] = norm.GetY();
		localAttrs[attrBase + attrOffset++] = norm.GetZ();
		vertexByteOffset += 4;

		if (buildArgs.psoFlags & PSOFlags::kHasTangent)
		{
			const uint32_t* tPacked = reinterpret_cast<const uint32_t*>(vPtr + vertexByteOffset);
			Math::Vector4 tangent = DecodeR10G10B10A2UNORMToFloat4(*tPacked);
			localAttrs[attrBase + attrOffset++] = tangent.GetX();
			localAttrs[attrBase + attrOffset++] = tangent.GetY();
			localAttrs[attrBase + attrOffset++] = tangent.GetZ();
			localAttrs[attrBase + attrOffset++] = tangent.GetW();
			vertexByteOffset += 4;
		}
		if (buildArgs.psoFlags & PSOFlags::kHasUV0)
		{
			const uint32_t* uv0Ptr = reinterpret_cast<const uint32_t*>(vPtr + vertexByteOffset);
			auto [unpackedU, unpackedV] = DecodeR16G16FLOATToFloat2(*uv0Ptr);
			localAttrs[attrBase + attrOffset++] = unpackedU;
			localAttrs[attrBase + attrOffset++] = unpackedV;
			vertexByteOffset += 4;
		}
	}

    std::vector<float> attributeWeights(attributeValueCount, 0.5f);
    size_t attributeOffset = 3;
    if (buildArgs.psoFlags & PSOFlags::kHasTangent)
    {
         attributeWeights[attributeOffset++] = 0.1f;
         attributeWeights[attributeOffset++] = 0.1f;
         attributeWeights[attributeOffset++] = 0.1f;
         attributeWeights[attributeOffset++] = 0.5f;
    }
    if (buildArgs.psoFlags & PSOFlags::kHasUV0)
    {
        attributeWeights[attributeOffset++] = 0.1f;
        attributeWeights[attributeOffset++] = 0.1f;
    }

	// Process lock array (localize)
	std::vector<unsigned char> localLocks;
	if (!vertexLock.empty())
	{
		localLocks.resize(localVertexCount);
		for (size_t i = 0; i < localVertexCount; ++i)
		{
			localLocks[i] = vertexLock[usedGlobalIndices[i]];
		}
	}

	// Run simplification (using local data)
	const size_t targetIndexCount = size_t(localIndices.size() * buildArgs.settings.TargetSimplifyRatio);
	std::vector<uint32_t> simplifiedLocal(localIndices.size());
	float resultError = 0.0f;

	unsigned int options =
		meshopt_SimplifySparse |
		meshopt_SimplifyErrorAbsolute |
		(buildArgs.settings.bUseSimplifyPermissive ? meshopt_SimplifyPermissive : 0);

	size_t newCount = meshopt_simplifyWithAttributes(
		simplifiedLocal.data(),
		localIndices.data(), localIndices.size(),
		localPositions.data(), localVertexCount, sizeof(float) * 3, // pos stride = 12
		localAttrs.data(), sizeof(float) * attributeValueCount,
		attributeWeights.data(), attributeValueCount,
		localLocks.empty() ? nullptr : localLocks.data(),
		targetIndexCount,
		FLT_MAX,
		options,
		&resultError);

	float ratio = float(newCount) / float(localIndices.size());
	if (ratio > buildArgs.settings.SimplificationFailurePercentage)
	{
		if (buildArgs.settings.bSimplifyFallbackSloppy)
		{
			float sloppyError = 0.0f;
			size_t sloppyCount = meshopt_simplifySloppy(
				simplifiedLocal.data(),
				localIndices.data(), localIndices.size(),
				localPositions.data(), localVertexCount, sizeof(float) * 3,
				localLocks.empty() ? nullptr : localLocks.data(),
				targetIndexCount,
				FLT_MAX,
				&sloppyError);
			ratio = float(sloppyCount) / float(localIndices.size());
			if (ratio <= buildArgs.settings.SimplificationSloppyFailurePercentage)
			{
				float errorScale = meshopt_simplifyScale(localPositions.data(), localVertexCount, sizeof(float) * 3);
				newCount = sloppyCount;
				resultError = sloppyError * errorScale * buildArgs.settings.SimplifySloppyErrorFactor;
			}
			else
			{
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	simplifiedLocal.resize(newCount);

	outError = resultError;

	// Remap simplified local indices back to global indices
	// BuildMeshletsFromIndices needs the original vertex stream, so we must provide global indices
	// Converted via usedGlobalIndices[localIdx]
	std::vector<uint32_t> simplifiedGlobal(newCount);
	for (size_t i = 0; i < newCount; ++i)
	{
		uint32_t localIdx = simplifiedLocal[i];
		// Ensure safe access
		if (localIdx < usedGlobalIndices.size())
			simplifiedGlobal[i] = usedGlobalIndices[localIdx];
		else
			ASSERT(false, "Local index out of bounds in SimplifyGroup");
	}

	// Rebuild meshlets using global indices
	BuildMeshletsFromIndices(buildArgs, simplifiedGlobal.data(), simplifiedGlobal.size(), outNewMeshlets);
	for (auto& ml : outNewMeshlets)
	{
		ml.RefineGroupID = g.GroupID;
	}

	return !outNewMeshlets.empty();
}

// Build hierarchy internal nodes, returning a flattened node array with 0 as root
// [group header] -- [meshlet headers] -- [index buffers] -- [vertex buffers]
GroupPackage MeshletBuilder::SerializeGroup(
	const MeshletBuildArgs& buildArgs,
	const Group& group,
	const std::vector<Meshlet>& meshlets,
	const std::unordered_map<uint32_t, uint32_t>& groupIdToOrder)
{
	GroupPackage package;
	if (meshlets.empty())
		return package;

	const uint32_t count = static_cast<uint32_t>(group.MeshletIDs.size());

	uint32_t indicesTotalBytes = 0;
	uint32_t verticesTotalBytes = 0;

	for (auto mid : group.MeshletIDs)
	{
		const Meshlet& m = meshlets[mid];
		indicesTotalBytes += Math::AlignUp(static_cast<uint32_t>(m.Triangles.size()), 4u);
		verticesTotalBytes += Math::AlignUp(static_cast<uint32_t>(m.Vertices.size() * buildArgs.vertexStride), 4u);
	}

	const uint32_t headerSize = sizeof(GroupHeader);
	const uint32_t meshletHeadsSize = count * sizeof(MeshletHeader);
	const uint32_t totalSize = headerSize + meshletHeadsSize + indicesTotalBytes + verticesTotalBytes;

	package.Blob.resize(totalSize);
	uint8_t* pBase = package.Blob.data();

	// group header
	GroupHeader header;
	header.MeshletCount = count;
	header.ParrentError = group.ParrentError;
	std::memcpy(header.BoundSphere, group.GroupSphere, sizeof(float) * 4);
	std::memcpy(header.BBoxMin, group.GroupBBoxMin, sizeof(float) * 3);
	std::memcpy(header.BBoxMax, group.GroupBBoxMax, sizeof(float) * 3);
	std::memcpy(pBase, &header, sizeof(GroupHeader));

	// meshlet headers
	auto* pHeaders = reinterpret_cast<MeshletHeader*>(pBase + headerSize);
	uint8_t* pIndicesCursor = pBase + headerSize + meshletHeadsSize;
	uint8_t* pVerticesCursor = pIndicesCursor + indicesTotalBytes;

	for (uint32_t i = 0; i < count; ++i)
	{
		const Meshlet& m = meshlets[group.MeshletIDs[i]];

		MeshletHeader& mh = pHeaders[i];
		// Fill meshlet header
		mh.TriangleCountMinusOne = static_cast<uint8_t>(m.Triangles.size() / 3 - 1);
		mh.VertexCountMinusOne = static_cast<uint8_t>(m.Vertices.size() - 1);
		mh.LODLevel = static_cast<uint8_t>(group.LODLevel);
		mh.TriangleOffset = static_cast<uint32_t>(pIndicesCursor - pBase);
		mh.VertexOffset = static_cast<uint32_t>(pVerticesCursor - pBase);
		mh.GroupChildIndex = static_cast<uint8_t>(i);
		mh.MeshBufferIndex = buildArgs.meshBufferIndex;
		mh.MaterialBufferIndex = buildArgs.materialBufferIndex;
		mh.PSOFlags = buildArgs.psoFlags;
		mh.VertexStride = buildArgs.vertexStride;
		mh.RefineGroupIndex = (m.RefineGroupID != 0xFFFFFFFF) ? 
			(groupIdToOrder.at(m.RefineGroupID) + buildArgs.baseGroupIndex) : 0xFFFFFFFF;
		std::memcpy(mh.BBoxMin, m.BBoxMin, sizeof(float) * 3);
		std::memcpy(mh.BBoxMax, m.BBoxMax, sizeof(float) * 3);

		// Copy index data
		const size_t triBytes = m.Triangles.size();
		std::memcpy(pIndicesCursor, m.Triangles.data(), triBytes);
		pIndicesCursor += Math::AlignUp(static_cast<uint32_t>(triBytes), 4u);

		// Copy vertex data
		for (size_t mvIdx = 0; mvIdx < m.Vertices.size(); mvIdx++)
		{
			const unsigned char* srcV = &(buildArgs.VBData[(size_t)m.Vertices[mvIdx] * buildArgs.vertexStride]);
			std::memcpy(pVerticesCursor + buildArgs.vertexStride * mvIdx, srcV, buildArgs.vertexStride);
		}

		pVerticesCursor += Math::AlignUp(static_cast<uint32_t>(m.Vertices.size() * buildArgs.vertexStride), 4u);
	}

	//package.Metadata.OffsetInGlobalBuffer = 0;
	package.Metadata.SizeBytes = static_cast<uint32_t>(package.Blob.size());
    package.Metadata.UncompressedSize = package.Metadata.SizeBytes; // Not compressed yet

	ASSERT((package.Metadata.UncompressedSize % 4u) == 0u, "Group package size must be 4-byte aligned");

	return package;
}

MeshletBuildProducts MeshletBuilder::BuildStreamingData(
	const MeshletBuildArgs& buildArgs,
	const std::vector<Group>& groups,
	const std::vector<Meshlet>& meshlets)
{
	MeshletBuildProducts products;

	if (meshlets.empty())
		return products;

	std::vector<size_t> groupOrder(groups.size());
	std::iota(groupOrder.begin(), groupOrder.end(), size_t(0));
	std::sort(groupOrder.begin(), groupOrder.end(),
		[&](size_t a, size_t b)
		{
			if (groups[a].LODLevel != groups[b].LODLevel)
				return groups[a].LODLevel > groups[b].LODLevel;
			return a < b;
		});

	uint32_t maxLodLevel = 0;
    std::unordered_map<uint32_t, uint32_t> groupIdToOrder; // Map build group id -> serialized group index
	std::unordered_map<uint32_t, std::vector<uint32_t>> lodGroups; // LODLevel -> build group id
	for (size_t i = 0; i < groupOrder.size(); ++i)
	{
		uint32_t buildGroupId = static_cast<uint32_t>(groupOrder[i]);
		groupIdToOrder[buildGroupId] = static_cast<uint32_t>(i);
		lodGroups[groups[buildGroupId].LODLevel].push_back(buildGroupId);
		if (groups[buildGroupId].LODLevel > maxLodLevel)
			maxLodLevel = groups[buildGroupId].LODLevel;
	}

    // Serialize groups
	for (auto idx : groupOrder)
	{
		products.Groups.push_back(SerializeGroup(
			buildArgs, groups[idx], meshlets, groupIdToOrder));
	}

    // ========== Build BVH by levels ==========
    // Use nodes with metadata and compute offsets later
	struct PendingNode
	{
		Renderer::HierarchyNode node;
		int32_t lodLevel;           // LOD level (-1 means the level connecting all LOD BVHs)
		uint32_t localChildStart;   // Child start within this level
		uint32_t childCount;        // Child count
	};

    // Store nodes by LOD level: lodBVHs[lod] = { node array }
	std::vector<std::vector<PendingNode>> lodBVHs(maxLodLevel + 1);
	std::vector<Renderer::HierarchyNode> lodRoots;

	for (int32_t lod = static_cast<int32_t>(maxLodLevel); lod >= 0; --lod)
	{
		// Build leaf nodes
		std::vector<Renderer::HierarchyNode> leafNodes;
		for (uint32_t gid : lodGroups[lod])
		{
			Renderer::HierarchyNode node{};
			std::memcpy(node.BoundSphere, groups[gid].GroupSphere, sizeof(float) * 4);
			std::memcpy(node.BBoxMin, groups[gid].GroupBBoxMin, sizeof(float) * 3);
			std::memcpy(node.BBoxMax, groups[gid].GroupBBoxMax, sizeof(float) * 3);
			node.MaxParrentError = groups[gid].ParrentError;
			node.Leaf.IsGroup = 1;
			node.Leaf.GroupIndex = groupIdToOrder[gid] + buildArgs.baseGroupIndex;
			node.Leaf.MeshletCountMinusOne = static_cast<uint8_t>(
				std::min<size_t>(groups[gid].MeshletIDs.size()-1, 127));
			leafNodes.push_back(node);
		}

		// Build BVH for this level (local offsets baseNodeIndex=0)
		std::vector<uint32_t> reorderedLeafNodesIndices(leafNodes.size());
		auto bvhNodes = BuildHierarchy(leafNodes, buildArgs.settings.MaxBVHNodeChildren, reorderedLeafNodesIndices);
		ASSERT(bvhNodes.size() > 0);

		// Convert to PendingNode
		lodBVHs[lod].reserve(bvhNodes.size());
		for (size_t i = 0; i < bvhNodes.size(); ++i)
		{
			PendingNode pn;
			pn.node = bvhNodes[i];
			pn.lodLevel = lod;
			// Extract local childStart (internal nodes only)
			if (pn.node.Internal.IsGroup == 0)
			{
				pn.localChildStart = pn.node.Internal.ChildStartIndex;
				pn.childCount = pn.node.Internal.ChildCount;
			}
			else
			{
				pn.localChildStart = 0;
				pn.childCount = 0;
			}
			lodBVHs[lod].push_back(pn);
		}

		// Save root node for top-level build
		if (!bvhNodes.empty())
			lodRoots.push_back(bvhNodes[0]);
	}


    // Build top-level BVH (connect LOD roots)
	std::vector<uint32_t> reorderedRootNodesIndices(lodRoots.size());
	auto topBVH = BuildHierarchy(lodRoots, buildArgs.settings.MaxBVHNodeChildren, reorderedRootNodesIndices);

	std::vector<PendingNode> topPending;
	for (size_t i = 0; i < topBVH.size(); ++i)
	{
		PendingNode pn;
		pn.node = topBVH[i];
		pn.lodLevel = -1; // Top-level marker
		if (pn.node.Internal.IsGroup == 0)
		{
			pn.localChildStart = pn.node.Internal.ChildStartIndex;
			pn.childCount = pn.node.Internal.ChildCount;
		}
		else
		{
			pn.localChildStart = 0;
			pn.childCount = 0;
		}
		topPending.push_back(pn);
	}

    // ========== Compute global offsets ==========
    // Layout: [top-level BVH] [LOD_max (skip root)] [LOD_max-1 (skip root)] ... [LOD_0 (skip root)]

	const uint32_t topSize = static_cast<uint32_t>(topPending.size());
	std::vector<uint32_t> lodOffsets(maxLodLevel + 1);
	uint32_t cursor = topSize;

	for (int32_t lod = static_cast<int32_t>(maxLodLevel); lod >= 0; --lod)
	{
		lodOffsets[lod] = cursor;
		// Skip root node (already in top level)
		cursor += static_cast<uint32_t>(lodBVHs[lod].size() - 1);
	}

	products.Hierarchy.resize(cursor);

    // ========== Write top-level BVH ==========
    // Top-level leaf nodes (original LOD roots) need ChildStart patched
	for (uint32_t i = 0; i < topSize; ++i)
	{
		auto& pn = topPending[i];
		Renderer::HierarchyNode node = pn.node;

		// Top-level leaf nodes are original LOD roots and must become internal nodes pointing to that LOD's children
		if (i >= topSize - lodRoots.size())
		{
			int32_t lod = static_cast<int32_t>(maxLodLevel) -
				static_cast<int32_t>(reorderedRootNodesIndices[i - (topSize - lodRoots.size())]);
			if (lod >= 0 && lodBVHs[lod].size() > 1)
			{
				// Child info for that LOD root
				const auto& lodRoot = lodBVHs[lod][0];
				node.Internal.IsGroup = 0;
				node.Internal.ChildCount = lodRoot.childCount;
				node.Internal.ChildStartIndex = lodOffsets[lod] + buildArgs.baseNodeIndex;
			}
			else
			{
				ASSERT(false, "Invalid LOD root in top-level BVH");
			}
		}
		else if (pn.childCount > 0)
		{
			// Top-level internal node, patch offset
			node.Internal.ChildStartIndex = pn.localChildStart + buildArgs.baseNodeIndex;
		}

		products.Hierarchy[i] = node;
	}

    // ========== Write each LOD level (skip root) ==========
	for (int32_t lod = static_cast<int32_t>(maxLodLevel); lod >= 0; --lod)
	{
		const auto& bvh = lodBVHs[lod];
		if (bvh.size() <= 1)
			continue;

		const uint32_t baseOffset = lodOffsets[lod];

		for (size_t i = 1; i < bvh.size(); ++i) // Start from 1, skip root
		{
			const auto& pn = bvh[i];
			Renderer::HierarchyNode node = pn.node;
			if (pn.childCount > 0 && node.Internal.IsGroup == 0)
			{
				// Patch internal node offset: local offset - 1 (skip root) + level base
				node.Internal.ChildStartIndex = baseOffset + (pn.localChildStart - 1) + buildArgs.baseNodeIndex;
			}
			products.Hierarchy[baseOffset + (i - 1)] = node;
		}
	}

	if (buildArgs.settings.bOutputDebugInfo)
	{
		// ========== Print stats ==========
		Utility::Printf(L"[BuildStreamingData] Summary:\n");
		Utility::Printf(L"Total Groups: %u\n", static_cast<uint32_t>(products.Groups.size()));
		Utility::Printf(L"Total BVH Nodes: %u\n", static_cast<uint32_t>(products.Hierarchy.size()));
		Utility::Printf(L"Top-level BVH Nodes: %u\n", topSize);
		Utility::Printf(L"LOD Levels: %u\n", maxLodLevel + 1);
	}

	if (buildArgs.settings.bOutputDebugInfo)
	{
		// Stats by LOD level
		for (uint32_t lod = 0; lod <= maxLodLevel; ++lod)
		{
			uint32_t groupCount = static_cast<uint32_t>(lodGroups[lod].size());
			uint32_t bvhCount = static_cast<uint32_t>(lodBVHs[lod].size());
			Utility::Printf(L"  LOD %u: %u groups, %u BVH nodes\n", lod, groupCount, bvhCount);
		}
	}

	if (buildArgs.settings.bOutputDebugInfo)
	{
		// -------------------------------------------------------
		// Dump BVH node contents
		// -------------------------------------------------------
		Utility::Printf(L"--- BVH Nodes Dump (%u nodes) ---\n", static_cast<uint32_t>(products.Hierarchy.size()));
		for (size_t i = 0; i < products.Hierarchy.size(); ++i)
		{
			const auto& node = products.Hierarchy[i];

			// Check IsGroup flag (bitfield shared in union; access Internal.IsGroup)
			if (node.Internal.IsGroup)
			{
				// Is Group node (Leaf)
				// ChildStartIndex/ChildCount are invalid here; print GroupIndex, etc.
				Utility::Printf(L"Node[%u]: IsGroup=YES (Leaf), GroupIndex=%u, MeshletCount=%u, Bounds=(%.3f, %.3f, %.3f, %.3f), Error=%.5f\n",
					static_cast<uint32_t>(i),
					node.Leaf.GroupIndex,
					node.Leaf.MeshletCountMinusOne + 1,
					node.BoundSphere[0], node.BoundSphere[1], node.BoundSphere[2], node.BoundSphere[3],
					node.MaxParrentError);
			}
			else
			{
				// Is internal node
				Utility::Printf(L"Node[%u]: IsGroup=NO,  ChildCount=%u, ChildStartIndex=%u, Bounds=(%.3f, %.3f, %.3f, %.3f), Error=%.5f\n",
					static_cast<uint32_t>(i),
					node.Internal.ChildCount,
					node.Internal.ChildStartIndex,
					node.BoundSphere[0], node.BoundSphere[1], node.BoundSphere[2], node.BoundSphere[3],
					node.MaxParrentError);
			}
		}
		Utility::Printf(L"-----------------------------------\n");
	}

	return products;
}

bool MeshletBuilder::ValidateBuild(
	const MeshletBuildArgs& buildArgs,
	const std::vector<Group>& groups,
	const std::vector<Meshlet>& meshlets)
{
	const uint32_t invalidId = 0xFFFFFFFFu;
	const size_t groupCount = groups.size();
	bool ok = true;
	uint32_t errorCount = 0;

	auto Report = [&](const wchar_t* fmt, auto... args)
	{
		if (errorCount < 64)
			Utility::Printf(fmt, args...);
		errorCount++;
		ok = false;
	};

	if (groupCount == 0 || meshlets.empty())
	{
		Report(L"[MeshletBuilder][Validate] Empty groups or meshlets (groups=%u, meshlets=%u)\n",
			static_cast<uint32_t>(groupCount), static_cast<uint32_t>(meshlets.size()));
		return false;
	}

	std::vector<uint32_t> refineRef(groupCount, 0);

	// Meshlet -> group/refine validation
	for (size_t mi = 0; mi < meshlets.size(); ++mi)
	{
		const Meshlet& m = meshlets[mi];
		if (m.GroupID >= groupCount)
		{
			Report(L"[MeshletBuilder][Validate] Meshlet %u has invalid GroupID=%u (groups=%u)\n",
				static_cast<uint32_t>(mi), m.GroupID, static_cast<uint32_t>(groupCount));
			continue;
		}

		if (m.RefineGroupID != invalidId)
		{
			if (m.RefineGroupID >= groupCount)
			{
				Report(L"[MeshletBuilder][Validate] Meshlet %u has invalid RefineGroupID=%u (groups=%u)\n",
					static_cast<uint32_t>(mi), m.RefineGroupID, static_cast<uint32_t>(groupCount));
			}
			else
			{
				refineRef[m.RefineGroupID]++;
			}
		}
	}

	// Group -> meshlet ownership and basic invariants
	for (size_t gi = 0; gi < groupCount; ++gi)
	{
		const Group& g = groups[gi];
		if (g.MeshletIDs.empty())
		{
			Report(L"[MeshletBuilder][Validate] Group %u has no meshlets\n", static_cast<uint32_t>(gi));
			continue;
		}

		for (uint32_t mid : g.MeshletIDs)
		{
			if (mid >= meshlets.size())
			{
				Report(L"[MeshletBuilder][Validate] Group %u references out-of-range meshlet %u (meshlets=%u)\n",
					static_cast<uint32_t>(gi), mid, static_cast<uint32_t>(meshlets.size()));
				continue;
			}
			if (meshlets[mid].GroupID != gi)
			{
				Report(L"[MeshletBuilder][Validate] Meshlet %u GroupID mismatch (meshlet.GroupID=%u, expected=%u)\n",
					mid, meshlets[mid].GroupID, static_cast<uint32_t>(gi));
			}
		}

		if (std::isinf(g.ParrentError) && refineRef[gi] > 0)
		{
			Report(L"[MeshletBuilder][Validate] Group %u has infinite error but is referenced by %u coarse meshlets\n",
				static_cast<uint32_t>(gi), refineRef[gi]);
		}
	}

	// LOD monotonic + continuity checks
	for (size_t mi = 0; mi < meshlets.size(); ++mi)
	{
		const Meshlet& m = meshlets[mi];
		if (m.RefineGroupID == invalidId || m.RefineGroupID >= groupCount || m.GroupID >= groupCount)
			continue;

		const Group& coarse = groups[m.GroupID];
		const Group& fine = groups[m.RefineGroupID];

		if (coarse.LODLevel <= fine.LODLevel)
		{
			Report(L"[MeshletBuilder][Validate] LOD order invalid: meshlet %u coarse LOD=%u, refine LOD=%u\n",
				static_cast<uint32_t>(mi), coarse.LODLevel, fine.LODLevel);
		}

		if (!std::isfinite(fine.ParrentError))
		{
			Report(L"[MeshletBuilder][Validate] Meshlet %u references refine group %u with non-finite error\n",
				static_cast<uint32_t>(mi), m.RefineGroupID);
		}

		if (coarse.ParrentError + kValidateEpsilon < fine.ParrentError)
		{
			Report(L"[MeshletBuilder][Validate] Error not monotonic: meshlet %u coarseErr=%.6f < refineErr=%.6f\n",
				static_cast<uint32_t>(mi), coarse.ParrentError, fine.ParrentError);
		}

		// Ensure culling bounds are conservative vs refine group bounds
		if (!SphereContains(m.BoundSphere, fine.GroupSphere, kValidateEpsilon))
		{
			Report(L"[MeshletBuilder][Validate] Bounds not conservative: meshlet %u bound does not enclose refine group %u\n",
				static_cast<uint32_t>(mi), m.RefineGroupID);
		}
	}

	if (buildArgs.settings.bOutputDebugInfo)
	{
		Utility::Printf(L"[MeshletBuilder][Validate] Done. groups=%u meshlets=%u errors=%u\n",
			static_cast<uint32_t>(groupCount), static_cast<uint32_t>(meshlets.size()), errorCount);
	}

	return ok;
}

std::vector<Renderer::HierarchyNode> MeshletBuilder::BuildHierarchy(
	const std::vector<Renderer::HierarchyNode>& initNodes,
	uint32_t maxBVHNodeChildren,
	std::vector<uint32_t>& outInitNodesReorderMap)
{
	if (initNodes.empty())
		return {};

    // Handle single-node case: if it's a group node, create a parent
	if (initNodes.size() == 1)
	{
		outInitNodesReorderMap[0] = 0;
		if (initNodes[0].Internal.IsGroup == 1)
		{
			// Create a parent for a single group node
			HierarchyNode parent{};
			parent.Internal.IsGroup = 0;
			parent.Internal.ChildCount = 1;
			parent.Internal.ChildStartIndex = 1; // Child node at index 1
			std::copy(initNodes[0].BoundSphere, initNodes[0].BoundSphere + 4, parent.BoundSphere);
			std::copy(initNodes[0].BBoxMin, initNodes[0].BBoxMin + 3, parent.BBoxMin);
			std::copy(initNodes[0].BBoxMax, initNodes[0].BBoxMax + 3, parent.BBoxMax);
			parent.MaxParrentError = initNodes[0].MaxParrentError;

			// Return [parent, child]
			std::vector<HierarchyNode> result(2);
			result[0] = parent;
			result[1] = initNodes[0];
			return result;
		}
		return initNodes;
	}

	const uint32_t maxChildren = maxBVHNodeChildren;

    // levels[0] = leaf level, levels[N] = root level
	std::vector<std::vector<HierarchyNode>> levels(1);

    // Initialize leaf level
	levels[0] = initNodes;//std::move(initNodes);

    // Build bottom-up
	while (levels.back().size() > 1)
	{
		const auto& currentLevel = levels.back();
		const size_t nodeCount = currentLevel.size();

		// Extract sphere centers for spatial clustering
		std::vector<float> centers(nodeCount * 3);
		for (size_t i = 0; i < nodeCount; ++i)
		{
			centers[i * 3 + 0] = currentLevel[i].BoundSphere[0];
			centers[i * 3 + 1] = currentLevel[i].BoundSphere[1];
			centers[i * 3 + 2] = currentLevel[i].BoundSphere[2];
		}

		// Spatial clustering
		std::vector<unsigned int> clusterIndices(nodeCount);
		meshopt_spatialClusterPoints(
			clusterIndices.data(),
			centers.data(),
			nodeCount,
			sizeof(float) * 3,
			maxChildren);

		if (levels.size() == 1)
		{
			// Output leaf-level reorder mapping
			for (size_t i = 0; i < nodeCount; ++i)
			{
				outInitNodesReorderMap[i] = clusterIndices[i];
			}
		}

		uint32_t clusterCount = (static_cast<uint32_t>(nodeCount) + maxChildren - 1) / maxChildren;


		// Count nodes per cluster
		std::vector<uint32_t> clusterSizes(clusterCount, maxChildren);
		if (nodeCount % maxChildren != 0)
			clusterSizes.back() = nodeCount % maxChildren;

		// Compute start offset of each cluster in reorder array
		std::vector<uint32_t> clusterStarts(clusterCount + 1, 0);
		for (size_t i = 0; i < clusterCount; ++i)
			clusterStarts[i + 1] = clusterStarts[i] + clusterSizes[i];

		// Reorder current level nodes by cluster (in-place to temp array)
		std::vector<HierarchyNode> sortedLevel(nodeCount);
		for (size_t i = 0; i < nodeCount; ++i)
		{
			sortedLevel[i] = currentLevel[clusterIndices[i]];
		}

		// Replace current level with sorted version
		levels.back() = std::move(sortedLevel);

		// Build parent level
		std::vector<HierarchyNode> parentLevel;
		parentLevel.reserve(clusterCount);

		for (size_t c = 0; c < clusterCount; ++c)
		{
			if (clusterSizes[c] == 0)
				continue;

			HierarchyNode parent{};
			parent.Internal.IsGroup = 0;
			parent.Internal.ChildCount = clusterSizes[c];
			parent.Internal.ChildStartIndex = clusterStarts[c];

			// Merge bounding spheres and error
			const auto& sortedCurrent = levels.back();
			float mergedSphere[4];
			std::copy(sortedCurrent[clusterStarts[c]].BoundSphere,
				sortedCurrent[clusterStarts[c]].BoundSphere + 4,
				mergedSphere);
			float mergedMin[3];
			float mergedMax[3];
			std::copy(sortedCurrent[clusterStarts[c]].BBoxMin,
				sortedCurrent[clusterStarts[c]].BBoxMin + 3,
				mergedMin);
			std::copy(sortedCurrent[clusterStarts[c]].BBoxMax,
				sortedCurrent[clusterStarts[c]].BBoxMax + 3,
				mergedMax);
			float maxError = sortedCurrent[clusterStarts[c]].MaxParrentError;

			for (uint32_t j = 1; j < clusterSizes[c]; ++j)
			{
				const auto& child = sortedCurrent[clusterStarts[c] + j];
				MergeSphere(mergedSphere, child.BoundSphere, mergedSphere);
				MergeAABB(mergedMin, mergedMax, child.BBoxMin, child.BBoxMax, mergedMin, mergedMax);
				maxError = std::max(maxError, child.MaxParrentError);
			}

			std::copy(mergedSphere, mergedSphere + 4, parent.BoundSphere);
			std::copy(mergedMin, mergedMin + 3, parent.BBoxMin);
			std::copy(mergedMax, mergedMax + 3, parent.BBoxMax);
			parent.MaxParrentError = maxError;

			parentLevel.push_back(std::move(parent));
		}

		levels.push_back(std::move(parentLevel));
	}

    // Flatten: order from root to leaf
	std::vector<size_t> levelOffsets(levels.size());
	size_t totalNodes = 0;
	for (int32_t lvl = static_cast<int32_t>(levels.size()) - 1; lvl >= 0; --lvl)
	{
		levelOffsets[lvl] = totalNodes;
		totalNodes += levels[lvl].size();
	}

	std::vector<Renderer::HierarchyNode> result(totalNodes);

	for (int32_t lvl = static_cast<int32_t>(levels.size()) - 1; lvl >= 0; --lvl)
	{
		const size_t baseOffset = levelOffsets[lvl];
		const size_t childLevelOffset = (lvl > 0) ? levelOffsets[lvl - 1] : 0;

		for (size_t i = 0; i < levels[lvl].size(); ++i)
		{
			auto& nwc = levels[lvl][i];
			Renderer::HierarchyNode node = nwc;
			// Set ChildStartIndex (internal nodes only)
			if (nwc.Internal.ChildCount > 0 && node.Internal.IsGroup == 0)
			{
				node.Internal.ChildStartIndex = static_cast<uint32_t>(
					childLevelOffset + nwc.Internal.ChildStartIndex);
			}

			result[baseOffset + i] = node;
		}
	}

	return result;
}
