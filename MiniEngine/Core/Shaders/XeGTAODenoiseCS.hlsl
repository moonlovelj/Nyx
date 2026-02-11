#include "XeGTAORS.hlsli"
#include "XeGTAOCommon.hlsli"

cbuffer GTAOConstantBuffer : register(b0)
{
    XeGTAOConstants g_GTAOConsts;
}

cbuffer DenoisePassConstants : register(b1)
{
    float g_FinalApply;
    float g_Padding;
}

SamplerState g_PointClampSampler : register(s0);

Texture2D<uint>    g_SourceAOTerm : register(t0);
Texture2D<float>   g_SourceEdges  : register(t1);
RWTexture2D<uint>  g_OutputAOTerm : register(u0);

[RootSignature(XE_GTAO_RootSig)]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    // Denoise processes two horizontal pixels per thread for cache efficiency.
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(
        pixCoordBase,
        g_GTAOConsts,
        g_SourceAOTerm,
        g_SourceEdges,
        g_PointClampSampler,
        g_OutputAOTerm,
        g_FinalApply > 0.5);
}

