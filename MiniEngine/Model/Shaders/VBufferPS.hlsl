#include "Common.hlsli"
#include "CommonResources.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    //float2 uv0 : TEXCOORD0;
    nointerpolation uint commandIndex : TEXCOORD1;
};

struct MRT
{
    uint2 VBuffer : SV_Target0;
};

[RootSignature(Renderer_RootSig)]
MRT main(VSOutput vsOutput, uint primID : SV_PrimitiveID)
{
    // TODO: alpha test support
    MRT mrt;
    mrt.VBuffer.g = asuint(vsOutput.position.z);
    mrt.VBuffer.r = ((vsOutput.commandIndex & 0x01FFFFFF) << 7) | (primID & 0x7F);
    return mrt;
}

