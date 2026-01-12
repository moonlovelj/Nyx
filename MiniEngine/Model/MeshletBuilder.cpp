#include "MeshletBuilder.h"
#include "pch.h"
#include <unordered_map>
#include <numeric>
#include <cassert>
#include <cfloat>
#include <execution> 
#include "../Core/Utility.h"
#include "Model.h"

using namespace Renderer;

static constexpr float kInfinity = std::numeric_limits<float>::infinity();

static constexpr uint32_t kMaxHierarchyChildren = 255;

MeshletBuildProducts MeshletBuilder::Build(
	const MeshletBuildArgs& buildArgs)
{
	if (buildArgs.indices.empty() || buildArgs.vertices.empty())
		return {};

	Utility::Printf(L"[MeshletBuilder] Start building meshlets for meshBufferIndex=%u, materialBufferIndex=%u\n",
		buildArgs.meshBufferIndex, buildArgs.materialBufferIndex);

	// 生成 position-only remap（供后续锁边用）
	std::vector<uint32_t> posRemap(buildArgs.vertices.size());
	GeneratePositionRemap(buildArgs.vertices, posRemap);

	// LOD0：整网格 -> meshlet
	std::vector<Meshlet> current = BuildLOD0Meshlets(buildArgs.vertices, buildArgs.indices, buildArgs.settings);

	// LOD0：初始化待简化队列
	std::vector<uint32_t> activeIds(current.size());
	std::iota(activeIds.begin(), activeIds.end(), 0u);

	uint32_t lastTriCount = 0;
	// 打印 LOD0 统计
	{
		uint32_t triCount0 = 0;
		for (const auto& m : current) triCount0 += static_cast<uint32_t>(m.Triangles.size() / 3);
		Utility::Printf(L"[MeshletBuilder] LOD %u: meshlets=%u, triangles=%u\n",
			0u, static_cast<uint32_t>(current.size()), triCount0);
		lastTriCount = triCount0;
	}

	std::vector<Group> currentGroups;

	// 迭代简化
	uint32_t lodLevel = 1;
	while (true)
	{
		// 基于共享顶点 / 空间接近度分组
		auto groupIds= GroupMeshlets(current, activeIds, buildArgs.vertices, buildArgs.settings.TargetMeshletsPerGroup, lodLevel-1, currentGroups);
		if (groupIds.empty())
			break;

		for (auto gid : groupIds)
		{
			for (auto mid : currentGroups[gid].MeshletIDs)
			{
				// 设置 meshlet 的 GroupID
				current[mid].GroupID = gid;
				current[mid].GroupChildIndex = mid;
			}
		}

		// 构建顶点锁（跨组共享 position-only 顶点全部上锁）
		std::vector<unsigned char> vertexLock(buildArgs.vertices.size(), 0);
		BuildVertexLocksByGroups(currentGroups, groupIds, current, posRemap, buildArgs.vertices.size(), vertexLock);

		// 对每个组尝试简化 -> 生成下一层 meshlets
		struct GroupResult
		{
			bool simplified = false;
			float err = 0.0f;                     // SimplifyGroup 返回的误差
			float groupSphere[4]{};
			std::vector<Meshlet> generated;   // 该组生成的新 meshlets
			std::vector<uint32_t> originalIds;    // 该组原有 meshlet id（用于写回 ParentError/Bounds）
			uint32_t groupID = 0;				  // 原有group id	
		};

		std::vector<GroupResult> results(groupIds.size());
		std::vector<size_t> gi(groupIds.size());
		std::iota(gi.begin(), gi.end(), size_t(0));

		// 并行执行每个分组的简化尝试（只产生局部结果，不改写 shared 状态）
		std::for_each(std::execution::par, gi.begin(), gi.end(),
			[&](size_t idx)
			{
				const Group& g = currentGroups[groupIds[idx]];
				GroupResult r;
				r.groupID = g.GroupID;
				std::copy(g.GroupSphere, g.GroupSphere + 4, r.groupSphere);
				r.originalIds = g.MeshletIDs;

				// 单个 meshlet 分组无需简化
				if (g.MeshletIDs.size() <= 1)
				{
					r.simplified = false;
					results[idx] = std::move(r);
					return;
				}

				float err = 0.0f;
				std::vector<Meshlet> generated;
				if (SimplifyGroup(g, current, buildArgs.vertices, buildArgs, vertexLock, generated, err))
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

		// 串行：设置误差、包围球、LODLevel，更新父误差，构建 nextActive
		for (const auto& r : results)
		{
			if (!r.simplified)
			{
				// 未简化成功，保留原 meshlet
				currentGroups[r.groupID].ParrentError = kInfinity;
				continue;
			}

			const size_t baseOffset = newMeshlets.size();
			// 写包围球
			for (auto& nm : const_cast<std::vector<Meshlet>&>(r.generated))
			{
				Meshlet out = std::move(nm);
				ComputeMeshletSphere(out, buildArgs.vertices, out.BoundSphere);
				newMeshlets.emplace_back(std::move(out));
			}

			// 新 meshlet 的全局索引：current.size() + baseOffset + j
			for (size_t j = 0; j < r.generated.size(); ++j)
				nextActive.push_back(static_cast<uint32_t>(current.size() + baseOffset + j));

			// 写回父误差/包围（仅在成功简化时）
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

		// 打印本层统计
		{
			uint32_t triCount = 0;
			for (const auto& m : newMeshlets) triCount += static_cast<uint32_t>(m.Triangles.size() / 3);
			float reduction = 1.0f - float(triCount) / float(lastTriCount);
			Utility::Printf(L"[MeshletBuilder] LOD %u: meshlets=%u, triangles=%u\n",
				lodLevel, static_cast<uint32_t>(newMeshlets.size()), triCount);
			lastTriCount = triCount;
			if (reduction < buildArgs.settings.minReductionRatio)
			{
				Utility::Printf(L"[MeshletBuilder]  Simplification reduction %.2f%% below threshold %.2f%%, stopping.\n",
					reduction * 100.0f, buildArgs.settings.minReductionRatio * 100.0f);
				break;
			}
		}

		// 追加到 current，记录 LOD 层与当层可见 meshlet
		current.insert(current.end(),
			std::make_move_iterator(newMeshlets.begin()),
			std::make_move_iterator(newMeshlets.end()));

		++lodLevel;
		activeIds.swap(nextActive);
	}

	return BuildStreamingData(buildArgs, currentGroups, current);
}

std::vector<MeshletBuilder::Meshlet>
MeshletBuilder::BuildLOD0Meshlets(
	std::span<const RawVertex> vertices,
	std::span<const uint32_t> indices,
	const MeshletBuildSettings& s)
{
	std::vector<Meshlet> out;
	if (indices.empty())
		return out;

	BuildMeshletsFromIndices(indices.data(), indices.size(), vertices, s, out);

	// LOD0 误差 = 0
	for (auto& m : out)
	{
		ComputeMeshletSphere(m, vertices, m.BoundSphere);
	}
	return out;
}

void MeshletBuilder::BuildMeshletsFromIndices(
	const uint32_t* indices, size_t indexCount,
	std::span<const RawVertex> vertices,
	const MeshletBuildSettings& s,
	std::vector<Meshlet>& out)
{
	const float* pos = reinterpret_cast<const float*>(&vertices[0].Position);
	const size_t stride = sizeof(RawVertex);

	// 申请上界缓冲
	size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, s.MaxMeshletVertices, s.MaxMeshletTriangles);
	std::vector<meshopt_Meshlet> mlets(maxMeshlets);
	std::vector<uint32_t> mlVertices(indexCount); 
	std::vector<unsigned char> mlTriangles(indexCount);

	size_t mlCount = meshopt_buildMeshlets(
		mlets.data(), mlVertices.data(), mlTriangles.data(),
		indices, indexCount,
		pos, vertices.size(), stride,
		s.MaxMeshletVertices, s.MaxMeshletTriangles, 0.0f);

	// 转为 TempMeshlet
	out.reserve(out.size() + mlCount);
	for (size_t i = 0; i < mlCount; ++i)
	{
		const auto& ml = mlets[i];

		Meshlet tm;

		tm.Vertices.resize(ml.vertex_count);
		std::copy_n(mlVertices.data() + ml.vertex_offset, ml.vertex_count, tm.Vertices.begin());

		tm.Triangles.resize(ml.triangle_count * 3);
		std::copy_n(mlTriangles.data() + ml.triangle_offset, ml.triangle_count * 3, tm.Triangles.begin());

		// 可选优化局部性
		meshopt_optimizeMeshlet(
			tm.Vertices.data(), tm.Triangles.data(),
			ml.triangle_count, ml.vertex_count);

		out.emplace_back(std::move(tm));
	}
}

void MeshletBuilder::ComputeMeshletSphere(
	const Meshlet& m,
	std::span<const RawVertex> vertices,
	float outSphere[4])
{
	const float* pos = reinterpret_cast<const float*>(&vertices[0].Position);
	const size_t stride = sizeof(RawVertex);

	meshopt_Bounds b = meshopt_computeMeshletBounds(
		m.Vertices.data(), m.Triangles.data(),
		m.Triangles.size() / 3,
		pos, vertices.size(), stride);

	outSphere[0] = b.center[0];
	outSphere[1] = b.center[1];
	outSphere[2] = b.center[2];
	outSphere[3] = b.radius;
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

void MeshletBuilder::GeneratePositionRemap(
	std::span<const RawVertex> vertices,
	std::vector<uint32_t>& outPosRemap)
{
	const float* pos = reinterpret_cast<const float*>(&vertices[0].Position);
	const size_t stride = sizeof(RawVertex);
	meshopt_generatePositionRemap(outPosRemap.data(), pos, vertices.size(), stride);
}

std::vector<uint32_t> MeshletBuilder::GroupMeshlets(
	const std::vector<Meshlet>& current,
	std::span<const uint32_t> subsetIds,
	std::span<const RawVertex> vertices,
	uint32_t targetGroupSize,
	uint32_t LODLevel,
	std::vector<Group>& groups)
{
	const size_t clusterCount = subsetIds.size();
	if (clusterCount == 0) return {};

	// 准备给 partitionClusters 的输入
	// 把每个 meshlet 的唯一顶点表串接起来
	// 统计展开后总索引数
	size_t totalIndexCount = 0;
	for (uint32_t id : subsetIds)
		totalIndexCount += current[id].Triangles.size();

	std::vector<uint32_t> clusterIndices;
	clusterIndices.reserve(totalIndexCount);
	std::vector<unsigned int> clusterCounts(clusterCount);

	// 将每个 meshlet 的 micro-triangles 展开为原始顶点索引
	for (size_t i = 0; i < clusterCount; ++i)
	{
		const auto& m = current[subsetIds[i]];
		const size_t triIdxCount = m.Triangles.size(); // 3 * triangle_count
		clusterCounts[i] = static_cast<unsigned int>(triIdxCount);

		for (size_t t = 0; t < triIdxCount; ++t)
		{
			uint8_t local = m.Triangles[t];           // 局部 0..N-1
			uint32_t original = m.Vertices[local];    // 原始顶点索引
			clusterIndices.push_back(original);
		}
	}

	std::vector<unsigned int> partition(clusterCount);
	const float* pos = reinterpret_cast<const float*>(&vertices[0].Position);
	const size_t stride = sizeof(RawVertex);

	size_t partCount = meshopt_partitionClusters(
		partition.data(),
		clusterIndices.data(), clusterIndices.size(),
		clusterCounts.data(), clusterCount,
		pos, vertices.size(), stride,
		targetGroupSize);

	// 聚合为组
	std::vector<Group> newGroups(partCount);
	for (size_t i = 0; i < clusterCount; ++i)
		newGroups[partition[i]].MeshletIDs.push_back(subsetIds[i]);

	std::vector<uint32_t> groupIds(partCount);
	// 计算组球（合并各 meshlet 球）
	for (size_t groupIndex = 0; groupIndex < newGroups.size(); groupIndex++)
	{
		auto& g = newGroups[groupIndex];
		bool first = true;
		float acc[4]{};
		for (uint32_t mid : g.MeshletIDs)
		{
			float bs[4];
			ComputeMeshletSphere(current[mid], vertices, bs);
			if (first) { std::copy(bs, bs + 4, acc); first = false; }
			else MergeSphere(acc, bs, acc);
		}
		std::copy(acc, acc + 4, g.GroupSphere);

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
	// 标记每个 position-only 顶点首次被哪个组占用
	// 若同一 position 顶点被不同组占用 -> 锁定该 position
	// 最后把对应的所有原始顶点（拥有该 position id）设为 lock=1
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

	// 写回到原始顶点锁
	for (size_t v = 0; v < vertexCount; ++v)
	{
		outVertexLock[v] = lockedPos[posRemap[v]] ? 1 : 0;
	}
}

bool MeshletBuilder::SimplifyGroup(
	const Group& g,
	const std::vector<Meshlet>& current,
	std::span<const RawVertex> vertices,
	const MeshletBuildArgs& buildArgs,
	const std::vector<unsigned char>& vertexLock,
	std::vector<Meshlet>& outNewMeshlets,
	float& outError)
{
	// 收集组内用到的所有唯一顶点索引，构建 Global->Local 映射
	// 预估最大顶点数 = Meshlet数 * MaxVerts
	// 这里使用 Vector + Sort + Unique 比 unordered_map 快且省内存
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

	// Global Index -> Local Index 的查找表 (因为 global index 很大，无法用直接数组，这里用二分查找代替 map)
	auto GetLocalIndex = [&](uint32_t globalIdx) -> uint32_t {
		auto it = std::lower_bound(usedGlobalIndices.begin(), usedGlobalIndices.end(), globalIdx);
		if (it != usedGlobalIndices.end() && *it == globalIdx)
			return static_cast<uint32_t>(std::distance(usedGlobalIndices.begin(), it));
		return 0xFFFFFFFF;
		};

	// 构建局部顶点/索引数据
	// 拼接组三角（使用 Local 索引）
	std::vector<uint32_t> localIndices;
	localIndices.reserve(g.MeshletIDs.size() * buildArgs.settings.MaxMeshletTriangles * 3);

	for (uint32_t mid : g.MeshletIDs)
	{
		const auto& m = current[mid];
		for (size_t t = 0; t < m.Triangles.size(); t += 3)
		{
			uint32_t g0 = m.Vertices[m.Triangles[t + 0]];
			uint32_t g1 = m.Vertices[m.Triangles[t + 1]];
			uint32_t g2 = m.Vertices[m.Triangles[t + 2]];
			localIndices.push_back(GetLocalIndex(g0));
			localIndices.push_back(GetLocalIndex(g1));
			localIndices.push_back(GetLocalIndex(g2));
		}
	}

	if (localIndices.empty())
		return false;

	// 准备局部属性缓冲 (Position + Attributes)
	size_t localVertexCount = usedGlobalIndices.size();

	std::vector<float> localPositions(localVertexCount * 3);

	uint32_t attributeValueCount = 3; // Normal
	if (buildArgs.psoFlags & PSOFlags::kHasTangent) attributeValueCount += 4;
	if (buildArgs.psoFlags & PSOFlags::kHasUV0) attributeValueCount += 2;

	std::vector<float> localAttrs(localVertexCount * attributeValueCount);

	for (size_t i = 0; i < localVertexCount; ++i)
	{
		uint32_t globalIdx = usedGlobalIndices[i];
		const RawVertex& v = vertices[globalIdx];

		// Position
		localPositions[i * 3 + 0] = v.Position.GetX();
		localPositions[i * 3 + 1] = v.Position.GetY();
		localPositions[i * 3 + 2] = v.Position.GetZ();

		// Attributes
		size_t attrBase = i * attributeValueCount;
		size_t attrOffset = 0;

		localAttrs[attrBase + attrOffset++] = v.Normal.GetX();
		localAttrs[attrBase + attrOffset++] = v.Normal.GetY();
		localAttrs[attrBase + attrOffset++] = v.Normal.GetZ();

		if (buildArgs.psoFlags & PSOFlags::kHasTangent)
		{
			localAttrs[attrBase + attrOffset++] = v.Tangent.GetX();
			localAttrs[attrBase + attrOffset++] = v.Tangent.GetY();
			localAttrs[attrBase + attrOffset++] = v.Tangent.GetZ();
			localAttrs[attrBase + attrOffset++] = v.Tangent.GetW();
		}
		if (buildArgs.psoFlags & PSOFlags::kHasUV0)
		{
			localAttrs[attrBase + attrOffset++] = v.UV0[0];
			localAttrs[attrBase + attrOffset++] = v.UV0[1];
		}
	}

	std::vector<float> attributeWeights(attributeValueCount, 0.5f);
	if (buildArgs.psoFlags & PSOFlags::kHasTangent) attributeWeights[6] = 1.f;

	// 处理 Lock 数组 (局部化)
	std::vector<unsigned char> localLocks;
	if (!vertexLock.empty())
	{
		localLocks.resize(localVertexCount);
		for (size_t i = 0; i < localVertexCount; ++i)
		{
			localLocks[i] = vertexLock[usedGlobalIndices[i]];
		}
	}

	// 执行简化 (使用局部数据)
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

	simplifiedLocal.resize(newCount);

	// 简化失败判定
	float ratio = float(newCount) / float(localIndices.size());
	if (ratio > buildArgs.settings.SimplificationFailurePercentage)
		return false;

	outError = resultError;

	// 将简化后的 Local 索引 remap 回 Global 索引
	// BuildMeshletsFromIndices 需要原始顶点数据流，所以我们要给它 global indices
	// 这里通过 usedGlobalIndices[localIdx] 转换
	std::vector<uint32_t> simplifiedGlobal(newCount);
	for (size_t i = 0; i < newCount; ++i)
	{
		uint32_t localIdx = simplifiedLocal[i];
		// 确保安全访问
		if (localIdx < usedGlobalIndices.size())
			simplifiedGlobal[i] = usedGlobalIndices[localIdx];
		else
			ASSERT(false, "Local index out of bounds in SimplifyGroup");
	}

	// 用全局索引重建 meshlet
	BuildMeshletsFromIndices(simplifiedGlobal.data(), simplifiedGlobal.size(), vertices, buildArgs.settings, outNewMeshlets);
	for (auto& ml : outNewMeshlets)
	{
		ml.RefineGroupID = g.GroupID;
	}

	return !outNewMeshlets.empty();
}

// 构建层次结构内部节点，返回的是扁平化的节点数组，0为根节点
// [group header] -- [meshlet headers] -- [index buffers] -- [vertex buffers]
GroupPackage MeshletBuilder::SerializeGroup(
	const MeshletBuildArgs& buildArgs,
	const Group& group,
	const std::vector<Meshlet>& meshlets,
	const std::unordered_map<uint32_t, uint32_t>& groupIdToOrder)
{
	GroupPackage package;
	if (meshlets.empty() || buildArgs.vertices.empty())
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
	std::memcpy(pBase, &header, sizeof(GroupHeader));

	// meshlet headers
	auto* pHeaders = reinterpret_cast<MeshletHeader*>(pBase + headerSize);
	uint8_t* pIndicesCursor = pBase + headerSize + meshletHeadsSize;
	uint8_t* pVerticesCursor = pIndicesCursor + indicesTotalBytes;

	for (uint32_t i = 0; i < count; ++i)
	{
		const Meshlet& m = meshlets[group.MeshletIDs[i]];

		MeshletHeader& mh = pHeaders[i];
		// 填充 meshlet 头
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
		std::memcpy(mh.BoundSphere, m.BoundSphere, sizeof(float) * 4);

		// 拷贝索引数据
		const size_t triBytes = m.Triangles.size();
		std::memcpy(pIndicesCursor, m.Triangles.data(), triBytes);
		pIndicesCursor += Math::AlignUp(static_cast<uint32_t>(triBytes), 4u);

		// 拷贝顶点数据
		for (uint32_t mvIdx = 0; mvIdx < m.Vertices.size(); mvIdx++)
		{
			const RawVertex& srcV = buildArgs.vertices[m.Vertices[mvIdx]];
			std::memcpy(pVerticesCursor + buildArgs.vertexStride * mvIdx, srcV.VertexData, buildArgs.vertexStride);
		}

		pVerticesCursor += Math::AlignUp(static_cast<uint32_t>(m.Vertices.size() * buildArgs.vertexStride), 4u);
	}

	//package.Metadata.OffsetInGlobalBuffer = 0;
	package.Metadata.SizeBytes = static_cast<uint32_t>(package.Blob.size());
	package.Metadata.UncompressedSize = package.Metadata.SizeBytes; // 暂时未压缩

	ASSERT((package.Metadata.UncompressedSize % 4u) == 0u, "Group package size must be 4-byte aligned");

	return package;
}

MeshletBuildProducts MeshletBuilder::BuildStreamingData(
	const MeshletBuildArgs& buildArgs,
	const std::vector<Group>& groups,
	const std::vector<Meshlet>& meshlets)
{
	MeshletBuildProducts products;

	if (meshlets.empty() || buildArgs.vertices.empty())
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
	std::unordered_map<uint32_t, uint32_t> groupIdToOrder; // 存储Build group id -> 序列化 group index
	std::unordered_map<uint32_t, std::vector<uint32_t>> lodGroups; // LODLevel -> build group id
	for (size_t i = 0; i < groupOrder.size(); ++i)
	{
		uint32_t buildGroupId = static_cast<uint32_t>(groupOrder[i]);
		groupIdToOrder[buildGroupId] = static_cast<uint32_t>(i);
		lodGroups[groups[buildGroupId].LODLevel].push_back(buildGroupId);
		if (groups[buildGroupId].LODLevel > maxLodLevel)
			maxLodLevel = groups[buildGroupId].LODLevel;
	}

	// 序列化 Groups
	for (auto idx : groupOrder)
	{
		products.Groups.push_back(SerializeGroup(
			buildArgs, groups[idx], meshlets, groupIdToOrder));
	}

	// ========== 分层构建 BVH ==========
	// 使用带元数据的节点，延迟计算偏移
	struct PendingNode
	{
		Renderer::HierarchyNode node;
		int32_t lodLevel;           // 所属 LOD 层（-1 表示连接所有LOD层级BVH的层级）
		uint32_t localChildStart;   // 在本层内的子节点起始
		uint32_t childCount;        // 子节点数量
	};

	// 按 LOD 层存储节点：lodBVHs[lod] = { 节点数组 }
	std::vector<std::vector<PendingNode>> lodBVHs(maxLodLevel + 1);
	std::vector<Renderer::HierarchyNode> lodRoots;

	for (int32_t lod = static_cast<int32_t>(maxLodLevel); lod >= 0; --lod)
	{
		// 构建叶子节点
		std::vector<Renderer::HierarchyNode> leafNodes;
		for (uint32_t gid : lodGroups[lod])
		{
			Renderer::HierarchyNode node{};
			std::memcpy(node.BoundSphere, groups[gid].GroupSphere, sizeof(float) * 4);
			node.MaxParrentError = groups[gid].ParrentError;
			node.Leaf.IsGroup = 1;
			node.Leaf.GroupIndex = groupIdToOrder[gid] + buildArgs.baseGroupIndex;
			node.Leaf.MeshletCountMinusOne = static_cast<uint8_t>(
				std::min<size_t>(groups[gid].MeshletIDs.size()-1, 127));
			leafNodes.push_back(node);
		}

		// 构建本层 BVH（使用局部偏移 baseNodeIndex=0）
		std::vector<uint32_t> reorderedLeafNodesIndices(leafNodes.size());
		auto bvhNodes = BuildHierarchy(leafNodes, buildArgs.settings.MaxBVHNodeChildren, reorderedLeafNodesIndices);
		ASSERT(bvhNodes.size() > 0);

		// 转换为 PendingNode
		lodBVHs[lod].reserve(bvhNodes.size());
		for (size_t i = 0; i < bvhNodes.size(); ++i)
		{
			PendingNode pn;
			pn.node = bvhNodes[i];
			pn.lodLevel = lod;
			// 提取局部 childStart（仅内部节点有效）
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

		// 保存根节点用于构建顶层
		if (!bvhNodes.empty())
			lodRoots.push_back(bvhNodes[0]);
	}


	// 构建顶层 BVH（连接各 LOD 根节点）
	std::vector<uint32_t> reorderedRootNodesIndices(lodRoots.size());
	auto topBVH = BuildHierarchy(lodRoots, buildArgs.settings.MaxBVHNodeChildren, reorderedRootNodesIndices);

	std::vector<PendingNode> topPending;
	for (size_t i = 0; i < topBVH.size(); ++i)
	{
		PendingNode pn;
		pn.node = topBVH[i];
		pn.lodLevel = -1; // 顶层标记
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

	// ========== 计算全局偏移 ==========
	// 布局：[顶层BVH] [LOD_max (跳过根)] [LOD_max-1 (跳过根)] ... [LOD_0 (跳过根)]

	const uint32_t topSize = static_cast<uint32_t>(topPending.size());
	std::vector<uint32_t> lodOffsets(maxLodLevel + 1);
	uint32_t cursor = topSize;

	for (int32_t lod = static_cast<int32_t>(maxLodLevel); lod >= 0; --lod)
	{
		lodOffsets[lod] = cursor;
		// 跳过根节点（已在顶层）
		cursor += static_cast<uint32_t>(lodBVHs[lod].size() - 1);
	}

	products.Hierarchy.resize(cursor);

	// ========== 写入顶层 BVH ==========
	// 顶层叶子节点（原各 LOD 根）需要修正 ChildStart
	for (uint32_t i = 0; i < topSize; ++i)
	{
		auto& pn = topPending[i];
		Renderer::HierarchyNode node = pn.node;

		// 顶层叶子节点是原 LOD 根，需要变成指向该 LOD 子节点的内部节点
		if (i >= topSize - lodRoots.size())
		{
			int32_t lod = static_cast<int32_t>(maxLodLevel) -
				static_cast<int32_t>(reorderedRootNodesIndices[i - (topSize - lodRoots.size())]);
			if (lod >= 0 && lodBVHs[lod].size() > 1)
			{
				// 该 LOD 根的子节点信息
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
			// 顶层内部节点，修正偏移
			node.Internal.ChildStartIndex = pn.localChildStart + buildArgs.baseNodeIndex;
		}

		products.Hierarchy[i] = node;
	}

	// ========== 写入各 LOD 层（跳过根节点）==========
	for (int32_t lod = static_cast<int32_t>(maxLodLevel); lod >= 0; --lod)
	{
		const auto& bvh = lodBVHs[lod];
		if (bvh.size() <= 1)
			continue;

		const uint32_t baseOffset = lodOffsets[lod];

		for (size_t i = 1; i < bvh.size(); ++i) // 从 1 开始，跳过根
		{
			const auto& pn = bvh[i];
			Renderer::HierarchyNode node = pn.node;
			if (pn.childCount > 0 && node.Internal.IsGroup == 0)
			{
				// 修正内部节点偏移：局部偏移 - 1（跳过根）+ 层基址
				node.Internal.ChildStartIndex = baseOffset + (pn.localChildStart - 1) + buildArgs.baseNodeIndex;
			}
			products.Hierarchy[baseOffset + (i - 1)] = node;
		}
	}

	// ========== 打印统计信息 ==========
	Utility::Printf(L"[BuildStreamingData] Summary:\n");
	Utility::Printf(L"Total Groups: %u\n", static_cast<uint32_t>(products.Groups.size()));
	Utility::Printf(L"Total BVH Nodes: %u\n", static_cast<uint32_t>(products.Hierarchy.size()));
	Utility::Printf(L"Top-level BVH Nodes: %u\n", topSize);
	Utility::Printf(L"LOD Levels: %u\n", maxLodLevel + 1);

	// 按 LOD 层统计
	for (uint32_t lod = 0; lod <= maxLodLevel; ++lod)
	{
		uint32_t groupCount = static_cast<uint32_t>(lodGroups[lod].size());
		uint32_t bvhCount = static_cast<uint32_t>(lodBVHs[lod].size());
		Utility::Printf(L"  LOD %u: %u groups, %u BVH nodes\n", lod, groupCount, bvhCount);
	}

	// -------------------------------------------------------
	// 打印 BVH 节点内容
	// -------------------------------------------------------
	Utility::Printf(L"--- BVH Nodes Dump (%u nodes) ---\n", static_cast<uint32_t>(products.Hierarchy.size()));
	for (size_t i = 0; i < products.Hierarchy.size(); ++i)
	{
		const auto& node = products.Hierarchy[i];

		// 检查 IsGroup 标志 (位域在 union 中共享，访问 Internal.IsGroup 即可)
		if (node.Internal.IsGroup)
		{
			// 是 Group 节点 (Leaf)
			// 此时 ChildStartIndex/ChildCount 字段无效，应打印 GroupIndex 等信息
			Utility::Printf(L"Node[%u]: IsGroup=YES (Leaf), GroupIndex=%u, MeshletCount=%u, Bounds=(%.3f, %.3f, %.3f, %.3f), Error=%.5f\n",
				static_cast<uint32_t>(i),
				node.Leaf.GroupIndex,
				node.Leaf.MeshletCountMinusOne + 1,
				node.BoundSphere[0], node.BoundSphere[1], node.BoundSphere[2], node.BoundSphere[3],
				node.MaxParrentError);
		}
		else
		{
			// 是内部节点 (Internal)
			Utility::Printf(L"Node[%u]: IsGroup=NO,  ChildCount=%u, ChildStartIndex=%u, Bounds=(%.3f, %.3f, %.3f, %.3f), Error=%.5f\n",
				static_cast<uint32_t>(i),
				node.Internal.ChildCount,
				node.Internal.ChildStartIndex,
				node.BoundSphere[0], node.BoundSphere[1], node.BoundSphere[2], node.BoundSphere[3],
				node.MaxParrentError);
		}
	}
	Utility::Printf(L"-----------------------------------\n");

	return products;
}

std::vector<Renderer::HierarchyNode> MeshletBuilder::BuildHierarchy(
	const std::vector<Renderer::HierarchyNode>& initNodes,
	uint32_t maxBVHNodeChildren,
	std::vector<uint32_t>& outInitNodesReorderMap)
{
	if (initNodes.empty())
		return {};

	// 检查单节点情况：如果是 group 节点，需要创建父节点
	if (initNodes.size() == 1)
	{
		outInitNodesReorderMap[0] = 0;
		if (initNodes[0].Internal.IsGroup == 1)
		{
			// 为单个 group 节点创建父节点
			HierarchyNode parent{};
			parent.Internal.IsGroup = 0;
			parent.Internal.ChildCount = 1;
			parent.Internal.ChildStartIndex = 1; // 子节点在索引1
			std::copy(initNodes[0].BoundSphere, initNodes[0].BoundSphere + 4, parent.BoundSphere);
			parent.MaxParrentError = initNodes[0].MaxParrentError;

			// 返回 [父节点, 子节点]
			std::vector<HierarchyNode> result(2);
			result[0] = parent;
			result[1] = initNodes[0];
			return result;
		}
		return initNodes;
	}

	const uint32_t maxChildren = maxBVHNodeChildren;

	// levels[0] = 叶子层, levels[N] = 根层
	std::vector<std::vector<HierarchyNode>> levels(1);

	// 初始化叶子层
	levels[0] = initNodes;//std::move(initNodes);

	// 自底向上构建
	while (levels.back().size() > 1)
	{
		const auto& currentLevel = levels.back();
		const size_t nodeCount = currentLevel.size();

		// 提取球心用于空间聚类
		std::vector<float> centers(nodeCount * 3);
		for (size_t i = 0; i < nodeCount; ++i)
		{
			centers[i * 3 + 0] = currentLevel[i].BoundSphere[0];
			centers[i * 3 + 1] = currentLevel[i].BoundSphere[1];
			centers[i * 3 + 2] = currentLevel[i].BoundSphere[2];
		}

		// 空间聚类
		std::vector<unsigned int> clusterIndices(nodeCount);
		meshopt_spatialClusterPoints(
			clusterIndices.data(),
			centers.data(),
			nodeCount,
			sizeof(float) * 3,
			maxChildren);

		if (levels.size() == 1)
		{
			// 输出叶子层重排映射
			for (size_t i = 0; i < nodeCount; ++i)
			{
				outInitNodesReorderMap[i] = clusterIndices[i];
			}
		}

		uint32_t clusterCount = (static_cast<uint32_t>(nodeCount) + maxChildren - 1) / maxChildren;


		// 统计每个簇的节点数
		std::vector<uint32_t> clusterSizes(clusterCount, maxChildren);
		if (nodeCount % maxChildren != 0)
			clusterSizes.back() = nodeCount % maxChildren;

		// 计算每个簇在重排数组中的起始位置
		std::vector<uint32_t> clusterStarts(clusterCount + 1, 0);
		for (size_t i = 0; i < clusterCount; ++i)
			clusterStarts[i + 1] = clusterStarts[i] + clusterSizes[i];

		// 按簇重排当前层节点（原地重排到临时数组）
		std::vector<HierarchyNode> sortedLevel(nodeCount);
		for (size_t i = 0; i < nodeCount; ++i)
		{
			sortedLevel[i] = currentLevel[clusterIndices[i]];
		}

		// 替换当前层为已排序版本
		levels.back() = std::move(sortedLevel);

		// 构建父节点层
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

			// 合并包围球和误差
			const auto& sortedCurrent = levels.back();
			float mergedSphere[4];
			std::copy(sortedCurrent[clusterStarts[c]].BoundSphere,
				sortedCurrent[clusterStarts[c]].BoundSphere + 4,
				mergedSphere);
			float maxError = sortedCurrent[clusterStarts[c]].MaxParrentError;

			for (uint32_t j = 1; j < clusterSizes[c]; ++j)
			{
				const auto& child = sortedCurrent[clusterStarts[c] + j];
				MergeSphere(mergedSphere, child.BoundSphere, mergedSphere);
				maxError = std::max(maxError, child.MaxParrentError);
			}

			std::copy(mergedSphere, mergedSphere + 4, parent.BoundSphere);
			parent.MaxParrentError = maxError;

			parentLevel.push_back(std::move(parent));
		}

		levels.push_back(std::move(parentLevel));
	}

	// 扁平化：从根到叶排列
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
			// 设置 ChildStartIndex（仅对内部节点）
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
