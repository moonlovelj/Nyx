#include "Common.hlsli"
#include "CommonResources.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    //float2 uv0 : TEXCOORD0;
    nointerpolation uint commandIndex : TEXCOORD1;
};

[RootSignature(Renderer_RootSig)]
void main(VSOutput vsOutput, uint primID : SV_PrimitiveID)
{
    uint64_t packed = (((uint64_t) asuint(vsOutput.position.z)) << 32) | 
    ((vsOutput.commandIndex & 0x01FFFFFF) << 7) | (primID & 0x7F);
    
    RWTexture2D<uint64_t> vbufferUAV = GetVBufferUAV();
    
    InterlockedMax(vbufferUAV[uint2(vsOutput.position.xy)], packed);
}

