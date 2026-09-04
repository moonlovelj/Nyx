#include "VirtualShadowMap.h.slang"
#include "../VirtualShadowMapCullShared.h"

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
StructuredBuffer<DirectionalVsmAddressGpu> g_DirectionalVsmAddresses;
ByteAddressBuffer g_VsmPageRequestMask;
StructuredBuffer<uint> g_VsmPreviousPageTable;
StructuredBuffer<uint> g_VsmFreePhysicalPagesSRV;
ByteAddressBuffer g_VsmPhysicalPageUsedMaskSRV;
StructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequestsSRV;
ByteAddressBuffer g_VsmPageManagementCountersSRV;
ByteAddressBuffer g_VsmCullCounters;

RWStructuredBuffer<uint> g_VsmCurrentPageTable;
RWStructuredBuffer<VsmPhysicalPageMetadata> g_VsmPhysicalPageMetadataUAV;
RWStructuredBuffer<VsmPhysicalPageView> g_VsmPhysicalPageViewsUAV;
RWStructuredBuffer<uint> g_VsmFreePhysicalPagesUAV;
RWStructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
RWByteAddressBuffer g_VsmPhysicalPageUsedMaskUAV;
RWByteAddressBuffer g_VsmPageManagementCounters;
RWByteAddressBuffer g_VsmRenderRequestPredicate;

struct VsmPageManagementConstants
{
    uint FrameNumber;
    uint AllocationClass;
};

struct MarkPhysicalPageRenderedConstants
{
    uint FirstRenderRequestIndex;
    uint RenderRequestCount;
    uint AddressType;
    uint RequireCullSuccess;
};

struct MarkVsmViewDirtyConstants
{
    uint ViewId;
};

ConstantBuffer<VsmPageManagementConstants> g_VsmPageManagement;
ConstantBuffer<MarkPhysicalPageRenderedConstants> g_MarkPhysicalPageRendered;
ConstantBuffer<MarkVsmViewDirtyConstants> g_MarkVsmViewDirty;

uint GetRequestedPageTableIndex(uint wordIndex, uint bitIndex)
{
    return wordIndex * VSM_REQUEST_MASK_WORD_BITS + bitIndex;
}

bool IsCoarseFallbackView(VsmShadowView shadowView)
{
    return (shadowView.Flags & VSM_SHADOW_VIEW_FLAG_COARSE_FALLBACK) != 0u;
}

bool ShouldProcessView(VsmShadowView shadowView)
{
    bool processCoarse = g_VsmPageManagement.AllocationClass == VSM_PAGE_ALLOCATION_CLASS_COARSE;
    return IsCoarseFallbackView(shadowView) == processCoarse;
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
    metadata.Flags = VSM_PHYSICAL_PAGE_METADATA_VALID;
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

    g_VsmRenderRequestPredicate.Store2(
        renderRequestIndex * VSM_RENDER_REQUEST_PREDICATE_STRIDE,
        uint2(1u, 0u));
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
        uint previousPageTableEntry = g_VsmPreviousPageTable[shadowView.PreviousPageTableBase + pageTableIndex];
        if (!IsVsmPageTableEntryExact(previousPageTableEntry))
            continue;

        uint physicalPageIndex = GetVsmPageTablePhysicalPageIndex(previousPageTableEntry);
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
        g_VsmCurrentPageTable[shadowView.PageTableBase + pageTableIndex] =
            EncodeVsmPageTableEntry(physicalPageIndex, 0u);

        uint originalValue;
        if (needsRender)
        {
            uint renderRequestFlags = isRendered ? VSM_PAGE_RENDER_REQUEST_HISTORY_VALID : 0u;
            AppendRenderRequest(viewId, pageTableIndex, physicalPageIndex, renderRequestFlags);

            if (isRendered)
            {
                g_VsmPageManagementCounters.InterlockedAdd(
                    VSM_HISTORY_VALID_REQUEST_COUNT_OFFSET,
                    1u,
                    originalValue);
            }
        }
        else
        {
            g_VsmPageManagementCounters.InterlockedAdd(VSM_CACHE_HIT_PAGE_COUNT_OFFSET, 1u, originalValue);
        }

        g_VsmPageManagementCounters.InterlockedAdd(VSM_REUSED_PAGE_COUNT_OFFSET, 1u, originalValue);
        if (IsCoarseFallbackView(shadowView))
        {
            g_VsmPageManagementCounters.InterlockedAdd(
                VSM_COARSE_MAPPED_PAGE_COUNT_OFFSET,
                1u,
                originalValue);
        }
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
    if (!ShouldProcessView(shadowView))
        return;

    uint requestWord = g_VsmPageRequestMask.Load(GetVsmRequestMaskWordByteAddress(shadowView, wordIndex));
    while (requestWord != 0u)
    {
        uint bitIndex = firstbitlow(requestWord);
        requestWord &= requestWord - 1u;

        uint pageTableIndex = GetRequestedPageTableIndex(wordIndex, bitIndex);
        uint pageTableEntryIndex = shadowView.PageTableBase + pageTableIndex;
        if (IsVsmPageTableEntryValid(g_VsmCurrentPageTable[pageTableEntryIndex]))
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
            if (IsCoarseFallbackView(shadowView))
            {
                g_VsmPageManagementCounters.InterlockedAdd(
                    VSM_COARSE_OVERFLOW_PAGE_COUNT_OFFSET,
                    1u,
                    originalValue);
            }
            continue;
        }

        uint physicalPageIndex = g_VsmFreePhysicalPagesSRV[freePageIndex];
        g_VsmPhysicalPageMetadataUAV[physicalPageIndex] = MakePhysicalPageMetadata(shadowView, virtualPage);
        g_VsmCurrentPageTable[pageTableEntryIndex] = EncodeVsmPageTableEntry(physicalPageIndex, 0u);
        AppendRenderRequest(viewId, pageTableIndex, physicalPageIndex, 0u);

        uint originalValue;
        g_VsmPageManagementCounters.InterlockedAdd(VSM_NEW_PAGE_COUNT_OFFSET, 1u, originalValue);
        if (IsCoarseFallbackView(shadowView))
        {
            g_VsmPageManagementCounters.InterlockedAdd(
                VSM_COARSE_MAPPED_PAGE_COUNT_OFFSET,
                1u,
                originalValue);
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void markViewDirty(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint physicalPageIndex = dispatchThreadId.x;
    if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return;

    VsmShadowView shadowView = g_VsmShadowViews[g_MarkVsmViewDirty.ViewId];
    VsmPhysicalPageMetadata metadata = g_VsmPhysicalPageMetadataUAV[physicalPageIndex];
    bool isRendered = (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_RENDERED) != 0u;
    bool isDirty = (metadata.Flags & VSM_PHYSICAL_PAGE_METADATA_DIRTY) != 0u;
    if (!IsVsmPhysicalPageOwnedByView(metadata, shadowView) || !isRendered || isDirty)
        return;

    metadata.Flags |= VSM_PHYSICAL_PAGE_METADATA_DIRTY;
    g_VsmPhysicalPageMetadataUAV[physicalPageIndex] = metadata;

    uint originalValue;
    g_VsmPageManagementCounters.InterlockedAdd(VSM_MARKED_DIRTY_PAGE_COUNT_OFFSET, 1u, originalValue);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void markPhysicalPageRendered(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_MarkPhysicalPageRendered.RenderRequestCount)
        return;

    if (g_MarkPhysicalPageRendered.RequireCullSuccess != 0u &&
        g_VsmCullCounters.Load(VSM_CULL_OVERFLOW_COUNT_OFFSET) != 0u)
    {
        return;
    }

    uint renderRequestIndex = g_MarkPhysicalPageRendered.FirstRenderRequestIndex + dispatchThreadId.x;
    uint renderRequestCount = g_VsmPageManagementCountersSRV.Load(VSM_RENDER_REQUEST_COUNT_OFFSET);
    if (renderRequestIndex >= renderRequestCount)
        return;

    VsmPageRenderRequest request = g_VsmPageRenderRequestsSRV[renderRequestIndex];
    if (request.PhysicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        return;

    VsmShadowView shadowView = g_VsmShadowViews[request.ViewId];
    if (g_MarkPhysicalPageRendered.AddressType != VSM_ADDRESS_TYPE_INVALID &&
        shadowView.AddressType != g_MarkPhysicalPageRendered.AddressType)
    {
        return;
    }

    VsmPhysicalPageView pageView = g_VsmPhysicalPageViewsUAV[request.PhysicalPageIndex];
    if (pageView.PhysicalPageIndex != request.PhysicalPageIndex || pageView.ViewId != request.ViewId)
        return;

    VsmPhysicalPageMetadata metadata = g_VsmPhysicalPageMetadataUAV[request.PhysicalPageIndex];
    metadata.Flags |= VSM_PHYSICAL_PAGE_METADATA_RENDERED;
    metadata.Flags &= ~VSM_PHYSICAL_PAGE_METADATA_DIRTY;
    g_VsmPhysicalPageMetadataUAV[request.PhysicalPageIndex] = metadata;

    pageView.PrevViewProjRow0 = pageView.ViewProjRow0;
    pageView.PrevViewProjRow1 = pageView.ViewProjRow1;
    pageView.PrevViewProjRow2 = pageView.ViewProjRow2;
    pageView.PrevViewProjRow3 = pageView.ViewProjRow3;
    g_VsmPhysicalPageViewsUAV[request.PhysicalPageIndex] = pageView;
}
