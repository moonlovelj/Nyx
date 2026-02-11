#include "XeGTAORS.hlsli"
#include "XeGTAOCommon.hlsli"

cbuffer GTAOConstantBuffer : register(b0)
{
    XeGTAOConstants g_GTAOConsts;
}

SamplerState g_PointClampSampler : register(s0);

Texture2D<float>     g_SourceDepth      : register(t0);
RWTexture2D<float> g_WorkingDepthMIP0 : register(u0);
RWTexture2D<float> g_WorkingDepthMIP1 : register(u1);
RWTexture2D<float> g_WorkingDepthMIP2 : register(u2);
RWTexture2D<float> g_WorkingDepthMIP3 : register(u3);
RWTexture2D<float> g_WorkingDepthMIP4 : register(u4);

[RootSignature(XE_GTAO_RootSig)]
[numthreads(8, 8, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID)
{
    // Build the depth pyramid consumed by the main XeGTAO horizon search pass.
    XeGTAO_PrefilterDepths16x16(
        dispatchThreadID,
        groupThreadID,
        g_GTAOConsts,
        g_SourceDepth,
        g_PointClampSampler,
        g_WorkingDepthMIP0,
        g_WorkingDepthMIP1,
        g_WorkingDepthMIP2,
        g_WorkingDepthMIP3,
        g_WorkingDepthMIP4);
}

