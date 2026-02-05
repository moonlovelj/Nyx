#pragma once
#include <vector>
#include <cstdint>
#include <span>
#include <limits>
#include <algorithm>
#include <memory>
#include <cmath>
#include <unordered_map>
#include "MeshletStructs.h"
#include "../Core/Math/Vector.h"

struct MeshletBuildSettings
{
	uint32_t MaxMeshletVertices = 128;
	uint32_t MaxMeshletTriangles = 128;
	uint32_t MinMeshletTriangles = 32;
	uint32_t TargetMeshletsPerGroup = 32;
	uint32_t MaxBVHNodeChildren = 8;

	float ClusterSplitFactor = 2.0f;

	// Simplification targets and failure thresholds
	float TargetSimplifyRatio = 0.5f;
	float SimplificationFailurePercentage = 0.51f;
	float SimplificationSloppyFailurePercentage = 0.85f;
	float minReductionRatio = 0.01f; // Stop simplifying below this ratio

	float LODErrorMergePrevious = 1.5f; // Used to ensure monotonicity

	bool bUseSimplifyPermissive = true;

	bool bSimplifyFallbackSloppy = true;
	float SimplifySloppyErrorFactor = 2.0f;

	bool bOutputDebugInfo = false;
    bool bValidateBuild = false; // optional offline validation for LOD/structure
};

struct GroupPackage
{
	Renderer::GroupMetadata Metadata{};
	std::vector<uint8_t> Blob;
	uint64_t TempFileOffset = 0;
};

struct MeshletBuildProducts
{
	std::vector<GroupPackage> Groups;
	std::vector<Renderer::HierarchyNode> Hierarchy; //[0]-->root
};

struct MeshletBuildArgs
{
	unsigned char* VBData = nullptr;
	unsigned char* IBData = nullptr;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
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
	// Full build pipeline: LOD0 -> grouping -> boundary locking -> group-level simplification -> new LOD -> iterate
	static MeshletBuildProducts Build(
		const MeshletBuildArgs& buildArgs);
private:

	struct Group
	{
		std::vector<uint32_t> MeshletIDs; // Meshlets in this group (indices into current array)
		float GroupSphere[4]{};
		float GroupBBoxMin[3]{};
		float GroupBBoxMax[3]{};
		float ParrentError = 0;
		uint32_t GroupID = 0xFFFFFFFF;
		uint8_t LODLevel = 0xFF;
	};

	struct Meshlet
	{
		std::vector<uint32_t> Vertices; // Original vertex indices
		std::vector<uint8_t>  Triangles;// Local indices
		float BoundSphere[4]{};
		float BBoxMin[3]{};
		float BBoxMax[3]{};
		uint32_t GroupChildIndex = 0xFFFFFFFF; // Group-local child index
		uint32_t GroupID = 0;
		uint32_t RefineGroupID = 0xFFFFFFFF;// No refinement group for LOD0
	};

	// LOD0: build meshlets directly from full model indices
	static std::vector<Meshlet> BuildLOD0Meshlets(
		const MeshletBuildArgs& buildArgs);

	// Build meshlets from an index stream (rebuild after simplification)
	static void BuildMeshletsFromIndices(
		const MeshletBuildArgs& buildArgs,
		const uint32_t* indices, size_t indexCount,
		std::vector<Meshlet>& out);

	// Compute meshlet bounding sphere (use meshopt for accuracy)
	static void ComputeMeshletSphere(
		const MeshletBuildArgs& buildArgs,
		const Meshlet& m,
		float outSphere[4]);

    static void ComputeMeshletAABB(
        const MeshletBuildArgs& buildArgs,
        const Meshlet& m,
        float outMin[3],
        float outMax[3]);

	// Merge two spheres (Ritter merge)
	static void MergeSphere(const float a[4], const float b[4], float out[4]);

	// position-only remap
	static void GeneratePositionRemap(
		const MeshletBuildArgs& buildArgs,
		std::vector<uint32_t>& outPosRemap);

	// Group by shared vertices/spatial proximity using partitionClusters
	static std::vector<uint32_t> GroupMeshlets(
		const MeshletBuildArgs& buildArgs,
		const std::vector<Meshlet>& current,
		std::span<const uint32_t> subsetIds,
		std::span<const uint32_t> posRemap,
		uint32_t LODLevel,
		std::vector<Group>& groups);

	// Boundary locking: lock all original vertices that share position-only vertices across groups
	static void BuildVertexLocksByGroups(
		const std::vector<Group>& groups,
		const std::vector<uint32_t>& groupIds,
		const std::vector<Meshlet>& current,
		const std::vector<uint32_t>& posRemap,
		size_t vertexCount,
		std::vector<unsigned char>& outVertexLock);

	// Group-level simplification: concat group indices -> simplifyWithAttributes (normals + locks) -> rebuild meshlets
	static bool SimplifyGroup(
		const MeshletBuildArgs& buildArgs,
		const Group& g,
		const std::vector<Meshlet>& current,
		const std::vector<unsigned char>& vertexLock, // Global lock array
		std::vector<Meshlet>& outNewMeshlets,
		float& outError);

	// Serialize existing meshlet data into a group blob (reusable)
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

	static bool ValidateBuild(
		const MeshletBuildArgs& buildArgs,
		const std::vector<Group>& groups,
		const std::vector<Meshlet>& meshlets);

	static std::vector<Renderer::HierarchyNode> BuildHierarchy(
		const std::vector<Renderer::HierarchyNode>& initNodes,
		uint32_t maxBVHNodeChildren,
		std::vector<uint32_t>& outInitNodesReorderMap); // outInitNodesReorderMap returns the initial child reorder mapping
};
