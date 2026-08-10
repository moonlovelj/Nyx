#include "VirtualShadowMap.h.slang"

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
ByteAddressBuffer g_VsmPageRequestMask;
StructuredBuffer<uint> g_VsmPreviousPageTable;
StructuredBuffer<uint> g_VsmFreePhysicalPagesSRV;
ByteAddressBuffer g_VsmPhysicalPageUsedMaskSRV;
StructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequestsSRV;
ByteAddressBuffer g_VsmPageManagementCountersSRV;

RWStructuredBuffer<uint> g_VsmCurrentPageTable;
RWStructuredBuffer<VsmPhysicalPageMetadata> g_VsmPhysicalPageMetadataUAV;
RWStructuredBuffer<uint> g_VsmFreePhysicalPagesUAV;
RWStructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
RWByteAddressBuffer g_VsmPhysicalPageUsedMaskUAV;
RWByteAddressBuffer g_VsmPageManagementCounters;

struct VsmPageManagementConstants
{
    uint FrameNumber;
};

struct MarkPhysicalPageRenderedConstants
{
    uint RenderRequestIndex;
};

ConstantBuffer<VsmPageManagementConstants> g_VsmPageManagement;
ConstantBuffer<MarkPhysicalPageRenderedConstants> g_MarkPhysicalPageRendered;

uint GetRequestedPageTableIndex(uint wordIndex, uint bitIndex)
{
    return wordIndex * VSM_REQUEST_MASK_WORD_BITS + bitIndex;
}

bool TryGetRequestedVirtualPage(VsmShadowView shadowView, uint pageTableIndex, out int2 virtualPage)
{
    if (shadowView.AddressType == VSM_ADDRESS_TYPE_DIRECTIONAL_CLIPMAP)
    {
        DirectionalVsmAddressGpu addressData = g_DirectionalVsmAddresses[shadowView.AddressDataIndex];
        virtualPage = GetDirectionalVsmGlobalPage(pageTableIndex, addressData);
        return true;
    }

    if (shadowView.AddressType == VSM_ADDRESS_TYPE_LOCAL_PERSPECTIVE)
    {
        virtualPage = int2(GetVsmPageTableCoord(pageTableIndex));
        return true;
    }

    virtualPage = int2(0);
    return false;
}

VsmPhysicalPageMetadata MakePhysicalPageMetadata(VsmShadowView shadowView, int2 virtualPage)
{
    VsmPhysicalPageMetadata metadata;
    metadata.StableShadowMapId = shadowView.StableShadowMapId;
    metadata.AddressGeneration = shadowView.AddressGeneration;
    metadata.AddressType = shadowView.AddressType;
    metadata.Layer = shadowView.Layer;
    metadata.VirtualPage = virtualPage;
    metadata.Flags = VSM_PHYSICAL_PAGE_METADATA_VALID | VSM_PHYSICAL_PAGE_METADATA_DIRTY;
    metadata.LastRequestedFrame = g_VsmPageManagement.FrameNumber;
    return metadata;
}

bool TryClaimPhysicalPage(uint physicalPageIndex)
{
    uint wordIndex = physicalPageIndex >> VSM_REQUEST_MASK_WORD_LOG2;
    uint bitMask = 1u << (physicalPageIndex & VSM_REQUEST_MASK_WORD_BIT_MASK);

    uint previousMask;
    g_VsmPhysicalPageUsedMaskUAV.InterlockedOr(wordIndex * sizeof(uint), bitMask, previousMask);
    return (previousMask & bitMask) == 0u;
}

void AppendRenderRequest(uint viewId, uint pageTableIndex, uint physicalPageIndex, uint flags)
{
    uint renderRequestIndex;
    g_VsmPageManagementCounters.InterlockedAdd(VSM_RENDER_REQUEST_COUNT_OFFSET, 1u, renderRequestIndex);
    if (renderRequestIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return;

    VsmPageRenderRequest renderRequest;
    renderRequest.ViewId = viewId;
    renderRequest.PageTableIndex = pageTableIndex;
    renderRequest.PhysicalPageIndex = physicalPageIndex;
    renderRequest.Flags = flags;
    g_VsmPageRenderRequests[renderRequestIndex] = renderRequest;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void reuseRequestedPages(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint wordIndex = dispatchThreadId.x;
    uint viewId = dispatchThreadId.y;
    if (wordIndex >= VSM_REQUEST_MASK_WORD_COUNT_PER_VIEW)
        return;

    VsmShadowView shadowView = g_VsmShadowViews[viewId];
    if (shadowView.PreviousPageTableBase == VSM_INVALID_PAGE_TABLE_BASE)
        return;

    uint requestWord = g_VsmPageRequestMask.Load(GetVsmRequestMaskWordByteAddress(shadowView, wordIndex));
    while (requestWord != 0u)
    {
        uint bitIndex = firstbitlow(requestWord);
        requestWord &= requestWord - 1u;

        uint pageTableIndex = GetRequestedPageTableIndex(wordIndex, bitIndex);
        uint physicalPageIndex = g_VsmPreviousPageTable[shadowView.PreviousPageTableBase + pageTableIndex];
        if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
            continue;

        int2 virtualPage;
        if (!TryGetRequestedVirtualPage(shadowView, pageTableIndex, virtualPage))
            continue;

        VsmPhysicalPageMetadata metadata = g_VsmPhysicalPageMetadataUAV[physicalPageIndex];
        if (!IsVsmPhysicalPageMetadataMatch(metadata, shadowView, virtualPage) ||
            !TryClaimPhysicalPage(physicalPageIndex))
            continue;

        bool isRendered = (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_RENDERED) != 0u;
        bool needsRender = !isRendered || (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_DIRTY) != 0u;

        metadata.LastRequestedFrame = g_VsmPageManagement.FrameNumber;
        g_VsmPhysicalPageMetadataUAV[physicalPageIndex] = metadata;
        g_VsmCurrentPageTable[shadowView.PageTableBase + pageTableIndex] = physicalPageIndex;

        uint originalValue;
        if (needsRender)
        {
            uint renderRequestFlags = isRendered ? VSM_PAGE_RENDER_REQUEST_HISTORY_VALID : 0u;
            AppendRenderRequest(viewId, pageTableIndex, physicalPageIndex, renderRequestFlags);
        }
        else
        {
            g_VsmPageManagementCounters.InterlockedAdd(VSM_CACHE_HIT_PAGE_COUNT_OFFSET, 1u, originalValue);
        }

        g_VsmPageManagementCounters.InterlockedAdd(VSM_REUSED_PAGE_COUNT_OFFSET, 1u, originalValue);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void buildFreePhysicalPageList(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint physicalPageIndex = dispatchThreadId.x;
    if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return;

    uint wordIndex = physicalPageIndex >> VSM_REQUEST_MASK_WORD_LOG2;
    uint bitMask = 1u << (physicalPageIndex & VSM_REQUEST_MASK_WORD_BIT_MASK);
    if ((g_VsmPhysicalPageUsedMaskSRV.Load(wordIndex * sizeof(uint)) & bitMask) != 0u)
        return;

    uint freePageIndex;
    g_VsmPageManagementCounters.InterlockedAdd(VSM_FREE_PAGE_COUNT_OFFSET, 1u, freePageIndex);
    g_VsmFreePhysicalPagesUAV[freePageIndex] = physicalPageIndex;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void allocateNewPages(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint wordIndex = dispatchThreadId.x;
    uint viewId = dispatchThreadId.y;
    if (wordIndex >= VSM_REQUEST_MASK_WORD_COUNT_PER_VIEW)
        return;

    VsmShadowView shadowView = g_VsmShadowViews[viewId];
    uint requestWord = g_VsmPageRequestMask.Load(GetVsmRequestMaskWordByteAddress(shadowView, wordIndex));
    while (requestWord != 0u)
    {
        uint bitIndex = firstbitlow(requestWord);
        requestWord &= requestWord - 1u;

        uint pageTableIndex = GetRequestedPageTableIndex(wordIndex, bitIndex);
        uint pageTableEntryIndex = shadowView.PageTableBase + pageTableIndex;
        if (g_VsmCurrentPageTable[pageTableEntryIndex] != VSM_INVALID_PHYSICAL_PAGE)
            continue;

        int2 virtualPage;
        if (!TryGetRequestedVirtualPage(shadowView, pageTableIndex, virtualPage))
            continue;

        uint freePageIndex;
        g_VsmPageManagementCounters.InterlockedAdd(VSM_FREE_PAGE_READ_OFFSET, 1u, freePageIndex);
        if (freePageIndex >= g_VsmPageManagementCounters.Load(VSM_FREE_PAGE_COUNT_OFFSET))
        {
            uint originalValue;
            g_VsmPageManagementCounters.InterlockedAdd(VSM_OVERFLOW_PAGE_COUNT_OFFSET, 1u, originalValue);
            continue;
        }

        uint physicalPageIndex = g_VsmFreePhysicalPagesSRV[freePageIndex];
        g_VsmPhysicalPageMetadataUAV[physicalPageIndex] = MakePhysicalPageMetadata(shadowView, virtualPage);
        g_VsmCurrentPageTable[pageTableEntryIndex] = physicalPageIndex;
        AppendRenderRequest(viewId, pageTableIndex, physicalPageIndex, 0u);

        uint originalValue;
        g_VsmPageManagementCounters.InterlockedAdd(VSM_NEW_PAGE_COUNT_OFFSET, 1u, originalValue);
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void markPhysicalPageRendered()
{
    uint renderRequestCount = g_VsmPageManagementCountersSRV.Load(VSM_RENDER_REQUEST_COUNT_OFFSET);
    if (g_MarkPhysicalPageRendered.RenderRequestIndex >= renderRequestCount)
        return;

    VsmPageRenderRequest request = g_VsmPageRenderRequestsSRV[g_MarkPhysicalPageRendered.RenderRequestIndex];
    if (request.PhysicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return;

    VsmPhysicalPageMetadata metadata = g_VsmPhysicalPageMetadataUAV[request.PhysicalPageIndex];
    metadata.Flags |= VSM_PHYSICAL_PAGE_METADATA_RENDERED;
    metadata.Flags &= ~VSM_PHYSICAL_PAGE_METADATA_DIRTY;
    g_VsmPhysicalPageMetadataUAV[request.PhysicalPageIndex] = metadata;
}
