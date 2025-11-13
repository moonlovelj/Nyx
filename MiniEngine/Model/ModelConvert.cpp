//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):  James Stanard
//             Chuck Walbourn (ATG)
//
// This code depends on DirectXTex
//

#include "ModelLoader.h"
#include "Renderer.h"
#include "glTF.h"
#include "TextureConvert.h"
#include "MeshConvert.h"
#include "TextureManager.h"
#include "GraphicsCommon.h"
#include "../Core/Utility.h"
#include "../Core/Math/Common.h"
#include "MeshOptimizer/MeshOptimizer.h"
#include "metis.h"

#include <fstream>
#include <map>
#include <unordered_map>

using namespace DirectX;
using namespace Math;
using namespace Renderer;
using namespace Graphics;

#include "NaniteLODConfig.h"
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <deque>
#include <future>
#include <iostream>
#include <thread>

namespace Renderer
{
	struct MeshletWIP
	{
		uint32_t id;
		uint32_t lodLevel;

		std::vector<uint8_t> indexBuffer;
		uint32_t indexCount;
		uint32_t indexSize;

		float bounds[4];						// {cx, cy, cz, r}
		float shareSiblingsBounds[4];           // {cx, cy, cz, r}
		float lodError;							// 当前层级的简化误差
		float maxSiblingsError;					// 累积误差
		float parentError;						// 父层级误差
		float parentBounds[4];					// 父层级包围球
	};

	struct PreMeshletInfo
	{
		bool index32 = false;
		std::deque<MeshletWIP> allMeshlets;
		std::vector<uint32_t> rootMeshletIds;
		uint16_t vertexStride = 0;
		uint32_t vertexCount = 0;
		Utility::ByteArray VB;
		Utility::ByteArray DepthVB;
	};

	// 无向边 key
	static inline uint64_t MakeEdgeKey(uint32_t a, uint32_t b)
	{
		uint32_t lo = std::min(a, b);
		uint32_t hi = std::max(a, b);
		return (uint64_t(hi) << 32) | uint64_t(lo);
	}

	// 从 meshlet 中提取边界边
	static std::unordered_set<uint64_t> ExtractBoundaryEdges(const MeshletWIP* m, bool is32BitIndex)
	{
		std::unordered_map<uint64_t, uint32_t> edgeCount;
		edgeCount.reserve(m->indexCount * 3 / 2);

		auto addEdge = [&](uint32_t v0, uint32_t v1)
			{
				uint64_t k = MakeEdgeKey(v0, v1);
				edgeCount[k] += 1;
			};

		if (is32BitIndex)
		{
			const uint32_t* idx = reinterpret_cast<const uint32_t*>(m->indexBuffer.data());
			for (uint32_t i = 0; i < m->indexCount; i += 3)
			{
				uint32_t a = idx[i + 0];
				uint32_t b = idx[i + 1];
				uint32_t c = idx[i + 2];
				addEdge(a, b); addEdge(b, c); addEdge(c, a);
			}
		}
		else
		{
			const uint16_t* idx = reinterpret_cast<const uint16_t*>(m->indexBuffer.data());
			for (uint32_t i = 0; i < m->indexCount; i += 3)
			{
				uint32_t a = idx[i + 0];
				uint32_t b = idx[i + 1];
				uint32_t c = idx[i + 2];
				addEdge(a, b); addEdge(b, c); addEdge(c, a);
			}
		}

		std::unordered_set<uint64_t> boundary;
		boundary.reserve(edgeCount.size());
		for (auto& kv : edgeCount)
			if (kv.second == 1) boundary.insert(kv.first);
		return boundary;
	}

	// METIS 分区：返回每组 meshlets（组内大小≤groupSize；失败则顺序分组）
	static std::vector<std::vector<MeshletWIP*>> PartitionMeshletsByMetis(
		const std::vector<MeshletWIP*>& levelMeshlets,
		bool is32BitIndex,
		size_t groupSize,
		bool enableLogging)
	{
		std::vector<std::vector<MeshletWIP*>> groups;
		const size_t N = levelMeshlets.size();
		if (N == 0) return groups;
		if (groupSize == 0) groupSize = 1;
		if (N <= groupSize)
		{
			groups.push_back(levelMeshlets);
			return groups;
		}

		// 每个 meshlet 的边界边
		std::vector<std::unordered_set<uint64_t>> boundaries(N);
		for (size_t i = 0; i < N; ++i)
			boundaries[i] = ExtractBoundaryEdges(levelMeshlets[i], is32BitIndex);

		// 构建邻接（共享边界边 => 邻接，权重=共享边界边数量 ----
		std::vector<std::unordered_map<uint32_t, uint32_t>> adjacencyWeighted(N);

		// 统计所有 boundary 边数量，便于预留
		size_t totalBoundaryEdges = 0;
		for (const auto& bset : boundaries)
			totalBoundaryEdges += bset.size();

		// 反向索引：edgeKey -> 拥有该边的 meshlet 列表
		std::unordered_map<uint64_t, std::vector<uint32_t>> edgeToMeshlets;
		edgeToMeshlets.reserve(totalBoundaryEdges * 2); // 经验系数，减少 rehash

		for (uint32_t mi = 0; mi < (uint32_t)N; ++mi)
		{
			for (uint64_t edgeKey : boundaries[mi])
			{
				auto& list = edgeToMeshlets[edgeKey];
				list.push_back(mi);
			}
		}

		// 根据每条共享边的参与者两两配对增加权重
		for (auto& kv : edgeToMeshlets)
		{
			const std::vector<uint32_t>& owners = kv.second;
			for (size_t a = 0; a < owners.size(); ++a)
			{
				uint32_t i = owners[a];
				for (size_t b = a + 1; b < owners.size(); ++b)
				{
					uint32_t j = owners[b];
					adjacencyWeighted[i][j] += 1;
					adjacencyWeighted[j][i] += 1;
				}
			}
		}

		size_t totalLinks = 0;
		for (auto& mp : adjacencyWeighted) totalLinks += mp.size();
		if (totalLinks == 0)
		{
			// 无邻接，顺序分组
			for (size_t i = 0; i < N; i += groupSize)
			{
				std::vector<MeshletWIP*> g;
				for (size_t j = i; j < std::min(i + groupSize, N); ++j)
					g.push_back(levelMeshlets[j]);
				groups.push_back(std::move(g));
			}
			if (enableLogging)
				Utility::Printf("    [METIS] No adjacency, fallback sequential groups=%zu\n", groups.size());
			return groups;
		}

		// 构建 METIS CSR 结构
		// xadj: size N+1, adjncy: size 2*E (双向), adjwgt
		std::vector<idx_t> xadj(N + 1, 0);
		std::vector<idx_t> adjncy;
		std::vector<idx_t> adjwgt;

		// 统计总边数（双向）
		size_t edgeEntries = 0;
		for (size_t i = 0; i < N; ++i)
			edgeEntries += adjacencyWeighted[i].size();

		adjncy.reserve(edgeEntries);
		adjwgt.reserve(edgeEntries);

		for (size_t i = 0; i < N; ++i)
		{
			xadj[i] = (idx_t)adjncy.size();
			for (auto& kv : adjacencyWeighted[i])
			{
				adjncy.push_back((idx_t)kv.first);
				adjwgt.push_back((idx_t)kv.second); // 权重=共享边界边数量
			}
		}
		xadj[N] = (idx_t)adjncy.size();

		// 节点权重：三角形数（提高均衡性）
		std::vector<idx_t> vwgt(N, 0);
		for (size_t i = 0; i < N; ++i)
			vwgt[i] = (idx_t)(levelMeshlets[i]->indexCount / 3);

		idx_t ncon = 1;
		idx_t nvtxs = (idx_t)N;
		idx_t nparts = (idx_t)((N + groupSize - 1) / groupSize); // 目标划分簇数

		std::vector<idx_t> part(N, 0);
		idx_t objval = 0;

		int options[METIS_NOPTIONS];
		METIS_SetDefaultOptions(options);
		options[METIS_OPTION_UFACTOR] = 200;

		int status = METIS_PartGraphKway(
			&nvtxs, &ncon,
			xadj.data(), adjncy.data(),
			vwgt.data(), nullptr, adjwgt.data(),
			&nparts,
			nullptr, nullptr,
			options,
			&objval, part.data());

		if (status != METIS_OK)
		{
			if (enableLogging)
				Utility::Printf("    [METIS] Partition failed (status=%d). Fallback sequential.\n", status);
			for (size_t i = 0; i < N; i += groupSize)
			{
				std::vector<MeshletWIP*> g;
				for (size_t j = i; j < std::min(i + groupSize, N); ++j)
					g.push_back(levelMeshlets[j]);
				groups.push_back(std::move(g));
			}
			return groups;
		}

		// 根据 part 汇聚初始簇
		std::unordered_map<idx_t, std::vector<MeshletWIP*>> buckets;
		buckets.reserve(nparts);
		for (size_t i = 0; i < N; ++i)
			buckets[part[i]].push_back(levelMeshlets[i]);

		// 处理超大簇：拆分为 ≤ groupSize 块（简单顺序或 BFS）
		for (auto& kv : buckets)
		{
			auto& vec = kv.second;
			if (vec.size() <= groupSize)
			{
				groups.push_back(vec);
			}
			else
			{
				// 简单顺序 chunk 拆分（//TODO: 换为局部再次调用 METIS）
				for (size_t i = 0; i < vec.size(); i += groupSize)
				{
					std::vector<MeshletWIP*> g;
					for (size_t j = i; j < std::min(i + groupSize, vec.size()); ++j)
						g.push_back(vec[j]);
					groups.push_back(std::move(g));
				}
			}
		}

		if (enableLogging)
			Utility::Printf("    [METIS] Partition result: %zu groups (target parts=%d, obj=%d)\n",
				groups.size(), (int)nparts, (int)objval);

		return groups;
	}

	// 合并包围球（Ritter's algorithm）
	static void MergeBoundingSpheres(float* result, const float* a, const float* b)
	{
		float dx = b[0] - a[0];
		float dy = b[1] - a[1];
		float dz = b[2] - a[2];
		float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

		if (dist + b[3] <= a[3])
		{
			std::memcpy(result, a, sizeof(float) * 4);
		}
		else if (dist + a[3] <= b[3])
		{
			std::memcpy(result, b, sizeof(float) * 4);
		}
		else
		{
			float newRadius = (dist + a[3] + b[3]) * 0.5f;
			float t = (newRadius - a[3]) / dist;
			result[0] = a[0] + dx * t;
			result[1] = a[1] + dy * t;
			result[2] = a[2] + dz * t;
			result[3] = newRadius;
		}
	}

	// 根据三角形数量计算动态简化率
	static float CalculateDynamicSimplificationRate(
		size_t triangleCount,
		const NaniteLODConfig& cfg)
	{
		const auto& dr = cfg.dynamicRates;

		if (triangleCount > dr.largeThreshold)
			return dr.largeRate;
		else if (triangleCount > dr.mediumThreshold)
			return dr.mediumRate;
		else if (triangleCount > dr.smallThreshold)
			return dr.smallRate;
		else
			return dr.tinyRate;
	}

	// 合并 meshlets
	static std::vector<uint32_t> MergeMeshlets(
		const std::vector<MeshletWIP*>& group,
		bool is32BitIndex)
	{
		size_t totalIndices = 0;
		for (auto* m : group)
			totalIndices += m->indexCount;

		std::vector<uint32_t> merged;
		merged.reserve(totalIndices);

		for (auto* m : group)
		{
			if (is32BitIndex)
			{
				const uint32_t* src = reinterpret_cast<const uint32_t*>(m->indexBuffer.data());
				merged.insert(merged.end(), src, src + m->indexCount);
			}
			else
			{
				const uint16_t* src = reinterpret_cast<const uint16_t*>(m->indexBuffer.data());
				for (uint32_t i = 0; i < m->indexCount; ++i)
					merged.push_back(uint32_t(src[i]));
			}
		}

		return merged;
	}

	// 拆分为 meshlets
	static std::vector<MeshletWIP> SplitIntoMeshlets(
		const std::vector<uint32_t>& indices,
		uint32_t lodLevel,
		uint32_t& nextMeshletId,
		float lodError,
		float cumulativeError,
		const float* groupBounds,
		const float* positions,
		uint16_t vertexStride,
		uint32_t vertexCount,
		bool is32BitIndex,
		const NaniteLODConfig& cfg)
	{
		size_t max_meshlets = meshopt_buildMeshletsBound(
			indices.size(), cfg.meshletMaxVertices, cfg.meshletMaxTriangles);
		std::vector<meshopt_Meshlet> meshlets(max_meshlets);
		std::vector<unsigned int> meshlet_vertices(indices.size());
		std::vector<unsigned char> meshlet_triangles(indices.size());

		size_t meshlet_count = meshopt_buildMeshlets(
			meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(),
			indices.data(), indices.size(),
			positions, vertexCount, vertexStride,
			cfg.meshletMaxVertices, cfg.meshletMaxTriangles,
			cfg.meshletBackfaceCullingConeWeight);

		// 优化每个 meshlet
		for (size_t m = 0; m < meshlet_count; ++m)
		{
			const meshopt_Meshlet& ml = meshlets[m];
			meshopt_optimizeMeshlet(
				&meshlet_vertices[ml.vertex_offset],
				&meshlet_triangles[ml.triangle_offset],
				ml.triangle_count, ml.vertex_count);
		}

		// 创建 MeshletWIP
		std::vector<MeshletWIP> result;
		result.reserve(meshlet_count);

		for (size_t m = 0; m < meshlet_count; ++m)
		{
			const meshopt_Meshlet& ml = meshlets[m];

			MeshletWIP wip{};
			wip.id = nextMeshletId++;
			wip.lodLevel = lodLevel;
			wip.indexCount = ml.triangle_count * 3;
			wip.indexSize = is32BitIndex ? 4 : 2;
			wip.lodError = lodError;
			wip.maxSiblingsError = cumulativeError;
			wip.parentError = std::numeric_limits<float>::infinity();
			std::memcpy(wip.parentBounds, groupBounds, sizeof(float) * 4);

			// 计算包围球
			const meshopt_Bounds mb = meshopt_computeMeshletBounds(
				&meshlet_vertices[ml.vertex_offset],
				&meshlet_triangles[ml.triangle_offset],
				ml.triangle_count, positions, vertexCount, vertexStride);
			wip.bounds[0] = mb.center[0];
			wip.bounds[1] = mb.center[1];
			wip.bounds[2] = mb.center[2];
			wip.bounds[3] = mb.radius;

			// 重排 IB
			size_t ibSize = wip.indexCount * wip.indexSize;
			wip.indexBuffer.resize(ibSize);

			uint32_t localIndexCursor = 0;
			for (uint32_t t = 0; t < ml.triangle_count; ++t)
			{
				unsigned char ia = meshlet_triangles[ml.triangle_offset + t * 3 + 0];
				unsigned char ib = meshlet_triangles[ml.triangle_offset + t * 3 + 1];
				unsigned char ic = meshlet_triangles[ml.triangle_offset + t * 3 + 2];

				uint32_t va = meshlet_vertices[ml.vertex_offset + ia];
				uint32_t vb = meshlet_vertices[ml.vertex_offset + ib];
				uint32_t vc = meshlet_vertices[ml.vertex_offset + ic];

				if (is32BitIndex)
				{
					uint32_t* dst = reinterpret_cast<uint32_t*>(wip.indexBuffer.data());
					dst[localIndexCursor++] = va;
					dst[localIndexCursor++] = vb;
					dst[localIndexCursor++] = vc;
				}
				else
				{
					uint16_t* dst = reinterpret_cast<uint16_t*>(wip.indexBuffer.data());
					dst[localIndexCursor++] = static_cast<uint16_t>(va);
					dst[localIndexCursor++] = static_cast<uint16_t>(vb);
					dst[localIndexCursor++] = static_cast<uint16_t>(vc);
				}
			}

			result.push_back(std::move(wip));
		}

		return result;
	}

	static std::vector<MeshletWIP> SplitIntoMeshletsConcurrent(
		const std::vector<uint32_t>& indices,
		uint32_t lodLevel,
		std::atomic<uint32_t>& idCounter,
		float lodError,
		float cumulativeError,
		const float* groupBounds,
		const float* positions,
		uint16_t vertexStride,
		uint32_t vertexCount,
		bool is32BitIndex,
		const NaniteLODConfig& cfg)
	{
		size_t max_meshlets = meshopt_buildMeshletsBound(
			indices.size(), cfg.meshletMaxVertices, cfg.meshletMaxTriangles);
		std::vector<meshopt_Meshlet> meshlets(max_meshlets);
		std::vector<unsigned int> meshlet_vertices(indices.size());
		std::vector<unsigned char> meshlet_triangles(indices.size());

		size_t meshlet_count = meshopt_buildMeshlets(
			meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(),
			indices.data(), indices.size(),
			positions, vertexCount, vertexStride,
			cfg.meshletMaxVertices, cfg.meshletMaxTriangles,
			cfg.meshletBackfaceCullingConeWeight);

		for (size_t m = 0; m < meshlet_count; ++m)
		{
			const meshopt_Meshlet& ml = meshlets[m];
			meshopt_optimizeMeshlet(
				&meshlet_vertices[ml.vertex_offset],
				&meshlet_triangles[ml.triangle_offset],
				ml.triangle_count, ml.vertex_count);
		}

		std::vector<MeshletWIP> result;
		result.reserve(meshlet_count);

		for (size_t m = 0; m < meshlet_count; ++m)
		{
			const meshopt_Meshlet& ml = meshlets[m];
			MeshletWIP wip{};
			wip.id = idCounter.fetch_add(1, std::memory_order_relaxed);
			wip.lodLevel = lodLevel;
			wip.indexCount = ml.triangle_count * 3;
			wip.indexSize = is32BitIndex ? 4 : 2;
			wip.lodError = lodError;
			wip.maxSiblingsError = cumulativeError;
			std::memcpy(wip.shareSiblingsBounds, groupBounds, sizeof(float) * 4);
			wip.parentError = std::numeric_limits<float>::infinity();
			std::memcpy(wip.parentBounds, groupBounds, sizeof(float) * 4);

			const meshopt_Bounds mb = meshopt_computeMeshletBounds(
				&meshlet_vertices[ml.vertex_offset],
				&meshlet_triangles[ml.triangle_offset],
				ml.triangle_count, positions, vertexCount, vertexStride);
			wip.bounds[0] = mb.center[0];
			wip.bounds[1] = mb.center[1];
			wip.bounds[2] = mb.center[2];
			wip.bounds[3] = mb.radius;

			size_t ibSize = wip.indexCount * wip.indexSize;
			wip.indexBuffer.resize(ibSize);
			uint32_t localIndexCursor = 0;
			for (uint32_t t = 0; t < ml.triangle_count; ++t)
			{
				unsigned char ia = meshlet_triangles[ml.triangle_offset + t * 3 + 0];
				unsigned char ib = meshlet_triangles[ml.triangle_offset + t * 3 + 1];
				unsigned char ic = meshlet_triangles[ml.triangle_offset + t * 3 + 2];
				uint32_t va = meshlet_vertices[ml.vertex_offset + ia];
				uint32_t vb = meshlet_vertices[ml.vertex_offset + ib];
				uint32_t vc = meshlet_vertices[ml.vertex_offset + ic];

				if (is32BitIndex)
				{
					uint32_t* dst = reinterpret_cast<uint32_t*>(wip.indexBuffer.data());
					dst[localIndexCursor++] = va;
					dst[localIndexCursor++] = vb;
					dst[localIndexCursor++] = vc;
				}
				else
				{
					uint16_t* dst = reinterpret_cast<uint16_t*>(wip.indexBuffer.data());
					dst[localIndexCursor++] = static_cast<uint16_t>(va);
					dst[localIndexCursor++] = static_cast<uint16_t>(vb);
					dst[localIndexCursor++] = static_cast<uint16_t>(vc);
				}
			}
			result.push_back(std::move(wip));
		}
		return result;
	}

	// 基于 megaMeshlet（索引并集）计算包围球；positions 的前 12 字节为 float3 位置
	static void ComputeMegaMeshletBounds(
		const std::vector<uint32_t>& megaMeshlet,
		const float* positions,
		uint16_t vertexStride,
		float outBounds[4],          // {cx, cy, cz, r}
		float expandRadius = 0.0f)   // 可选外扩半径（为 0 则不外扩）
	{
		if (megaMeshlet.empty())
		{
			outBounds[0] = outBounds[1] = outBounds[2] = 0.0f;
			outBounds[3] = 0.0f;
			return;
		}

		// 收集唯一顶点到紧凑 float3 数组
		thread_local std::vector<float> localPositions;
		localPositions.clear();
		localPositions.reserve(megaMeshlet.size()); // 近似保留，减少重分配

		// 用哈希表做 old->local 映射，避免全局 visited = vertexCount 的巨型数组
		std::unordered_map<uint32_t, uint32_t> old2new;
		old2new.reserve(megaMeshlet.size() / 2 + 1);

		for (uint32_t gi : megaMeshlet)
		{
			auto it = old2new.find(gi);
			if (it == old2new.end())
			{
				const uint8_t* base = reinterpret_cast<const uint8_t*>(positions) + size_t(gi) * vertexStride;
				const float* p = reinterpret_cast<const float*>(base); // 要求位置在前 12 字节
				localPositions.push_back(p[0]);
				localPositions.push_back(p[1]);
				localPositions.push_back(p[2]);
				old2new.emplace(gi, static_cast<uint32_t>(old2new.size()));
			}
		}

		// 可选：为每个点添加统一半径进行保守外扩
		const float* radiiPtr = nullptr;
		size_t radiiStride = 0;
		thread_local std::vector<float> localRadii;
		if (expandRadius > 0.0f)
		{
			localRadii.assign(localPositions.size() / 3, expandRadius);
			radiiPtr = localRadii.data();
			radiiStride = sizeof(float);
		}

		meshopt_Bounds sb = meshopt_computeSphereBounds(
			localPositions.data(),
			localPositions.size() / 3,
			sizeof(float) * 3,
			radiiPtr,
			radiiStride);

		outBounds[0] = sb.center[0];
		outBounds[1] = sb.center[1];
		outBounds[2] = sb.center[2];
		outBounds[3] = sb.radius;
	}

	// 计算 megaMeshlet 所引用子集顶点的 scale（用于相对误差->绝对误差）
	static float ComputeMegaMeshletScale(
		const std::vector<uint32_t>& megaMeshlet,
		const float* positions,
		uint16_t vertexStride)
	{
		if (megaMeshlet.empty()) return 0.0f;

		thread_local std::vector<float> subsetPositions;
		subsetPositions.clear();
		subsetPositions.reserve(megaMeshlet.size()); // 粗略预留

		// 去重收集唯一顶点
		std::unordered_set<uint32_t> seen;
		seen.reserve(megaMeshlet.size());
		for (uint32_t idx : megaMeshlet)
		{
			if (!seen.insert(idx).second) continue;
			const uint8_t* base = reinterpret_cast<const uint8_t*>(positions) + size_t(idx) * vertexStride;
			const float* p = reinterpret_cast<const float*>(base); // 要求位置在前 12 字节
			subsetPositions.push_back(p[0]);
			subsetPositions.push_back(p[1]);
			subsetPositions.push_back(p[2]);
		}

		// 计算子集 scale（与 Sparse 的“子集 extents”一致）
		float scale = meshopt_simplifyScale(
			subsetPositions.data(),
			subsetPositions.size() / 3,
			sizeof(float) * 3);

		return scale;
	}

	static void BuildNaniteLODDAG(
		const std::vector<uint32_t>& indices32,
		const float* positions,
		PreMeshletInfo& outInfo,
		const NaniteLODConfig& cfg = NaniteLODPresets::Default())
	{
		uint32_t nextMeshletId = 0;
		std::vector<MeshletWIP*> currentLevel;

		const uint32_t initialTriCount = (uint32_t)(indices32.size() / 3);

		if (cfg.enableDetailedLogging)
		{
			Utility::Printf("========================================\n");
			Utility::Printf("Building Nanite LOD dag\n");
			Utility::Printf("  Initial triangles: %u\n", initialTriCount);
			Utility::Printf("  Config:\n");
			Utility::Printf("    Max LODs: %zu\n", cfg.maxLods);
			Utility::Printf("    Group size: %zu\n", cfg.groupSize);
			Utility::Printf("    Meshlet max verts: %zu\n", cfg.meshletMaxVertices);
			Utility::Printf("    Meshlet max tris: %zu\n", cfg.meshletMaxTriangles);
			Utility::Printf("    Target error: %.4f\n", cfg.simplificationTargetError);
			Utility::Printf("========================================\n");
		}

		// ========== LOD 0: 创建底层 meshlets ==========
		std::atomic<uint32_t> atomicNextMeshletId(nextMeshletId);
		{
			float mockBounds[4] = { 0, 0, 0, 1.0f };
			auto bottomMeshlets = SplitIntoMeshletsConcurrent(
				indices32, 0, atomicNextMeshletId, 0.0f, 0.0f, mockBounds,
				positions, outInfo.vertexStride, outInfo.vertexCount,
				outInfo.index32, cfg);

			if (cfg.enableDetailedLogging)
			{
				Utility::Printf("  LOD 0: %zu meshlets, %u triangles\n",
					bottomMeshlets.size(), initialTriCount);
			}

			for (auto& m : bottomMeshlets)
			{
				outInfo.allMeshlets.push_back(std::move(m));
				currentLevel.push_back(&outInfo.allMeshlets.back());
			}

		}

		// ========== LOD 1-N: 迭代简化 ==========
		float targetError = cfg.simplificationTargetError;
		uint32_t lastTriangleCount = initialTriCount;
		uint32_t currentTriangleCount = initialTriCount;

		for (size_t lod = 1; lod < cfg.maxLods && currentLevel.size() > 1; ++lod)
		{
			// 停止条件：层间简化率不足
			if (lod > 1)
			{
				float simplificationBetweenLevels =
					1.0f - (float(currentTriangleCount) / lastTriangleCount);
				float requiredSimplification = 1.0f - cfg.simplificationFactorRequirementBetweenLevels;

				if (simplificationBetweenLevels < requiredSimplification)
				{
					if (cfg.enableDetailedLogging)
					{
						Utility::Printf("  LOD %zu: Simplification too slow (%.1f%% < %.1f%%), stopping.\n",
							lod, simplificationBetweenLevels * 100.0f,
							requiredSimplification * 100.0f);
					}
					break;
				}
			}

			lastTriangleCount = currentTriangleCount;
			
			std::vector<MeshletWIP*> nextLevel;
			std::vector<std::vector<MeshletWIP*>> partitionedGroups =
				PartitionMeshletsByMetis(currentLevel, outInfo.index32, cfg.groupSize, cfg.enablePerGroupLogging);

			struct GroupResult
			{
				std::vector<MeshletWIP> newMeshlets;
				std::vector<uint32_t> rootIds;
				struct ChildUpdate { MeshletWIP* ptr; float parentError; float parentBounds[4]; };
				std::vector<ChildUpdate> childUpdates;
			};

			std::vector<GroupResult> results(partitionedGroups.size());
			std::vector<std::future<void>> tasks;
			tasks.reserve(partitionedGroups.size());

			for (size_t gi = 0; gi < partitionedGroups.size(); ++gi)
			{
				tasks.push_back(std::async(std::launch::async, [&, gi]() {
					const auto& group = partitionedGroups[gi];
					GroupResult local;

					std::vector<uint32_t> megaMeshlet = MergeMeshlets(group, outInfo.index32);
					size_t currentTriCount = megaMeshlet.size() / 3;

					float megaMeshletBounds[4];
					ComputeMegaMeshletBounds(
						megaMeshlet,
						positions,
						outInfo.vertexStride,
						megaMeshletBounds);

					if (currentTriCount <= cfg.minRootTriangles)
					{
						for (auto* child : group)
						{
							GroupResult::ChildUpdate upd;
							upd.ptr = child;
							upd.parentError = std::numeric_limits<float>::infinity();
							std::memcpy(upd.parentBounds, megaMeshletBounds, sizeof(upd.parentBounds));
							local.childUpdates.push_back(upd);
							local.rootIds.push_back(child->id);
						}
						results[gi] = std::move(local);
						return;
					}

					float dynamicRate = CalculateDynamicSimplificationRate(currentTriCount, cfg);
					size_t targetIndexCount = size_t(megaMeshlet.size() * dynamicRate);

					std::vector<uint32_t> simplifiedIndices(megaMeshlet.size());
					float resultError = 0.0f;

					size_t resultCount = meshopt_simplify(
						simplifiedIndices.data(),
						megaMeshlet.data(), megaMeshlet.size(),
						positions, outInfo.vertexCount, outInfo.vertexStride,
						targetIndexCount, targetError,
						meshopt_SimplifyLockBorder | meshopt_SimplifySparse,
						&resultError);

					float errorScale = ComputeMegaMeshletScale(megaMeshlet, positions, outInfo.vertexStride);
					resultError *= errorScale;

					simplifiedIndices.resize(resultCount);

					meshopt_optimizeVertexCache(
						simplifiedIndices.data(),
						simplifiedIndices.data(),
						simplifiedIndices.size(),
						outInfo.vertexCount);

					meshopt_optimizeOverdraw(
						simplifiedIndices.data(),
						simplifiedIndices.data(),
						simplifiedIndices.size(),
						positions,
						outInfo.vertexCount,
						outInfo.vertexStride,
						1.05f);

					// 计算误差与合并包围球
					float groupMaxError = 0.0f;
					for (auto* m : group)
					{
						groupMaxError = std::max(groupMaxError, m->maxSiblingsError);
					}

					float lodError = std::max(resultError, 1e-9f);
					float totalError = lodError + groupMaxError;
					// 拆分新 meshlets（并发版本）
					auto newMeshlets = SplitIntoMeshletsConcurrent(
						simplifiedIndices, (uint32_t)lod, atomicNextMeshletId,
						lodError, totalError, megaMeshletBounds,
						positions, outInfo.vertexStride, outInfo.vertexCount,
						outInfo.index32, cfg);

					// 延迟对子 meshlet 的 parent 写入
					for (auto* childMeshlet : group)
					{
						GroupResult::ChildUpdate upd;
						upd.ptr = childMeshlet;
						upd.parentError = totalError;
						std::memcpy(upd.parentBounds, megaMeshletBounds, sizeof(upd.parentBounds));
						local.childUpdates.push_back(upd);
					}

					local.newMeshlets = std::move(newMeshlets);
					results[gi] = std::move(local);
				}));
			}

			// 等待任务
			for (auto& f : tasks) f.get();

			// 串行合并结果
			for (auto& r : results)
			{
				for (auto& upd : r.childUpdates)
				{
					upd.ptr->parentError = upd.parentError;
					std::memcpy(upd.ptr->parentBounds, upd.parentBounds, sizeof(upd.parentBounds));
				}
				for (auto rid : r.rootIds)
					outInfo.rootMeshletIds.push_back(rid);
				for (auto& nm : r.newMeshlets)
				{
					outInfo.allMeshlets.push_back(std::move(nm));
					nextLevel.push_back(&outInfo.allMeshlets.back());
				}
			}

			currentLevel = nextLevel;
			targetError *= cfg.simplificationTargetErrorMultiplier;

			currentTriangleCount = 0;
			for (auto* m : currentLevel)
				currentTriangleCount += m->indexCount / 3;
			
			if (cfg.enableDetailedLogging)
			{
				float percentOfOriginal = 100.0f * currentTriangleCount / initialTriCount;
				Utility::Printf("  LOD %zu: %u triangles (%.1f%%), %zu meshlets\n",
					lod, currentTriangleCount, percentOfOriginal, currentLevel.size());
			}
		}

		// 最后一层级标记为根节点
		if (outInfo.rootMeshletIds.empty())
		{
			for (auto* m : currentLevel)
			{
				m->parentError = std::numeric_limits<float>::infinity();
				outInfo.rootMeshletIds.push_back(m->id);
			}
		}

		// 统计信息
		if (cfg.enableDetailedLogging)
		{
			std::map<uint8_t, uint32_t> lodStats;
			for (const auto& m : outInfo.allMeshlets)
				lodStats[(uint8_t)m.lodLevel]++;

			Utility::Printf("========================================\n");
			Utility::Printf("Nanite LOD dag built successfully\n");
			Utility::Printf("  Total meshlets: %zu\n", outInfo.allMeshlets.size());
			Utility::Printf("  Root meshlets: %zu\n", outInfo.rootMeshletIds.size());
			Utility::Printf("  LOD distribution:\n");

			for (const auto& [lod, count] : lodStats)
			{
				uint32_t tris = 0;
				for (const auto& m : outInfo.allMeshlets)
					if (m.lodLevel == lod) tris += m.indexCount / 3;

				float percentOfOriginal = 100.0f * tris / initialTriCount;
				Utility::Printf("    LOD %u: %u meshlets, %u triangles (%.2f%%)\n",
					lod, count, tris, percentOfOriginal);
			}
			Utility::Printf("========================================\n");
		}
	}

} // namespace Renderer

static inline Vector3 SafeNormalize(Vector3 x)
{
    float lenSq = LengthSquare(x);
    return lenSq < 1e-10f ? Vector3(kXUnitVector) : x * RecipSqrt(lenSq);
}

void Renderer::CompileMesh(
    std::vector<Mesh*>& meshList,
    std::vector<unsigned char>& bufferMemory,
    glTF::Mesh& srcMesh,
    uint32_t matrixIdx,
    const Matrix4& localToObject,
    BoundingSphere& boundingSphere,
    AxisAlignedBox& boundingBox
    )
{
	NaniteLODConfig cfg = NaniteLODPresets::Default();

    // We still have a lot of work to do.  Now that we know about all of the primitives in this mesh
    // and have standardized their vertex buffer streams, we must set out to identify which primitives
    // have the same vertex format and material.  These can share a PSO and Vertex/Index buffer views.
    // There may be more than one draw call per group due to 16-bit indices.

    size_t totalVertexSize = 0;
    size_t totalDepthVertexSize = 0;
    size_t totalIndexSize = 0;

    BoundingSphere sphereOS(kZero);
    AxisAlignedBox bboxOS(kZero);

    std::vector<Primitive> primitives(srcMesh.primitives.size());
    for (uint32_t i = 0; i < primitives.size(); ++i)
    {
        OptimizeMesh(primitives[i], srcMesh.primitives[i], localToObject);
        sphereOS = sphereOS.Union(primitives[i].m_BoundsOS);
        bboxOS.AddBoundingBox(primitives[i].m_BBoxOS);
    }

    boundingSphere = sphereOS;
    boundingBox = bboxOS;

    std::map<uint32_t, std::vector<Primitive*>> renderMeshes;
    for (auto& prim : primitives)
    {
        uint32_t hash = prim.hash;
        renderMeshes[hash].push_back(&prim);
        totalVertexSize += prim.VB->size();
        totalDepthVertexSize += prim.DepthVB->size();
        totalIndexSize += Math::AlignUp(prim.IB->size(), 4);
    }

	//(uint32_t)(totalVertexSize + totalDepthVertexSize + totalIndexSize);

    //Utility::ByteArray stagingBuffer;
    //stagingBuffer.reset(new std::vector<unsigned char>(totalBufferSize));
    //uint8_t* uploadMem = reinterpret_cast<uint8_t*>(stagingBuffer->data());

	uint32_t curVBOffset = 0;
    for (auto& iter : renderMeshes)
    {
		std::vector<Renderer::PreMeshletInfo> preInfo;
		preInfo.reserve(iter.second.size());

		size_t vbSize = 0;
		size_t vbDepthSize = 0;
		size_t ibSize = 0;
		size_t totalDrawsAfterSplit = 0;
		uint32_t totalBufferSize = 0;

		// Compute local space bounding sphere for all submeshes
		BoundingSphere collectiveSphere(kZero);

        for (Primitive* draw : iter.second)
        {
            PreMeshletInfo info{};
            info.index32 = (draw->index32 != 0);
			ASSERT(info.index32);
            info.vertexStride = draw->vertexStride;
            info.vertexCount = static_cast<uint32_t>(draw->VB->size() / draw->vertexStride);
            info.VB = draw->VB;
            info.DepthVB = draw->DepthVB;

            std::vector<uint32_t> indices32;
			indices32.resize(draw->primCount);

            if (info.index32)
            {
                const uint32_t* src = reinterpret_cast<const uint32_t*>(draw->IB->data());
                std::memcpy(indices32.data(), src, draw->primCount * sizeof(uint32_t));
            }
            else
            {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(draw->IB->data());
                for (uint32_t i = 0; i < draw->primCount; ++i)
                    indices32[i] = uint32_t(src[i]);
            }

			const float* positions = reinterpret_cast<const float*>(draw->VB->data());

			BuildNaniteLODDAG(indices32, positions, info, cfg);

			// 汇总统计
			vbSize += draw->VB->size();
			vbDepthSize += draw->DepthVB->size();

			// 计算所有 LOD 的 IB 总大小
			for (const auto& m : info.allMeshlets)
				ibSize += m.indexBuffer.size();

			collectiveSphere = collectiveSphere.Union(draw->m_BoundsLS);
			totalDrawsAfterSplit += info.allMeshlets.size();

			preInfo.push_back(std::move(info));
        }

		ibSize = Math::AlignUp(ibSize, 4);
		totalBufferSize = (uint32_t)(vbSize + vbDepthSize + ibSize);

		Utility::ByteArray stagingBuffer;
		stagingBuffer.reset(new std::vector<unsigned char>(totalBufferSize));
		uint8_t* uploadMem = reinterpret_cast<uint8_t*>(stagingBuffer->data());
		
        // 分配 Mesh（使用 meshlet 粒度的 draw 数）
        size_t numDraws = totalDrawsAfterSplit;
        Mesh* mesh = (Mesh*)malloc(sizeof(Mesh) + sizeof(Mesh::Draw) * (numDraws - 1));

        mesh->bounds[0] = collectiveSphere.GetCenter().GetX();
        mesh->bounds[1] = collectiveSphere.GetCenter().GetY();
        mesh->bounds[2] = collectiveSphere.GetCenter().GetZ();
        mesh->bounds[3] = collectiveSphere.GetRadius();
        mesh->vbOffset = (uint32_t)bufferMemory.size();
        mesh->vbSize = (uint32_t)vbSize;
        mesh->vbDepthOffset = (uint32_t)(bufferMemory.size() + vbSize);
        mesh->vbDepthSize = (uint32_t)vbDepthSize;
        mesh->ibOffset = (uint32_t)(bufferMemory.size() + vbSize + vbDepthSize);
        mesh->ibSize = (uint32_t)ibSize;
        mesh->vbStride = (uint8_t)iter.second[0]->vertexStride;
        mesh->ibFormat = uint8_t(iter.second[0]->index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT);
        mesh->meshCBV = (uint16_t)matrixIdx;
        mesh->materialCBV = iter.second[0]->materialIdx;
        mesh->psoFlags = iter.second[0]->psoFlags;
        mesh->pso = 0xFFFF;
        if (srcMesh.skin >= 0)
        {
            mesh->numJoints = 0xFFFF;
            mesh->startJoint = (uint16_t)srcMesh.skin;
        }
        else
        {
            mesh->numJoints = 0;
            mesh->startJoint = 0xFFFF;
        }

        mesh->numDraws = (uint16_t)numDraws;

		// ===== 填充 LOD 层级信息 =====
		std::map<uint8_t, uint32_t> lodCounts;
		for (const auto& preMesh : preInfo)
		{
			for (const auto& m : preMesh.allMeshlets)
				lodCounts[(uint8_t)m.lodLevel]++;
		}

		mesh->numLODs = (uint8_t)lodCounts.size();
		mesh->numRootMeshlets = 0;
		for (const auto& preMesh : preInfo)
			mesh->numRootMeshlets += (uint16_t)preMesh.rootMeshletIds.size();

		// 填充 lodOffsets
		//uint32_t offset = 0;
		//for (uint8_t lod = 0; lod < mesh->numLODs; ++lod)
		//{
		//	mesh->lodOffsets[lod] = offset;
		//	auto it = lodCounts.find(lod);
		//	if (it != lodCounts.end())
		//		offset += it->second;
		//}

		// ========== 填充 Draw ==========
        uint32_t drawIdx = 0;
        uint32_t curPrimVBOffset = 0;
        uint32_t curVertOffset = 0;
        uint32_t curMeshletIBOffset = 0;
        uint32_t curIndexOffset = 0;
        uint32_t curPrimDepthVBOffset = 0;

		for (size_t iPrim = 0; iPrim < preInfo.size(); ++iPrim)
		{
			const PreMeshletInfo& info = preInfo[iPrim];

			// 按 LOD 层级排序
			std::vector<const MeshletWIP*> sorted;
			for (const auto& m : info.allMeshlets)
				sorted.push_back(&m);
			std::sort(sorted.begin(), sorted.end(),
				[](const MeshletWIP* a, const MeshletWIP* b) {
					if (a->lodLevel != b->lodLevel)
						return a->lodLevel < b->lodLevel;
					return a->id < b->id;
				});

			for (const auto* m : sorted)
			{
				Mesh::Draw& d = mesh->draw[drawIdx];

				d.primCount = m->indexCount;
				d.baseVertex = curVertOffset;
				d.startIndex = curIndexOffset;
				std::memcpy(d.bounds, m->bounds, sizeof(d.bounds));

				// LOD 数据
				d.parentError = m->parentError;
				std::memcpy(d.parentBounds, m->parentBounds, sizeof(d.parentBounds));
				d.maxSiblingsError = m->maxSiblingsError;
				std::memcpy(d.shareSiblingsBounds, m->shareSiblingsBounds, sizeof(d.shareSiblingsBounds));
				d.lodLevel = (uint8_t)m->lodLevel;

				++drawIdx;
				curIndexOffset += m->indexCount;
			}

			// 拷贝 VB
			std::memcpy(uploadMem + curPrimVBOffset,
				info.VB->data(), info.VB->size());
			curPrimVBOffset += (uint32_t)info.VB->size();
			curVertOffset += (uint32_t)(info.VB->size() / info.vertexStride);

			// 拷贝 DepthVB
			std::memcpy(uploadMem + vbSize + curPrimDepthVBOffset,
				info.DepthVB->data(), info.DepthVB->size());
			curPrimDepthVBOffset += (uint32_t)info.DepthVB->size();

			// 拷贝所有 LOD 的 IB
			for (const auto* m : sorted)
			{
				std::memcpy(uploadMem + vbSize + vbDepthSize + curMeshletIBOffset,
					m->indexBuffer.data(), m->indexBuffer.size());
				curMeshletIBOffset += (uint32_t)m->indexBuffer.size();
			}
		}

        meshList.push_back(mesh);

		bufferMemory.insert(bufferMemory.end(), stagingBuffer->begin(), stagingBuffer->end());

		curVBOffset += totalBufferSize;
    }
}


static uint32_t WalkGraph(
    std::vector<GraphNode>& sceneGraph,
    BoundingSphere& modelBSphere,
    AxisAlignedBox& modelBBox,
    std::vector<Mesh*>& meshList,
    std::vector<unsigned char>& bufferMemory,
    std::vector<CameraData>& cameraData,
    const std::vector<glTF::Node*>& siblings,
    uint32_t curPos,
    const Matrix4& xform
    )
{
    size_t numSiblings = siblings.size();

    for (size_t i = 0; i < numSiblings; ++i)
    {
        glTF::Node* curNode = siblings[i];
        GraphNode& thisGraphNode = sceneGraph[curPos];
        thisGraphNode.hasChildren = 0;
        thisGraphNode.hasSibling = 0;
        thisGraphNode.matrixIdx = curPos;
        thisGraphNode.skeletonRoot = curNode->skeletonRoot;
        curNode->linearIdx = curPos;

        // They might not be used, but we have space to hold the neutral values which could be
        // useful when updating the matrix via animation.
        std::memcpy((float*)&thisGraphNode.scale, curNode->scale, sizeof(curNode->scale));
        std::memcpy((float*)&thisGraphNode.rotation, curNode->rotation, sizeof(curNode->rotation));

        if (curNode->hasMatrix)
        {
            std::memcpy((float*)&thisGraphNode.xform, curNode->matrix, sizeof(curNode->matrix));
        }
        else
        {
            thisGraphNode.xform = Matrix4(
                Matrix3(thisGraphNode.rotation) * Matrix3::MakeScale(thisGraphNode.scale),
                Vector3(*(const XMFLOAT3*)curNode->translation)
            );
        }

        const Matrix4 LocalXform = xform * thisGraphNode.xform;

        if (!curNode->pointsToCamera && curNode->mesh != nullptr)
        {
            BoundingSphere sphereOS;
            AxisAlignedBox boxOS;
            CompileMesh(meshList, bufferMemory, *curNode->mesh, curPos, LocalXform, sphereOS, boxOS);
            modelBSphere = modelBSphere.Union(sphereOS);
            modelBBox.AddBoundingBox(boxOS);
        }
        else if (curNode->pointsToCamera && curNode->camera != nullptr)
        {
            CameraData camera;
			camera.aspectRatio = curNode->camera->aspectRatio;
			camera.yfov = curNode->camera->yfov;
			camera.znear = curNode->camera->znear;
			camera.zfar = curNode->camera->zfar;
            camera.matrixIdx = curPos;
            camera.type = curNode->camera->type == glTF::Camera::kPerspective ? CameraData::kPerspective : CameraData::kOrthographic;
            cameraData.emplace_back(camera);
        }

        uint32_t nextPos = curPos + 1;

        if (curNode->children.size() > 0)
        {
            thisGraphNode.hasChildren = 1;
            nextPos = WalkGraph(sceneGraph, modelBSphere, modelBBox, meshList, bufferMemory, cameraData, curNode->children, nextPos, LocalXform);
        }

        // Are there more siblings?
        if (i + 1 < numSiblings)
        {
            thisGraphNode.hasSibling = 1;
        }
        
        curPos = nextPos;
    }

    return curPos;
}

inline void CompileTexture(const std::wstring& basePath, const std::string& fileName, uint8_t flags)
{
    CompileTextureOnDemand(basePath + Utility::UTF8ToWideString(fileName), flags);
}

inline void SetTextureOptions(std::map<std::string, uint8_t>& optionsMap, glTF::Texture* texture, uint8_t options)
{
    if (texture && texture->source && optionsMap.find(texture->source->path) == optionsMap.end())
        optionsMap[texture->source->path] = options;
}

void BuildMaterials(ModelData& model, const glTF::Asset& asset)
{
    //static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

    // Replace texture filename extensions with "DDS" in the string table
    model.m_TextureNames.resize(asset.m_images.size());
    for (size_t i = 0; i < asset.m_images.size(); ++i)
        model.m_TextureNames[i] = asset.m_images[i].path;

    std::map<std::string, uint8_t> textureOptions;

    const uint32_t numMaterials = (uint32_t)asset.m_materials.size();

    model.m_MaterialConstants.resize(numMaterials);
    model.m_MaterialTextures.resize(numMaterials);

    for (uint32_t i = 0; i < numMaterials; ++i)
    {
        const glTF::Material& srcMat = asset.m_materials[i];

        MaterialConstantData& material = model.m_MaterialConstants[i];
        material.baseColorFactor[0] = srcMat.baseColorFactor[0];
        material.baseColorFactor[1] = srcMat.baseColorFactor[1];
        material.baseColorFactor[2] = srcMat.baseColorFactor[2];
        material.baseColorFactor[3] = srcMat.baseColorFactor[3];
        material.emissiveFactor[0] = srcMat.emissiveFactor[0];
        material.emissiveFactor[1] = srcMat.emissiveFactor[1];
        material.emissiveFactor[2] = srcMat.emissiveFactor[2];
        material.normalTextureScale = srcMat.normalTextureScale;
        material.metallicFactor = srcMat.metallicFactor;
        material.roughnessFactor = srcMat.roughnessFactor;
        material.flags = srcMat.flags;

        MaterialTextureData& dstMat = model.m_MaterialTextures[i];
        dstMat.addressModes = 0;

        for (uint32_t ti = 0; ti < kNumTextures; ++ti)
        {
            dstMat.stringIdx[ti] = 0xFFFF;

            if (srcMat.textures[ti] != nullptr)
            {
                if (srcMat.textures[ti]->source != nullptr)
                {
                    dstMat.stringIdx[ti] = uint16_t(srcMat.textures[ti]->source - asset.m_images.data());
                }

                if (srcMat.textures[ti]->sampler != nullptr)
                {
                    dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapS << (ti * 4);
                    dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapT << (ti * 4 + 2);
                }
                else
                {
                    dstMat.addressModes |= 0x5 << (ti * 4);
                }
            }
            else
            {
                dstMat.addressModes |= 0x5 << (ti * 4);
            }
        }

        SetTextureOptions(textureOptions, srcMat.textures[kBaseColor], TextureOptions(true, srcMat.alphaBlend | srcMat.alphaTest));
        SetTextureOptions(textureOptions, srcMat.textures[kMetallicRoughness], TextureOptions(false));
        SetTextureOptions(textureOptions, srcMat.textures[kOcclusion], TextureOptions(false));
        SetTextureOptions(textureOptions, srcMat.textures[kEmissive], TextureOptions(true));
        SetTextureOptions(textureOptions, srcMat.textures[kNormal], TextureOptions(false));
    }

    model.m_TextureOptions.clear();
    for (auto name : model.m_TextureNames)
    {
        auto iter = textureOptions.find(name);
        if (iter != textureOptions.end())
        {
            model.m_TextureOptions.push_back(iter->second);
            CompileTextureOnDemand(asset.m_basePath + Utility::UTF8ToWideString(iter->first), iter->second);
        }
        else
            model.m_TextureOptions.push_back(0xFF);
    }
    ASSERT(model.m_TextureOptions.size() == model.m_TextureNames.size());
}

void BuildAnimations(ModelData& model, const glTF::Asset& asset)
{
    size_t numAnimations = asset.m_animations.size();
    if (numAnimations == 0)
        return;

    model.m_Animations.resize(numAnimations);
    uint32_t animIdx = 0;

    for (const glTF::Animation& anim : asset.m_animations)
    {
        AnimationSet& animSet = model.m_Animations[animIdx++];
        animSet.duration = 0.0f;
        animSet.firstCurve = (uint32_t)model.m_AnimationCurves.size();
        animSet.numCurves = (uint32_t)anim.m_channels.size();

        for (size_t i = 0; i < animSet.numCurves; ++i)
        {
            const glTF::AnimChannel& channel = anim.m_channels[i];
            const glTF::AnimSampler& sampler = *channel.m_sampler;

            ASSERT(channel.m_target->linearIdx >= 0);

            AnimationCurve curve;
            curve.targetNode = channel.m_target->linearIdx;
            curve.targetPath = channel.m_path;
            curve.interpolation = sampler.m_interpolation;
            curve.keyFrameOffset = model.m_AnimationKeyFrameData.size();
            curve.keyFrameFormat = std::min<uint32_t>(sampler.m_output->componentType, AnimationCurve::kFloat);
            curve.numSegments = sampler.m_output->count - 1.0f;

            // In glTF, stride==0 means "packed tightly"
            if (sampler.m_output->stride == 0)
            {
                uint32_t numComponents = sampler.m_output->type + 1;
                uint32_t bytesPerComponent = sampler.m_output->componentType / 2 + 1;
                curve.keyFrameStride = numComponents * bytesPerComponent / 4;
            }
            else
            {
                ASSERT(sampler.m_output->stride <= 16 && sampler.m_output->stride % 4 == 0);
                curve.keyFrameStride = sampler.m_output->stride / 4;
            }

            // Determine start and stop time stamps
            const float* timeStamps = (float*)sampler.m_input->dataPtr;
            curve.startTime = timeStamps[0];

            const float endTime = timeStamps[sampler.m_output->count - 1];
            curve.rangeScale = curve.numSegments / (endTime - curve.startTime);

            animSet.duration = std::max<float>(animSet.duration, endTime);

            // Append this curve data
            model.m_AnimationKeyFrameData.insert(
                model.m_AnimationKeyFrameData.end(),
                sampler.m_output->dataPtr,
                sampler.m_output->dataPtr + sampler.m_output->count * curve.keyFrameStride * 4);

            model.m_AnimationCurves.push_back(curve);
        }
    }
}

void BuildSkins(ModelData& model, const glTF::Asset& asset)
{
    size_t numSkins = asset.m_skins.size();
    if (numSkins == 0)
        return;

    std::vector<std::pair<uint16_t, uint16_t>> skinMap;
    skinMap.reserve(asset.m_skins.size());

    for (const glTF::Skin& skin : asset.m_skins)
    {
        // Record offset and joint count
        uint16_t numJoints = (uint16_t)skin.joints.size();
        uint16_t curOffset = (uint16_t)model.m_JointIndices.size();
        skinMap.push_back(std::make_pair(curOffset, numJoints));

        // Append remapped joint indices
        for (glTF::Node* joint : skin.joints)
        {
            ASSERT(joint->linearIdx >= 0, "Skin joint not present in node hierarchy");
            model.m_JointIndices.push_back((uint16_t)joint->linearIdx);
        }

        // Append IBMs
        Matrix4* IBMstart = (Matrix4*)skin.inverseBindMatrices->dataPtr;
        Matrix4* IBMend = IBMstart + skin.inverseBindMatrices->count;
        ASSERT(skin.inverseBindMatrices->count == numJoints);
        model.m_JointIBMs.insert(model.m_JointIBMs.end(), IBMstart, IBMend);
    }

    // Assign skinned meshes the proper joint offset and count
    for (Mesh* mesh : model.m_Meshes)
    {
        if (mesh->numJoints != 0)
        {
            std::pair<uint16_t, uint16_t> offsetAndCount = skinMap[mesh->startJoint];
            mesh->startJoint = offsetAndCount.first;
            mesh->numJoints = offsetAndCount.second;
        }
    }
}

bool Renderer::BuildModel(ModelData& model, const glTF::Asset& asset, int sceneIdx)
{
    BuildMaterials(model, asset);

    // Generate scene graph and meshes
    model.m_SceneGraph.resize(asset.m_nodes.size());
    const glTF::Scene* scene = sceneIdx < 0 ? asset.m_scene : &asset.m_scenes[sceneIdx];
    if (scene == nullptr)
        return false;

    // Aggregate all of the vertex and index buffers in this unified buffer
    std::vector<unsigned char>& bufferMemory = model.m_GeometryData;

    model.m_BoundingSphere = BoundingSphere(kZero);
    model.m_BoundingBox = AxisAlignedBox(kZero);
    uint32_t numNodes = WalkGraph(model.m_SceneGraph, model.m_BoundingSphere, model.m_BoundingBox, model.m_Meshes, bufferMemory, model.m_Cameras, scene->nodes, 0, Matrix4(kIdentity));
    model.m_SceneGraph.resize(numNodes);

    BuildAnimations(model, asset);
    BuildSkins(model, asset);

    return true;
}

bool Renderer::SaveModel(const std::wstring& filePath, const ModelData& data)
{
    std::ofstream outFile(filePath, std::ios::out | std::ios::binary);
    if (!outFile)
        return false;

    FileHeader header;
    std::memcpy(header.id, "MINI", 4);
    header.version = CURRENT_MINI_FILE_VERSION;
    header.numNodes = (uint32_t)data.m_SceneGraph.size();
    header.numMeshes = (uint32_t)data.m_Meshes.size();
    header.numMaterials = (uint32_t)data.m_MaterialConstants.size();
    header.meshDataSize = 0;
    for (const Mesh* mesh : data.m_Meshes)
        header.meshDataSize += (uint32_t)sizeof(Mesh) + (mesh->numDraws - 1) * (uint32_t)sizeof(Mesh::Draw);
    header.numTextures = (uint32_t)data.m_TextureNames.size();
    header.stringTableSize = 0;
    for (const std::string& str : data.m_TextureNames)
        header.stringTableSize += (uint32_t)str.size() + 1;
    header.geometrySize = (uint32_t)data.m_GeometryData.size();
    header.keyFrameDataSize = (uint32_t)data.m_AnimationKeyFrameData.size();
    header.numAnimationCurves = (uint32_t)data.m_AnimationCurves.size();
    header.numAnimations = (uint32_t)data.m_Animations.size();
    header.numJoints = (uint32_t)data.m_JointIndices.size();
    header.numCameras = (uint32_t)data.m_Cameras.size();
    header.boundingSphere[0] = data.m_BoundingSphere.GetCenter().GetX();
    header.boundingSphere[1] = data.m_BoundingSphere.GetCenter().GetY();
    header.boundingSphere[2] = data.m_BoundingSphere.GetCenter().GetZ();
    header.boundingSphere[3] = data.m_BoundingSphere.GetRadius();
    header.minPos[0] = data.m_BoundingBox.GetMin().GetX();
    header.minPos[1] = data.m_BoundingBox.GetMin().GetY();
    header.minPos[2] = data.m_BoundingBox.GetMin().GetZ();
    header.maxPos[0] = data.m_BoundingBox.GetMax().GetX();
    header.maxPos[1] = data.m_BoundingBox.GetMax().GetY();
    header.maxPos[2] = data.m_BoundingBox.GetMax().GetZ();

    outFile.write((char*)&header, sizeof(FileHeader));
    outFile.write((char*)data.m_GeometryData.data(), header.geometrySize);
    outFile.write((char*)data.m_SceneGraph.data(), header.numNodes * sizeof(GraphNode));
    for (const Mesh* mesh : data.m_Meshes)
        outFile.write((char*)mesh, sizeof(Mesh) + (mesh->numDraws - 1) * sizeof(Mesh::Draw));
    outFile.write((char*)data.m_MaterialConstants.data(), header.numMaterials * sizeof(MaterialConstantData));
    outFile.write((char*)data.m_MaterialTextures.data(), header.numMaterials * sizeof(MaterialTextureData));
    for (uint32_t i = 0; i < header.numTextures; ++i)
        outFile << data.m_TextureNames[i] << '\0';
    outFile.write((char*)data.m_TextureOptions.data(), header.numTextures * sizeof(uint8_t));

    if (header.numAnimations > 0)
    {
        ASSERT(header.keyFrameDataSize > 0 && header.numAnimationCurves > 0);
        outFile.write((char*)data.m_AnimationKeyFrameData.data(), header.keyFrameDataSize);
        outFile.write((char*)data.m_AnimationCurves.data(), header.numAnimationCurves * sizeof(AnimationCurve));
        outFile.write((char*)data.m_Animations.data(), header.numAnimations * sizeof(AnimationSet));
    }
    else
    {
        ASSERT(header.keyFrameDataSize == 0 && header.numAnimationCurves == 0);
    }

    if (header.numJoints)
    {
        ASSERT(header.numJoints == (uint32_t)data.m_JointIBMs.size());
        outFile.write((char*)data.m_JointIndices.data(), header.numJoints * sizeof(uint16_t));
        outFile.write((char*)data.m_JointIBMs.data(), header.numJoints * sizeof(Matrix4));
    }

    if (header.numCameras)
    {
        outFile.write((char*)data.m_Cameras.data(), header.numCameras * sizeof(CameraData));
    }

    return true;
}
