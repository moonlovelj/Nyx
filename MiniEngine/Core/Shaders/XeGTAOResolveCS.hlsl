#include "XeGTAORS.hlsli"
#include "XeGTAOCommon.hlsli"

cbuffer GTAOConstantBuffer : register(b0)
{
    XeGTAOConstants g_GTAOConsts;
}

Texture2D<uint>           g_SourceAOTerm : register(t0);
RWTexture2D<unorm float>  g_OutputAO     : register(u0);

[RootSignature(XE_GTAO_RootSig)]
[numthreads(8, 8, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= g_GTAOConsts.ViewportSize.x || dispatchThreadID.y >= g_GTAOConsts.ViewportSize.y)
    {
        return;
    }

    // Convert packed uint AO (0..255) into Nyx AO target (R8_UNORM).
    g_OutputAO[dispatchThreadID] = saturate((float)g_SourceAOTerm[dispatchThreadID] / 255.0);
}

