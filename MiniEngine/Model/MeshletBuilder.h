#pragma once
#include <vector>
#include <cstdint>
#include <span>
#include <limits>
#include <algorithm>
#include <memory>
#include <cmath>
#include "MeshOptimizer/meshoptimizer.h"
#include "../Core/Math/Vector.h"

struct RawVertex
{
	Math::Vector3 Position;
	Math::Vector3 Normal;
};

struct MeshletBuildSettings
{
	uint32_t MaxMeshletVertices = 256;
	uint32_t MaxMeshletTriangles = 128;
	uint32_t TargetMeshletsPerGroup = 8;

	// 简化目标与失败阈值
	float TargetSimplifyRatio = 0.5f;
	float SimplificationFailurePercentage = 0.60f;
	float minReductionRatio = 0.1f; // 低于这个值则停止简化
};

struct TempMeshlet
{
	std::vector<uint32_t> Vertices;
	std::vector<uint8_t>  Triangles;
	float Sphere[4]{};
	float Error = 0.0f; // LOD0 = 0

	float LodBounds[4]{};
	float LodError = 0.0f;
	float ParentBounds[4]{};
	float ParentError = 0.0f;

	uint32_t LODLevel = 0;
};

class MeshletBuilder
{
public:
	// 完整构建流程：LOD0 -> 分组 -> 锁边 -> 组级简化 -> 新 LOD -> 迭代
	static std::vector<TempMeshlet> Build(
		std::span<const RawVertex> vertices,
		std::span<const uint32_t> indices,
		const MeshletBuildSettings& settings);

private:

	struct Group
	{
		std::vector<uint32_t> MeshletIDs; // 本组包含的 meshlet（指向 current 数组）
		float GroupSphere[4]{};
		float GroupError = 0.0f;
	};

	// LOD0：直接对整模型索引构建 meshlet
	static std::vector<TempMeshlet> BuildLOD0Meshlets(
		std::span<const RawVertex> vertices,
		std::span<const uint32_t> indices,
		const MeshletBuildSettings& s);

	// 从索引流构建一批 meshlet（简化后重建）
	static void BuildMeshletsFromIndices(
		const uint32_t* indices, size_t indexCount,
		std::span<const RawVertex> vertices,
		const MeshletBuildSettings& s,
		std::vector<TempMeshlet>& out);

	// 计算 meshlet 包围球（调用 meshopt 以保证准确）
	static void ComputeMeshletSphere(
		const TempMeshlet& m,
		std::span<const RawVertex> vertices,
		float outSphere[4]);

	// 合并两个球（Ritter 合并）
	static void MergeSphere(const float a[4], const float b[4], float out[4]);

	// position-only remap
	static void GeneratePositionRemap(
		std::span<const RawVertex> vertices,
		std::vector<uint32_t>& outPosRemap);

	// 用 partitionClusters 按共享顶点/空间接近分组
	static std::vector<Group> GroupMeshlets(
		const std::vector<TempMeshlet>& current,
		std::span<const uint32_t> subsetIds,
		std::span<const RawVertex> vertices,
		uint32_t targetGroupSize);

	// 锁边：跨组共享“position-only 顶点”的所有原始顶点上锁
	static void BuildVertexLocksByGroups(
		const std::vector<Group>& groups,
		const std::vector<TempMeshlet>& current,
		const std::vector<uint32_t>& posRemap,
		size_t vertexCount,
		std::vector<unsigned char>& outVertexLock);

	// 组级简化：拼接组索引 -> simplifyWithAttributes(法线+锁) -> 重建 meshlet
	static bool SimplifyGroup(
		const Group& g,
		const std::vector<TempMeshlet>& current,
		std::span<const RawVertex> vertices,
		const MeshletBuildSettings& s,
		const std::vector<unsigned char>& vertexLock, // 全局锁数组
		std::vector<TempMeshlet>& outNewMeshlets,
		float& outError);
};