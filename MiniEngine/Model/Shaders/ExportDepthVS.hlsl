
#include "Common.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

[RootSignature(Renderer_RootSig)]
VSOutput main(uint VertID : SV_VertexID)
{
    VSOutput vsOutput;
    vsOutput.uv = float2((VertID << 1) & 2, VertID & 2);
    vsOutput.position = float4(vsOutput.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vsOutput;
}