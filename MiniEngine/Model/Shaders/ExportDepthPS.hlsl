
#include "Common.hlsli"
#include "CommonResources.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct PSOutput
{
    float depth : SV_Depth;
};

[RootSignature(Renderer_RootSig)]
PSOutput main(VSOutput input)
{
    Texture2D<uint2> vbuffer = GetVBufferSRV();
    uint2 vData = vbuffer.Load(int3(input.position.xy, 0));
    PSOutput output;
    output.depth = asfloat(vData.g);
    return output;
}