#include "MeshletBuilder.h"
#include "pch.h"
#include <unordered_map>
#include <numeric>
#include <cassert>
#include <cfloat>
#include <execution> 
#include "../Core/Utility.h"

static constexpr float kInfinity = std::numeric_limits<float>::infinity();

std::vector<TempMeshlet> MeshletBuilder::Build(
	std::span<const RawVertex> vertices,
	std::span<const uint32_t> indices,
	const MeshletBuildSettings& settings)
{
	// 生成 position-only remap（供后续锁边用）
	std::vector<uint32_t> posRemap(vertices.size());
	GeneratePositionRemap(vertices, posRemap);

	// LOD0：整网格 -> meshlet
	std::vector<TempMeshlet> current = BuildLOD0Meshlets(vertices, indices, settings);

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

	// 迭代简化
	uint32_t lodLevel = 1;
	while (true)
	{
		// 基于共享顶点 / 空间接近度分组
		auto groups = GroupMeshlets(current, activeIds, vertices, settings.TargetMeshletsPerGroup);
		if (groups.empty())
			break;

		// 构建顶点锁（跨组共享 position-only 顶点全部上锁）
		std::vector<unsigned char> vertexLock(vertices.size(), 0);
		BuildVertexLocksByGroups(groups, current, posRemap, vertices.size(), vertexLock);

		// 对每个组尝试简化 -> 生成下一层 meshlets
		struct GroupResult
		{
			bool simplified = false;
			float err = 0.0f;                     // SimplifyGroup 返回的误差
			float groupError = 0.0f;              // g.GroupError
			float groupSphere[4]{};
			std::vector<TempMeshlet> generated;   // 该组生成的新 meshlets
			std::vector<uint32_t> keepIds;        // 简化失败或单元素组：直接延续的 meshlet id
			std::vector<uint32_t> originalIds;    // 该组原有 meshlet id（用于写回 ParentError/Bounds）
		};

		std::vector<GroupResult> results(groups.size());
		std::vector<size_t> gi(groups.size());
		std::iota(gi.begin(), gi.end(), size_t(0));

		// 并行执行每个分组的简化尝试（只产生局部结果，不改写 shared 状态）
		std::for_each(std::execution::par, gi.begin(), gi.end(),
			[&](size_t idx)
			{
				const Group& g = groups[idx];
				GroupResult r;
				r.groupError = g.GroupError;
				std::copy(g.GroupSphere, g.GroupSphere + 4, r.groupSphere);
				r.originalIds = g.MeshletIDs;

				// 单个 meshlet 分组无需简化
				if (g.MeshletIDs.size() <= 1)
				{
					r.simplified = false;
					r.keepIds = g.MeshletIDs;
					results[idx] = std::move(r);
					return;
				}

				float err = 0.0f;
				std::vector<TempMeshlet> generated;
				if (SimplifyGroup(g, current, vertices, settings, vertexLock, generated, err))
				{
					r.simplified = true;
					r.err = err;
					r.generated = std::move(generated);
				}
				else
				{
					r.simplified = false;
					r.keepIds = g.MeshletIDs; // 简化失败 -> 保留原 meshlet
				}

				results[idx] = std::move(r);
			});

		std::vector<TempMeshlet> newMeshlets;
		std::vector<uint32_t> nextActive;
		nextActive.reserve(activeIds.size());

		// 串行：设置误差、包围球、LODLevel，更新父误差，构建 nextActive
		for (const auto& r : results)
		{
			if (!r.simplified)
			{
				for (uint32_t id : r.keepIds) nextActive.push_back(id);
				continue;
			}

			const float totalErr = r.err + r.groupError;
			const size_t baseOffset = newMeshlets.size();

			// 写新 meshlet 的误差与包围球等
			for (auto& nm : const_cast<std::vector<TempMeshlet>&>(r.generated))
			{
				TempMeshlet out = std::move(nm);
				out.Error = totalErr;
				out.LodError = totalErr;
				std::copy(r.groupSphere, r.groupSphere + 4, out.LodBounds);
				ComputeMeshletSphere(out, vertices, out.Sphere);
				out.LODLevel = lodLevel;
				newMeshlets.emplace_back(std::move(out));
			}

			// 新 meshlet 的全局索引：current.size() + baseOffset + j
			for (size_t j = 0; j < r.generated.size(); ++j)
				nextActive.push_back(static_cast<uint32_t>(current.size() + baseOffset + j));

			// 写回父误差/包围（仅在成功简化时）
			for (uint32_t id : r.originalIds)
			{
				current[id].ParentError = totalErr;
				std::copy(r.groupSphere, r.groupSphere + 4, current[id].ParentBounds);
			}
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
			if (reduction < settings.minReductionRatio)
			{
				Utility::Printf(L"[MeshletBuilder]  Simplification reduction %.2f%% below threshold %.2f%%, stopping.\n",
					reduction * 100.0f, settings.minReductionRatio * 100.0f);
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

	return current;
}

std::vector<TempMeshlet>
MeshletBuilder::BuildLOD0Meshlets(
	std::span<const RawVertex> vertices,
	std::span<const uint32_t> indices,
	const MeshletBuildSettings& s)
{
	std::vector<TempMeshlet> out;
	if (indices.empty())
		return out;

	BuildMeshletsFromIndices(indices.data(), indices.size(), vertices, s, out);

	// LOD0 误差 = 0
	for (auto& m : out)
	{
		m.Error = 0.0f;
		ComputeMeshletSphere(m, vertices, m.Sphere);
	}
	return out;
}

void MeshletBuilder::BuildMeshletsFromIndices(
	const uint32_t* indices, size_t indexCount,
	std::span<const RawVertex> vertices,
	const MeshletBuildSettings& s,
	std::vector<TempMeshlet>& out)
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

		TempMeshlet tm;
		tm.ParentError = kInfinity;

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
	const TempMeshlet& m,
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

std::vector<MeshletBuilder::Group>
MeshletBuilder::GroupMeshlets(
	const std::vector<TempMeshlet>& current,
	std::span<const uint32_t> subsetIds,
	std::span<const RawVertex> vertices,
	uint32_t targetGroupSize)
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
	std::vector<Group> groups(partCount);
	for (size_t i = 0; i < clusterCount; ++i)
		groups[partition[i]].MeshletIDs.push_back(subsetIds[i]);

	// 计算组球（合并各 meshlet 球）
	for (auto& g : groups)
	{
		bool first = true;
		float acc[4]{};
		for (uint32_t mid : g.MeshletIDs)
		{
			float bs[4];
			ComputeMeshletSphere(current[mid], vertices, bs);
			if (first) { std::copy(bs, bs + 4, acc); first = false; }
			else MergeSphere(acc, bs, acc);

			g.GroupError = std::max(g.GroupError, current[mid].Error);
		}
		std::copy(acc, acc + 4, g.GroupSphere);
	}
	return groups;
}

void MeshletBuilder::BuildVertexLocksByGroups(
	const std::vector<Group>& groups,
	const std::vector<TempMeshlet>& current,
	const std::vector<uint32_t>& posRemap,
	size_t vertexCount,
	std::vector<unsigned char>& outVertexLock)
{
	// 标记每个 position-only 顶点首次被哪个组占用
	// 若同一 position 顶点被不同组占用 -> 锁定该 position
	// 最后把对应的所有原始顶点（拥有该 position id）设为 lock=1
	uint32_t maxPosId = 0;
	for (size_t v = 0; v < vertexCount; ++v) maxPosId = std::max(maxPosId, posRemap[v]);
	std::vector<int> owner(maxPosId + 1, -1);
	std::vector<uint8_t> lockedPos(maxPosId + 1, 0);

	for (size_t gi = 0; gi < groups.size(); ++gi)
	{
		int gid = static_cast<int>(gi);
		for (uint32_t mid : groups[gi].MeshletIDs)
		{
			const auto& m = current[mid];
			for (uint32_t v : m.Vertices)
			{
				uint32_t pid = posRemap[v];
				if (owner[pid] == -1) owner[pid] = gid;
				else if (owner[pid] != gid) lockedPos[pid] = 1;
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
	const std::vector<TempMeshlet>& current,
	std::span<const RawVertex> vertices,
	const MeshletBuildSettings& s,
	const std::vector<unsigned char>& vertexLock,
	std::vector<TempMeshlet>& outNewMeshlets,
	float& outError)
{
	// 拼接组三角（原始顶点索引）
	std::vector<uint32_t> expanded;
	// 预估容量：每个 meshlet <= MaxMeshletTriangles
	expanded.reserve(g.MeshletIDs.size() * s.MaxMeshletTriangles * 3);

	for (uint32_t mid : g.MeshletIDs)
	{
		const auto& m = current[mid];
		// micro tri -> 原始索引
		for (size_t t = 0; t < m.Triangles.size(); t += 3)
		{
			uint32_t l0 = m.Triangles[t + 0];
			uint32_t l1 = m.Triangles[t + 1];
			uint32_t l2 = m.Triangles[t + 2];
			expanded.push_back(m.Vertices[l0]);
			expanded.push_back(m.Vertices[l1]);
			expanded.push_back(m.Vertices[l2]);
		}
	}
	if (expanded.empty())
		return false;

	// 顶点位置/法线属性（法线用于 attribute metric）
	const float* pos = reinterpret_cast<const float*>(&vertices[0].Position);
	const size_t stride = sizeof(RawVertex);

	// 顶点属性流：法线3f，连续数组
	std::vector<float> normals;
	normals.resize(vertices.size() * 3);
	for (size_t i = 0; i < vertices.size(); ++i)
	{
		normals[i * 3 + 0] = vertices[i].Normal.GetX();
		normals[i * 3 + 1] = vertices[i].Normal.GetY();
		normals[i * 3 + 2] = vertices[i].Normal.GetZ();
	}
	const float attributeWeights[3] = { 0.5f, 0.5f, 0.5f };

	// 目标索引数（保留 ~50%）
	const size_t targetIndexCount = size_t(expanded.size() * s.TargetSimplifyRatio);

	std::vector<uint32_t> simplified(expanded.size());
	float resultError = 0.0f;

	unsigned int options =
		meshopt_SimplifySparse |
		meshopt_SimplifyErrorAbsolute;
		//meshopt_SimplifyPermissive |
		//meshopt_SimplifyPrune;

	size_t newCount = meshopt_simplifyWithAttributes(
		simplified.data(),
		expanded.data(), expanded.size(),
		pos, vertices.size(), stride,
		normals.data(), sizeof(float) * 3,
		attributeWeights, 3,
		vertexLock.data(),                    // 锁边
		targetIndexCount,
		FLT_MAX,                              // 绝对误差上限
		options,
		&resultError);

	simplified.resize(newCount);

	// 简化失败判定
	float ratio = float(newCount) / float(expanded.size());
	if (ratio > s.SimplificationFailurePercentage)
		return false;

	outError = resultError;

	// 用简化后的索引重建新 meshlet
	BuildMeshletsFromIndices(simplified.data(), simplified.size(), vertices, s, outNewMeshlets);
	return !outNewMeshlets.empty();
}