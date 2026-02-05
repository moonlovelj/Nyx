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
	// BVH nodes (for GPU culling and traversal), resident in VRAM
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
				uint32_t ChildStartIndex : 27;	// Offset relative to node array start
				uint32_t ChildCount : 4;		// Up to 8 child nodes
			} Internal;

			struct {
				uint32_t IsGroup : 1;			// is false for Leaf node
				uint32_t GroupIndex : 24;		// Index into ModelData::m_GroupInfos
				uint32_t MeshletCountMinusOne : 7;		// Up to 128 meshlets
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
	// MeshletGroup header (streaming)
	// -------------------------------------------------------
#ifdef __cplusplus
	struct GroupHeader
	{
		float    BoundSphere[4];				// Center(3) + Radius(1)
		float    BBoxMin[3];					// Tight AABB Min (xyz)
		float    BBoxMax[3];					// Tight AABB Max (xyz)
		float    ParrentError;					// Parent error (error at a coarser level)
		uint32_t MeshletCount;
		//uint32_t ResidentID;					// Runtime data (0 on disk, filled by streaming system after GPU load)
	};

	static_assert(alignof(GroupHeader) == 4, "GroupHeader should be 4-byte aligned");
#else
	struct GroupHeader
	{
		float4    BoundSphere;				// Center(3) + Radius(1)
		float3    BBoxMin;					// Tight AABB Min (xyz)
		float3    BBoxMax;					// Tight AABB Max (xyz)
		float     ParrentError;					// Parent error (error at a coarser level)
		uint	  MeshletCount;
	};
#endif

#ifdef __cplusplus
	// -------------------------------------------------------
	// Meshlet header (stored in group blob, streaming)
	// -------------------------------------------------------
	struct MeshletHeader
	{
		uint8_t  VertexCountMinusOne;			// Vertex count - 1 (0-255)
		uint8_t  TriangleCountMinusOne;			// Triangle count - 1 (0-255)
		uint8_t  LODLevel;						// LOD level
		uint8_t  GroupChildIndex;				// Group child index

		uint16_t MeshBufferIndex;				// Index into global Mesh CBV table
		uint16_t MaterialBufferIndex;			// Index into global Material CBV table
		uint16_t PSOFlags;           
		uint16_t VertexStride;					// Vertex size in bytes

		uint32_t RefineGroupIndex;				// Refinement group index (0xFFFFFFFF means none)

		float   BBoxMin[3];						// Tight AABB Min (xyz)
		float   BBoxMax[3];						// Tight AABB Max (xyz)

		// Relative offset: byte offset from start of Group data block
		uint32_t VertexOffset;					// Vertex index array (uint32_t or compressed)
		uint32_t TriangleOffset;				// Triangle index array (uint8_t)
	};

	static_assert(alignof(MeshletHeader) == 4, "MeshletHeader should be 4-byte aligned");
#else
	struct MeshletHeader
	{
		uint PackedCounts;						// VertexCountMinusOne(8) + TriangleCountMinusOne(8) + LODLevel(8) + GroupChildIndex(8)
		uint PackedIndices;						// MeshBufferIndex(16) + MaterialBufferIndex(16)
		uint PackedFlags;						// PSOFlags(16) + VertexStride(16)
		uint RefineGroupIndex;					// Refinement group index (0xFFFFFFFF means none)
		float3   BBoxMin;
		float3   BBoxMax;
		uint VertexOffset;						// Vertex index array offset
		uint TriangleOffset;				    // Triangle index array (uint8_t)

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
	// GPU lookup table: GroupIndex -> GPU address (CPU updates, GPU reads)
	// -------------------------------------------------------
	struct GroupDataLocation
	{
		uint32_t ChunkIndex;    // Index into ResourceDescriptorHeap (SRV index)
		uint32_t ByteOffset;    // Byte offset within the chunk
	};
#else
	struct GroupDataLocation
	{
		uint ChunkIndex;        // Index into ResourceDescriptorHeap
		uint ByteOffset;        // Byte offset within the chunk
	};
#endif

#ifdef __cplusplus
	// -------------------------------------------------------
	// Disk/memory metadata (managed on CPU, not sent to shader)
	// -------------------------------------------------------
	struct GroupMetadata
	{
		uint32_t SizeBytes;						// Compressed blob size
		uint32_t UncompressedSize;				// Uncompressed GPU memory footprint

		// --- Streaming logic info (for loading to GPU memory) ---
		uint32_t PageIndex;						// Which logical page this group belongs to (IO unit)
		uint32_t OffsetInPage;					// Relative offset within the page
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
