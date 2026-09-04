#include "CommonResources.slang"
#include "GeometryCommon.slang"
#include "VirtualShadowMap.h.slang"
#include "WaveUtil.h.slang"
#include "../VirtualShadowMapCullShared.h"

#define VSM_RASTER_PAGE_COUNT (VSM_RASTER_WINDOW_PAGES * VSM_RASTER_WINDOW_PAGES)
#define VSM_RASTER_AS_GROUP_SIZE 32
#define VSM_RASTER_MS_GROUP_SIZE 32
#define VSM_RASTER_MAX_VERTICES 128
#define VSM_RASTER_MAX_PRIMITIVES 128

StructuredBuffer<QueueState> g_VsmCullQueueState;
StructuredBuffer<VsmRasterItem> g_VsmRasterItems;
StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
StructuredBuffer<uint> g_VsmPageTable;
StructuredBuffer<VsmPhysicalPageView> g_VsmPhysicalPageViews;

struct VsmRasterPayload
{
    VisibleMeshletPayload Meshlet;
    uint PhysicalPageIndices[VSM_RASTER_PAGE_COUNT];
};

struct VsmRasterVertex
{
    float4 Position : SV_POSITION;
    float PageClipDistance0 : SV_ClipDistance0;
    float PageClipDistance1 : SV_ClipDistance1;
    float PageClipDistance2 : SV_ClipDistance2;
    float PageClipDistance3 : SV_ClipDistance3;
    float2 BaseColorUv : TEXCOORD0;
    nointerpolation uint PackedMaterialInfo : TEXCOORD1;
};

groupshared uint s_PageTableBase;
groupshared int2 s_WindowOriginPage;
groupshared uint2 s_WindowCoord;
groupshared uint s_PageMask;
groupshared uint s_ValidPageMask;
groupshared uint s_ViewId;
groupshared uint s_InstanceIndex;
groupshared uint s_PackedMeshlet;
groupshared VsmRasterPayload s_RasterPayload;

uint2 UnpackVsmRasterWindowCoord(uint packedWindowAndPageMask)
{
    return uint2(
        (packedWindowAndPageMask >> VSM_RASTER_WINDOW_X_SHIFT) & VSM_RASTER_WINDOW_COORD_MASK,
        (packedWindowAndPageMask >> VSM_RASTER_WINDOW_Y_SHIFT) & VSM_RASTER_WINDOW_COORD_MASK);
}

uint UnpackVsmRasterPageMask(uint packedWindowAndPageMask)
{
    return (packedWindowAndPageMask >> VSM_RASTER_WINDOW_PAGE_MASK_SHIFT) &
        VSM_RASTER_WINDOW_PAGE_MASK_MASK;
}

uint2 GetVsmRasterLocalPage(uint2 windowCoord, uint pageBitIndex)
{
    uint2 pageInWindow = uint2(
        pageBitIndex & (VSM_RASTER_WINDOW_PAGES - 1u),
        pageBitIndex >> VSM_RASTER_WINDOW_PAGES_LOG2);
    return (windowCoord << VSM_RASTER_WINDOW_PAGES_LOG2) + pageInWindow;
}

[shader("amplification")]
[numthreads(VSM_RASTER_AS_GROUP_SIZE, 1, 1)]
void amplificationMain(
    uint3 groupId : SV_GroupID,
    uint groupThreadId : SV_GroupThreadID)
{
    uint rasterItemIndex = groupId.y * VSM_RASTER_DISPATCH_WIDTH + groupId.x;
    uint rasterItemCount = min(
        g_VsmCullQueueState[0].PassState[0].VisibleMeshletCount,
        uint(MAX_VISIBLE_MESHLETS));

    if (groupThreadId == 0u)
    {
        s_PageTableBase = 0u;
        s_WindowOriginPage = int2(0);
        s_WindowCoord = uint2(0u);
        s_PageMask = 0u;
        s_ValidPageMask = 0u;
        s_ViewId = VSM_INVALID_VIEW_ID;
        s_InstanceIndex = 0u;
        s_PackedMeshlet = 0u;

        if (rasterItemIndex < rasterItemCount)
        {
            VsmRasterItem rasterItem = g_VsmRasterItems[rasterItemIndex];
            VsmShadowView shadowView = g_VsmShadowViews[rasterItem.ViewId];
            if (shadowView.AddressType == VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
            {
                DirectionalVsmAddressGpu addressData =
                    g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
                s_PageTableBase = shadowView.PageTableBase;
                s_WindowOriginPage = addressData.AddressAndWindowOriginPage.zw;
                s_WindowCoord = UnpackVsmRasterWindowCoord(rasterItem.PackedWindowAndPageMask);
                s_PageMask = UnpackVsmRasterPageMask(rasterItem.PackedWindowAndPageMask);
                s_ViewId = rasterItem.ViewId;
                s_InstanceIndex = rasterItem.Meshlet.InstanceIndex;
                s_PackedMeshlet = rasterItem.Meshlet.PackedData;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint physicalPageIndex = VSM_INVALID_PHYSICAL_PAGE;
    if (groupThreadId < VSM_RASTER_PAGE_COUNT && (s_PageMask & (1u << groupThreadId)) != 0u)
    {
        uint2 localPage = GetVsmRasterLocalPage(s_WindowCoord, groupThreadId);
        int2 globalPage = s_WindowOriginPage + int2(localPage);
        uint pageTableIndex = s_PageTableBase + GetVsmPageTableIndex(GetVsmPageTableCoord(globalPage));
        uint pageTableEntry = g_VsmPageTable[pageTableIndex];

        if (IsVsmPageTableEntryExact(pageTableEntry))
        {
            physicalPageIndex = GetVsmPageTablePhysicalPageIndex(pageTableEntry);
            if (physicalPageIndex < VSM_PHYSICAL_PAGE_CAPACITY)
            {
                VsmPhysicalPageView pageView = g_VsmPhysicalPageViews[physicalPageIndex];
                if (pageView.PhysicalPageIndex == physicalPageIndex && pageView.ViewId == s_ViewId)
                    InterlockedOr(s_ValidPageMask, 1u << groupThreadId);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (groupThreadId == 0u)
    {
        s_RasterPayload.Meshlet.InstanceIndex = s_InstanceIndex;
        s_RasterPayload.Meshlet.PackedData = s_PackedMeshlet;
    }

    if ((s_ValidPageMask & (1u << groupThreadId)) != 0u)
    {
        uint precedingPageMask = s_ValidPageMask & ((1u << groupThreadId) - 1u);
        s_RasterPayload.PhysicalPageIndices[countbits(precedingPageMask)] = physicalPageIndex;
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(countbits(s_ValidPageMask), 1u, 1u, s_RasterPayload);
}

float2 LoadVsmBaseColorUv(
    ByteAddressBuffer geometryBuffer,
    uint vertexByteOffset,
    uint vertexStride,
    uint vertexId,
    uint psoFlags,
    MaterialConstant material)
{
    if ((psoFlags & PSO_ALPHA_TEST) == 0u)
        return float2(0.0f);

    uint uvOffset = vertexByteOffset + vertexStride * vertexId + 12u + 4u;
    if ((psoFlags & PSO_HAS_TANGENT) != 0u)
        uvOffset += 4u;

    bool useUv1 = (material.flags & MAT_FLAG_BASE_COLOR_UV) != 0u &&
        (psoFlags & PSO_HAS_UV1) != 0u;
    if (useUv1 && (psoFlags & PSO_HAS_UV0) != 0u)
        uvOffset += 4u;

    bool hasUv = useUv1 || (psoFlags & PSO_HAS_UV0) != 0u;
    return hasUv ? DecodeR16G16FLOATToFloat2(geometryBuffer.Load(uvOffset)) : float2(0.0f);
}

groupshared float4 s_PageClipPositions[VSM_RASTER_MAX_VERTICES];
groupshared uint s_VisiblePrimitiveCount;
groupshared uint s_PrimitiveOutputIndices[VSM_RASTER_MAX_PRIMITIVES];
groupshared uint3 s_PrimitiveIndices[VSM_RASTER_MAX_PRIMITIVES];

[shader("mesh")]
[numthreads(VSM_RASTER_MS_GROUP_SIZE, 1, 1)]
[outputtopology("triangle")]
void meshMain(
    uint groupThreadId : SV_GroupThreadID,
    uint meshGroupId : SV_GroupID,
    in payload VsmRasterPayload payload,
    OutputVertices<VsmRasterVertex, VSM_RASTER_MAX_VERTICES> vertices,
    OutputIndices<uint3, VSM_RASTER_MAX_PRIMITIVES> triangles)
{
    uint physicalPageIndex = payload.PhysicalPageIndices[meshGroupId];
    VsmPhysicalPageView pageView = g_VsmPhysicalPageViews[physicalPageIndex];

    VisibleMeshletPayload meshlet = payload.Meshlet;
    InstanceConstant instance = LoadInstanceConstant(meshlet.InstanceIndex);
    MeshConstant mesh = LoadMeshConstant(instance.MeshBufferIdx);

    GroupDataLocation groupDataLocation = GetGroupDataLocationBufferSRV()[meshlet.GetGroupIndex()];
    ByteAddressBuffer geometryBuffer = GetGeometryChunksBufferSRV(groupDataLocation.ChunkIndex);
    MeshletHeader meshletHeader = geometryBuffer.Load<MeshletHeader>(
        groupDataLocation.ByteOffset + sizeof(GroupHeader) + meshlet.GetMeshletIndex() * sizeof(MeshletHeader));
    MaterialConstant material = LoadMaterialConstant(meshletHeader.GetMaterialBufferIndex());

    uint vertexCount = meshletHeader.GetVertexCount();
    uint primitiveCount = meshletHeader.GetTriangleCount();
    uint vertexByteOffset = groupDataLocation.ByteOffset + meshletHeader.VertexOffset;
    uint indexByteOffset = groupDataLocation.ByteOffset + meshletHeader.TriangleOffset;
    uint vertexStride = meshletHeader.GetVertexStride();
    uint psoFlags = meshletHeader.GetPSOFlags();

    if (groupThreadId == 0u)
        s_VisiblePrimitiveCount = 0u;

    for (uint vertexId = groupThreadId; vertexId < vertexCount; vertexId += VSM_RASTER_MS_GROUP_SIZE)
    {
        uint vertexOffset = vertexByteOffset + vertexStride * vertexId;
        float3 localPosition = asfloat(geometryBuffer.Load3(vertexOffset));
        float3 worldPosition = mul(mesh.WorldMatrix, float4(localPosition, 1.0f)).xyz;
        s_PageClipPositions[vertexId] = ProjectWorldToVsmPageClip(pageView, worldPosition);
    }

    GroupMemoryBarrierWithGroupSync();

    float determinantWorld = determinant((float3x3)mesh.WorldMatrix);
    bool isWorldFlipped = determinantWorld < 0.0f;
    bool isTwoSided = (psoFlags & PSO_TWO_SIDED) != 0u;

    for (uint primitiveId = groupThreadId; primitiveId < primitiveCount; primitiveId += VSM_RASTER_MS_GROUP_SIZE)
    {
        uint3 triangle = LoadAndUnpackTriangle(geometryBuffer, indexByteOffset, primitiveId);
        bool isFrontFacing = IsFrontFacing(
            s_PageClipPositions[triangle.x],
            s_PageClipPositions[triangle.y],
            s_PageClipPositions[triangle.z]) != isWorldFlipped;
        bool isVisible = isTwoSided || isFrontFacing;
        uint outputIndex;
        WaveInterlockedAddScalar(s_VisiblePrimitiveCount, isVisible, 1u, outputIndex);

        s_PrimitiveOutputIndices[primitiveId] = isVisible ? outputIndex : INVALID_ID;
        s_PrimitiveIndices[primitiveId] = triangle;
    }

    GroupMemoryBarrierWithGroupSync();
    SetMeshOutputCounts(vertexCount, s_VisiblePrimitiveCount);

    for (uint vertexId = groupThreadId; vertexId < vertexCount; vertexId += VSM_RASTER_MS_GROUP_SIZE)
    {
        float4 pageClipPosition = s_PageClipPositions[vertexId];
        float4 pageClipDistances = GetVsmPageClipDistances(pageClipPosition);

        VsmRasterVertex output;
        output.Position = TransformVsmPageClipToPhysicalAtlas(pageView, pageClipPosition);
        output.PageClipDistance0 = pageClipDistances.x;
        output.PageClipDistance1 = pageClipDistances.y;
        output.PageClipDistance2 = pageClipDistances.z;
        output.PageClipDistance3 = pageClipDistances.w;
        output.BaseColorUv = LoadVsmBaseColorUv(
            geometryBuffer,
            vertexByteOffset,
            vertexStride,
            vertexId,
            psoFlags,
            material);
        output.PackedMaterialInfo = (meshletHeader.GetMaterialBufferIndex() & 0xffffu) |
            ((material.flags & 0xffu) << 16u);

        vertices[vertexId] = output;
    }

    for (uint primitiveId = groupThreadId; primitiveId < primitiveCount; primitiveId += VSM_RASTER_MS_GROUP_SIZE)
    {
        uint outputIndex = s_PrimitiveOutputIndices[primitiveId];
        if (outputIndex != INVALID_ID)
            triangles[outputIndex] = s_PrimitiveIndices[primitiveId];
    }
}

[shader("fragment")]
void pixelMain(VsmRasterVertex input)
{
    uint materialFlags = (input.PackedMaterialInfo >> 16u) & 0xffu;
    if ((materialFlags & MAT_FLAG_ALPHA_TEST) == 0u)
        return;

    uint materialIndex = input.PackedMaterialInfo & 0xffffu;
    MaterialConstant material = LoadMaterialConstant(materialIndex);
    Texture2D<float4> baseColorTexture =
        GetResourceDescriptorHandle<Texture2D<float4>>(material.TextureStartIndex);
    SamplerState baseColorSampler = GetSamplerDescriptor(material.SamplerStartIndex);
    float alpha = material.baseColorFactor.a * baseColorTexture.Sample(baseColorSampler, input.BaseColorUv).a;
    float alphaCutoff = f16tof32(material.flags >> MAT_FLAG_ALPHA_REF_SHIFT);
    if (alpha < alphaCutoff)
        discard;
}
