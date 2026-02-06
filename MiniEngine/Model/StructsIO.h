#pragma once

#ifdef __cplusplus
#include <cstdint>
#endif

#define INVALID_ID 0xFFFFFFFFu

#define NODE_BYTE_STRIDE 8
#define MESHLET_BYTE_STRIDE 8

#define DAG_CULL_GROUP_SIZE 64
#define MAX_BVH_NODE_CHILDREN_BITS_NUM 3
#define MAX_BVH_NODE_CHILDREN_NUM (1 << MAX_BVH_NODE_CHILDREN_BITS_NUM)
#define MAX_BVH_NODES_PER_GROUP (DAG_CULL_GROUP_SIZE >> MAX_BVH_NODE_CHILDREN_BITS_NUM)

#define MAX_NODES 2 * 1024 * 1024

#define MAX_CANDIDATE_MESHLETS 16 * 1024 * 1024
#define MAX_CANDIDATE_MESHLETS_BATCH (MAX_CANDIDATE_MESHLETS / DAG_CULL_GROUP_SIZE )

#define MAX_VISIBLE_MESHLETS 4 * 1024 * 1024

#define MAX_STREAMING_REQUESTS 16384

struct DrawItem
{
#ifdef __cplusplus
	uint32_t InstanceIndex;
	uint32_t NodeIndex;
#else
	uint InstanceIndex;
	uint NodeIndex;
#endif
};

struct VisibleMeshletPayload
{
#ifdef __cplusplus
	uint32_t InstanceIndex;
	uint32_t GroupIndex : 24;
	uint32_t MeshletIndex : 7;
	uint32_t Padding : 1;
#else
	uint InstanceIndex;
	uint PackedData;

	uint GetGroupIndex()
	{
		return PackedData & 0xFFFFFF; // 24 bits
	}

	uint GetMeshletIndex()
	{
		return (PackedData >> 24) & 0x7F; // 7 bits
	}

	uint PackData(uint groupIndex, uint meshletIndex)
	{
		return (meshletIndex << 24) | (groupIndex & 0xFFFFFF);
	}
#endif
};

struct QueuePassState
{
#ifdef __cplusplus
	uint32_t	MeshletBatchReadOffset;
	uint32_t	CandidateMeshletWriteOffset;
	uint32_t	TotalMeshlets;
	uint32_t	NodeReadOffset;
	uint32_t	NodeWriteOffset;
	uint32_t	NodeCount;
	uint32_t	VisibleMeshletCount;
#else
	uint		MeshletBatchReadOffset;
	uint		CandidateMeshletWriteOffset;
	uint		TotalMeshlets;
	uint		NodeReadOffset;
	uint		NodeWriteOffset;
	uint		NodeCount;
	uint		VisibleMeshletCount;
#endif
};

struct QueueState
{
	QueuePassState PassState[2];
};

struct GeometryStreamingRequest
{
#ifdef __cplusplus
	union {
		uint32_t PackedData;
		struct {
			uint32_t GroupIndex : 24;
			uint32_t Priority : 8;
		};
	};
#else
	uint PackedData;
	uint GetGroupIndex()
	{
		return PackedData & 0xFFFFFF; // 24 bits
	}
	uint GetPriority()
	{
		return (PackedData >> 24) & 0xFF; // 8 bits
	}

	uint PackData(uint groupIndex, uint priority)
	{
		return (priority << 24) | (groupIndex & 0xFFFFFF);
	}	
#endif
};

struct GeometryStreamingState
{
#ifdef __cplusplus
	uint32_t NumRequests;
#else
	uint NumRequests;
#endif
};


