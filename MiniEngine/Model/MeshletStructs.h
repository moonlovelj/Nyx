#ifndef MESHLET_STRUCTS_H
#define MESHLET_STRUCTS_H

#ifdef __cplusplus
namespace Renderer
{
#endif // __cplusplus

#define INVALID_GROUP_INDEX 0xFFFFFFFF
#define INVALID_NODE_INDEX 0xFFFFFFFF
#define INVALID_CHUNK_INDEX 0xFFFFFFFF
	// -------------------------------------------------------
	// BVH 节点 (用于 GPU 剔除和遍历)，常驻显存
	// -------------------------------------------------------
	struct HierarchyNode
	{
#ifdef __cplusplus
		float BoundSphere[4];
		float BBoxMin[3];
		float BBoxMax[3];
		float MaxParrentError;
		union {
			struct {
				uint32_t IsGroup : 1;			// is false for internal node
				uint32_t ChildStartIndex : 27;	// 相对于节点数组起始位置的偏移
				uint32_t ChildCount : 4;		// 最大 8 个子节点
			} Internal;

			struct {
				uint32_t IsGroup : 1;			// is false for Leaf node
				uint32_t GroupIndex : 24;		// 指向 ModelData::m_GroupInfos
				uint32_t MeshletCountMinusOne : 7;		// 128 个 meshlet 上限
			} Leaf;
		};
#else
		float4 BoundSphere;
		float3 BBoxMin;
		float3 BBoxMax;
		float MaxParrentError;
		uint NodeData;

		void Init()
		{
			BoundSphere = float4(0.0f, 0.0f, 0.0f, 0.0f);
			BBoxMin = float3(0.0f, 0.0f, 0.0f);
			BBoxMax = float3(0.0f, 0.0f, 0.0f);
			MaxParrentError = 0.0f;
			NodeData = 0;
		}

		bool IsGroup()
		{
			return (NodeData & 0x1) != 0;
		}

		uint GetChildNodeStartIndex()
		{
			return (NodeData >> 1) & 0x7FFFFFF; // 27 bits
		}

		uint GetChildNodeCount()
		{
			return (NodeData >> 28) & 0xF; // 4 bits
		}

		uint GetGroupIndex()
		{
			return (NodeData >> 1) & 0xFFFFFF; // 24 bits
		}

		uint GetMeshletCount()
		{
			return ((NodeData >> 25) & 0x7F) + 1; // 7 bits
		}
#endif
	};

	// -------------------------------------------------------
	// MeshletGroup 头信息 (流式加载)
	// -------------------------------------------------------
#ifdef __cplusplus
	struct GroupHeader
	{
		float    BoundSphere[4];				// Center(3) + Radius(1)
		float    BBoxMin[3];					// Tight AABB Min (xyz)
		float    BBoxMax[3];					// Tight AABB Max (xyz)
		float    ParrentError;					// 父误差（更粗糙一层的误差）
		uint32_t MeshletCount;
		//uint32_t ResidentID;					// 运行时数据 (磁盘上为 0，加载到 GPU 后由流式系统填充)
	};

	static_assert(alignof(GroupHeader) == 4, "GroupHeader should be 4-byte aligned");
#else
	struct GroupHeader
	{
		float4    BoundSphere;				// Center(3) + Radius(1)
		float3    BBoxMin;					// Tight AABB Min (xyz)
		float3    BBoxMax;					// Tight AABB Max (xyz)
		float     ParrentError;					// 父误差（更粗糙一层的误差）
		uint	  MeshletCount;
	};
#endif

#ifdef __cplusplus
	// -------------------------------------------------------
	// Meshlet 头信息 (存储在 Group Blob 中， 流式加载)
	// -------------------------------------------------------
	struct MeshletHeader
	{
		uint8_t  VertexCountMinusOne;			// 顶点数量-1 (0-255)
		uint8_t  TriangleCountMinusOne;			// 三角形数量-1 (0-255)
		uint8_t  LODLevel;						// 所属 LOD 层级
		uint8_t  GroupChildIndex;				// 所属组内子节点索引

		uint16_t MeshBufferIndex;				// 指向全局 Mesh CBV 表
		uint16_t MaterialBufferIndex;			// 指向全局 Material CBV 表
		uint16_t PSOFlags;           
		uint16_t VertexStride;					// 单个顶点数据大小 (字节)

		uint32_t RefineGroupIndex;				// 精细化组Index (0xFFFFFFFF 表示无精细化组)

		float   BBoxMin[3];						// Tight AABB Min (xyz)
		float   BBoxMax[3];						// Tight AABB Max (xyz)

		// 相对偏移量：相对于 Group 数据块起始位置的字节偏移
		uint32_t VertexOffset;					// 顶点索引数组 (uint32_t 或压缩格式)
		uint32_t TriangleOffset;				// 三角形索引数组 (uint8_t)
	};

	static_assert(alignof(MeshletHeader) == 4, "MeshletHeader should be 4-byte aligned");
#else
	struct MeshletHeader
	{
		uint PackedCounts;						// VertexCountMinusOne(8) + TriangleCountMinusOne(8) + LODLevel(8) + GroupChildIndex(8)
		uint PackedIndices;						// MeshBufferIndex(16) + MaterialBufferIndex(16)
		uint PackedFlags;						// PSOFlags(16) + VertexStride(16)
		uint RefineGroupIndex;					// 精细化组Index (0xFFFFFFFF 表示无精细化组)
		float3   BBoxMin;
		float3   BBoxMax;
		uint VertexOffset;						// 顶点索引数组偏移
		uint TriangleOffset;				    // 三角形索引数组 (uint8_t)

		uint GetVertexCount()
		{
			return (PackedCounts & 0xFF) + 1;
		}

		uint GetTriangleCount()
		{
			return ((PackedCounts >> 8) & 0xFF) + 1;
		}

		uint GetLODLevel()
		{
			return (PackedCounts >> 16) & 0xFF;
		}

		uint GetGroupChildIndex()
		{
			return (PackedCounts >> 24) & 0xFF;
		}

		uint GetMeshBufferIndex()
		{
			return PackedIndices & 0xFFFF;
		}
		uint GetMaterialBufferIndex()
		{
			return (PackedIndices >> 16) & 0xFFFF;
		}

		uint GetPSOFlags()
		{
			return PackedFlags & 0xFFFF;
		}

		uint GetVertexStride()
		{
			return (PackedFlags >> 16) & 0xFFFF;
		}

		uint GetRefineGroupIndex()
		{
			return RefineGroupIndex;
		}
	};
#endif

#ifdef __cplusplus
	// -------------------------------------------------------
	// GPU 查找表：GroupIndex -> 显存物理地址 (CPU 更新，GPU 读取)
	// -------------------------------------------------------
	struct GroupDataLocation
	{
		uint32_t ChunkIndex;    // 对应 ResourceDescriptorHeap 中的索引 (SRV Index)
		uint32_t ByteOffset;    // 在该 Chunk 中的字节偏移量
	};
#else
	struct GroupDataLocation
	{
		uint ChunkIndex;        // 对应 ResourceDescriptorHeap 中的索引
		uint ByteOffset;        // 在该 Chunk 中的字节偏移量
	};
#endif

#ifdef __cplusplus
	// -------------------------------------------------------
	// 磁盘/内存元数据 (CPU 端管理，不传给 Shader)
	// -------------------------------------------------------
	struct GroupMetadata
	{
		uint32_t SizeBytes;						// 压缩后的 Blob 大小
		uint32_t UncompressedSize;				// 解压后 GPU 显存占用

		// --- 流送逻辑信息 (用于加载到显存) ---
		uint32_t PageIndex;						// 该 Group 属于哪个逻辑 Page (IO单位)
		uint32_t OffsetInPage;					// 在该 Page 内的相对偏移量
	};

	struct PageMetadata
	{
		uint32_t StartGroupIndex;
		uint32_t GroupCount;
	};

	struct PageCompressionInfo
	{
		uint64_t CompressedOffset; // Offset relative to geometry blob start
		uint32_t CompressedSize;   // Bytes on disk for this page
		uint32_t Reserved = 0;
	};
#endif

#ifdef __cplusplus
}
#endif

#endif
