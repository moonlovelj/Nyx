#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "../MeshletStructs.h"
#include "../StructsIO.h"

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    nointerpolation uint commandIndex : TEXCOORD1;
    nointerpolation uint packedMaterialInfo : TEXCOORD2;
};

[RootSignature(Renderer_RootSig)]
void main(VSOutput vsOutput, uint primID : SV_PrimitiveID)
{
    uint matFlags = (vsOutput.packedMaterialInfo >> 16) & 0xFF;
    
    // processed in dag cull
    //if (matFlags & MAT_FLAG_ALPHA_BLEND)
    //{
    //    discard;
    //}
    
    if (matFlags & MAT_FLAG_ALPHA_TEST)
    {
        const uint materialIndex = vsOutput.packedMaterialInfo & 0xFFFF;
        MaterialConstant materilConstant = GetMaterialConstantSRV(materialIndex);
        
        float4 baseColorFactor = materilConstant.baseColorFactor;
        uint TextureStartIndex = materilConstant.TextureStartIndex;
        uint SamplerStartIndex = materilConstant.SamplerStartIndex;

        Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[TextureStartIndex];
        SamplerState BaseColorSampler = SamplerDescriptorHeap[SamplerStartIndex];
        float alpha = baseColorFactor.a * BaseColorTexture.Sample(BaseColorSampler, vsOutput.uv0).a;
        float cutoff = f16tof32(materilConstant.flags >> MAT_FLAG_ALPHA_REF_SHIFT);
        if (alpha < cutoff)
            discard;
    }
    
    uint64_t packed = (((uint64_t) asuint(vsOutput.position.z)) << 32) | 
    ((vsOutput.commandIndex & 0x01FFFFFF) << 7) | (primID & 0x7F);
    
    RWTexture2D<uint64_t> vbufferUAV = GetVBufferUAV();
    
    InterlockedMax(vbufferUAV[uint2(vsOutput.position.xy)], packed);
}

