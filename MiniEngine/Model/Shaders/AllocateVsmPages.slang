#include "VirtualShadowMap.h.slang"

StructuredBuffer<VsmShadowView> g_VsmShadowViews;
ByteAddressBuffer g_VsmPageRequestMask;
RWStructuredBuffer<uint> g_VsmPageTable;
RWStructuredBuffer<VsmPageRenderRequest> g_VsmPageRenderRequests;
RWByteAddressBuffer g_VsmAllocationStatistics;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
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

        uint physicalPageIndex;
        g_VsmAllocationStatistics.InterlockedAdd(
            VSM_ALLOCATION_REQUESTED_PAGE_COUNT_OFFSET,
            1u,
            physicalPageIndex);

        if (physicalPageIndex >= VSM_PHYSICAL_PAGE_CAPACITY)
        {
            uint originalValue;
            g_VsmAllocationStatistics.InterlockedAdd(VSM_OVERFLOW_PAGE_COUNT_OFFSET, 1u, originalValue);
            continue;
        }

        uint pageTableIndex = wordIndex * VSM_REQUEST_MASK_WORD_BITS + bitIndex;
        g_VsmPageTable[GetVsmPageTableEntryIndex(shadowView, pageTableIndex)] = physicalPageIndex;

        VsmPageRenderRequest renderRequest;
        renderRequest.ViewId = viewId;
        renderRequest.PageTableIndex = pageTableIndex;
        renderRequest.PhysicalPageIndex = physicalPageIndex;
        renderRequest.Padding = 0u;
        g_VsmPageRenderRequests[physicalPageIndex] = renderRequest;

        uint originalValue;
        g_VsmAllocationStatistics.InterlockedAdd(VSM_ALLOCATED_PAGE_COUNT_OFFSET, 1u, originalValue);
    }
}
