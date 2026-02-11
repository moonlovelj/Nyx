#include "XeGTAORS.hlsli"
#include "XeGTAOCommon.hlsli"

cbuffer GTAOConstantBuffer : register(b0)
{
    XeGTAOConstants g_GTAOConsts;
}

cbuffer MainPassConstants : register(b1)
{
    float g_SliceCount;
    float g_StepsPerSlice;
}

SamplerState g_PointClampSampler : register(s0);

Texture2D<float>          g_WorkingDepth  : register(t0);
RWTexture2D<uint>         g_WorkingAOTerm : register(u0);
RWTexture2D<unorm float>  g_WorkingEdges  : register(u1);

#define XE_HILBERT_LEVEL 6U
#define XE_HILBERT_WIDTH (1U << XE_HILBERT_LEVEL)

uint HilbertIndex(uint posX, uint posY)
{
    uint index = 0U;
    for (uint curLevel = XE_HILBERT_WIDTH / 2U; curLevel > 0U; curLevel /= 2U)
    {
        uint regionX = ((posX & curLevel) > 0U) ? 1U : 0U;
        uint regionY = ((posY & curLevel) > 0U) ? 1U : 0U;
        index += curLevel * curLevel * ((3U * regionX) ^ regionY);
        if (regionY == 0U)
        {
            if (regionX == 1U)
            {
                posX = (XE_HILBERT_WIDTH - 1U) - posX;
                posY = (XE_HILBERT_WIDTH - 1U) - posY;
            }

            uint temp = posX;
            posX = posY;
            posY = temp;
        }
    }
    return index;
}

float2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)
{
    uint index = HilbertIndex(pixCoord.x, pixCoord.y);
    index += 288 * (temporalIndex % 64);

    return frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114));
}

[RootSignature(XE_GTAO_RootSig)]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    XeGTAO_MainPass(
        dispatchThreadID,
        (int)g_SliceCount,
        (int)g_StepsPerSlice,
        SpatioTemporalNoise(dispatchThreadID, g_GTAOConsts.NoiseIndex),
        g_GTAOConsts,
        g_WorkingDepth,
        g_PointClampSampler,
        g_WorkingAOTerm,
        g_WorkingEdges);
}

