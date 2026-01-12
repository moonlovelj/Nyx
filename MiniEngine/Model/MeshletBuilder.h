#pragma once
#include <vector>
#include <cstdint>
#include <span>
#include <limits>
#include <algorithm>
#include <memory>
#include <cmath>
#include <unordered_map>
#include "MeshOptimizer/meshoptimizer.h"
#include "MeshletStructs.h"
#include "../Core/Math/Vector.h"

struct RawVertex
{
	Math::Vector3 Position;
	Math::Vector3 Normal;
	Math::Vector4 Tangent;
	float UV0[2];
	const unsigned char* VertexData; //完整顶点数据（用于最终序列化）
};

struct MeshletBuildSettings
{
	uint32_t MaxMeshletVertices = 256;
	uint32_t MaxMeshletTriangles = 128;
	uint32_t TargetMeshletsPerGroup = 32;
	uint32_t MaxBVHNodeChildren = 8;

	// 简化目标与失败阈值
	float TargetSimplifyRatio = 0.5f;
	float SimplificationFailurePercentage = 0.85f;
	float minReductionRatio = 0.1f; // 低于这个值则停止简化

	float LODErrorMergePrevious = 1.5f; // 用来保证单调

	bool bUseSimplifyPermissive = true;
};

struct GroupPackage
{
	Renderer::GroupMetadata Metadata{};
	std::vector<uint8_t> Blob;
};

struct MeshletBuildProducts
{
	std::vector<GroupPackage> Groups;
	std::vector<Renderer::HierarchyNode> Hierarchy; //[0]-->root
};

struct MeshletBuildArgs
{
	std::span<const RawVertex> vertices;
	std::span<const uint32_t> indices;
	MeshletBuildSettings settings{};
	uint16_t meshBufferIndex = 0;
	uint16_t materialBufferIndex = 0;
	uint16_t psoFlags = 0;
	uint16_t vertexStride = 0;
	uint32_t baseGroupIndex = 0;
	uint32_t baseNodeIndex = 0;
};

class MeshletBuilder
{
public:
	// 完整构建流程：LOD0 -> 分组 -> 锁边 -> 组级简化 -> 新 LOD -> 迭代
	static MeshletBuildProducts Build(
		const MeshletBuildArgs& buildArgs);
private:

	struct Group
	{
		std::vector<uint32_t> MeshletIDs; // 本组包含的 meshlet（指向 current 数组）
		float GroupSphere[4]{};
		float ParrentError = 0;
		uint32_t GroupID = 0xFFFFFFFF;
		uint8_t LODLevel = 0xFF;
	};

	struct Meshlet
	{
		std::vector<uint32_t> Vertices; // 原始顶点索引
		std::vector<uint8_t>  Triangles;// 局部索引
		float BoundSphere[4]{};
		uint32_t GroupChildIndex = 0xFFFFFFFF; // 组内子索引
		uint32_t GroupID = 0;
		uint32_t RefineGroupID = 0xFFFFFFFF;// LOD0没有精细组
	};

	// LOD0：直接对整模型索引构建 meshlet
	static std::vector<Meshlet> BuildLOD0Meshlets(
		std::span<const RawVertex> vertices,
		std::span<const uint32_t> indices,
		const MeshletBuildSettings& s);

	// 从索引流构建一批 meshlet（简化后重建）
	static void BuildMeshletsFromIndices(
		const uint32_t* indices, size_t indexCount,
		std::span<const RawVertex> vertices,
		const MeshletBuildSettings& s,
		std::vector<Meshlet>& out);

	// 计算 meshlet 包围球（调用 meshopt 以保证准确）
	static void ComputeMeshletSphere(
		const Meshlet& m,
		std::span<const RawVertex> vertices,
		float outSphere[4]);

	// 合并两个球（Ritter 合并）
	static void MergeSphere(const float a[4], const float b[4], float out[4]);

	// position-only remap
	static void GeneratePositionRemap(
		std::span<const RawVertex> vertices,
		std::vector<uint32_t>& outPosRemap);

	// 用 partitionClusters 按共享顶点/空间接近分组
	static std::vector<uint32_t> GroupMeshlets(
		const std::vector<Meshlet>& current,
		std::span<const uint32_t> subsetIds,
		std::span<const RawVertex> vertices,
		uint32_t targetGroupSize,
		uint32_t LODLevel,
		std::vector<Group>& groups);

	// 锁边：跨组共享“position-only 顶点”的所有原始顶点上锁
	static void BuildVertexLocksByGroups(
		const std::vector<Group>& groups,
		const std::vector<uint32_t>& groupIds,
		const std::vector<Meshlet>& current,
		const std::vector<uint32_t>& posRemap,
		size_t vertexCount,
		std::vector<unsigned char>& outVertexLock);

	// 组级简化：拼接组索引 -> simplifyWithAttributes(法线+锁) -> 重建 meshlet
	static bool SimplifyGroup(
		const Group& g,
		const std::vector<Meshlet>& current,
		std::span<const RawVertex> vertices,
		const MeshletBuildArgs& buildArgs,
		const std::vector<unsigned char>& vertexLock, // 全局锁数组
		std::vector<Meshlet>& outNewMeshlets,
		float& outError);

	// 用已有 meshlet 数据序列化成 group blob（可单独复用）
	static GroupPackage SerializeGroup(
		const MeshletBuildArgs& buildArgs,
		const Group& group,
		const std::vector<Meshlet>& meshlets,
		const std::unordered_map<uint32_t, uint32_t>& groupIdToOrder);

	// Streaming Group + BVH
	static MeshletBuildProducts BuildStreamingData(
		const MeshletBuildArgs& buildArgs,
		const std::vector<Group>& groups,
		const std::vector<Meshlet>& meshlets);

	static std::vector<Renderer::HierarchyNode> BuildHierarchy(
		const std::vector<Renderer::HierarchyNode>& initNodes,
		uint32_t maxBVHNodeChildren,
		std::vector<uint32_t>& outInitNodesReorderMap); // outInitNodesReorderMap返回初始子节点重排映射
};