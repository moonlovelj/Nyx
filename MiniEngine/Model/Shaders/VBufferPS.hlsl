#include "Common.hlsli"
#include "MaterialCommon.hlsli"
#include "CommonResources.hlsli"


struct MRT
{
    uint2 VBuffer : SV_Target0;
};

[RootSignature(Renderer_RootSig)]
MRT main(VSOutput vsOutput, uint primID : SV_PrimitiveID)
{
    MRT mrt;
    //mrt.Color = float4(MatProps.Emissive, 1.0f);

    return mrt;
}

