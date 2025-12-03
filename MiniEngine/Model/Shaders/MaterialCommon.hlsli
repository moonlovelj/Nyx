#ifndef __MATERIAL_COMMON_HLSLI__
#define __MATERIAL_COMMON_HLSLI__

#include "VSTOPSCommon.hlsli"
#include "CommonResources.hlsli"

// Flag helpers
static const uint BASECOLOR_UV_OFFSET = 0;
static const uint METALLICROUGHNESS_UV_OFFSET = 1;
static const uint OCCLUSION_UV_OFFSET = 2;
static const uint EMISSIVE_UV_OFFSET = 3;
static const uint NORMAL_UV_OFFSET = 4;
#ifdef NO_SECOND_UV
#define UVSET( offset, flags ) vsOutput.uv0
#else
#define UVSET( offset, flags ) lerp(vsOutput.uv0, vsOutput.uv1, (flags >> offset) & 1)
#endif

float3 ComputeNormal(VSOutput vsOutput, Texture2D<float3> NormalTexture, SamplerState NormalSampler)
{
    MeshletConstant meshletConstant = GetMeshletConstantSRV(MeshletIndex);
    MaterialConstant materilConstant = GetMaterialConstantSRV(meshletConstant.MaterialConstantsIndex);
    float normalTextureScale = materilConstant.normalTextureScale;
    uint flags = materilConstant.flags;

    float3 normal = normalize(vsOutput.normal);

#ifdef NO_TANGENT_FRAME
    return normal;
#else
    // Construct tangent frame
    float3 tangent = normalize(vsOutput.tangent.xyz);
    float3 bitangent = normalize(cross(normal, tangent)) * vsOutput.tangent.w;
    float3x3 tangentFrame = float3x3(tangent, bitangent, normal);

    // Read normal map and convert to SNORM (TODO:  convert all normal maps to R8G8B8A8_SNORM?)
    normal = NormalTexture.Sample(NormalSampler, UVSET(NORMAL_UV_OFFSET, flags)) * 2.0 - 1.0;

    // glTF spec says to normalize N before and after scaling, but that's excessive
    normal = normalize(normal * float3(normalTextureScale, normalTextureScale, 1));

    // Multiply by transpose (reverse order)
    return mul(normal, tangentFrame);
#endif
}

struct MaterialProperties
{
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float Occlusion;
    float3 Emissive;
    float3 Normal;
};

MaterialProperties GetMaterialProperties(VSOutput vsOutput)
{
    MeshletConstant meshletConstant = GetMeshletConstantSRV(MeshletIndex);
    MaterialConstant materilConstant = GetMaterialConstantSRV(meshletConstant.MaterialConstantsIndex);
	float4 baseColorFactor = materilConstant.baseColorFactor;
	float3 emissiveFactor = materilConstant.emissiveFactor;
	float normalTextureScale = materilConstant.normalTextureScale;
	float2 metallicRoughnessFactor = materilConstant.metallicRoughnessFactor;
	uint flags = materilConstant.flags;
	uint TextureStartIndex = materilConstant.TextureStartIndex;
	uint SamplerStartIndex = materilConstant.SamplerStartIndex;

    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[TextureStartIndex];
    Texture2D<float3> MetallicRoughnessTexture = ResourceDescriptorHeap[TextureStartIndex + 1];
    Texture2D<float1> OcclusionTexture = ResourceDescriptorHeap[TextureStartIndex + 2];
    Texture2D<float3> EmissiveTexture = ResourceDescriptorHeap[TextureStartIndex + 3];
    Texture2D<float3> NormalTexture = ResourceDescriptorHeap[TextureStartIndex + 4];

    SamplerState BaseColorSampler = SamplerDescriptorHeap[SamplerStartIndex];
    SamplerState MetallicRoughnessSampler = SamplerDescriptorHeap[SamplerStartIndex + 1];
    SamplerState OcclusionSampler = SamplerDescriptorHeap[SamplerStartIndex + 2];
    SamplerState EmissiveSampler = SamplerDescriptorHeap[SamplerStartIndex + 3];
    SamplerState NormalSampler = SamplerDescriptorHeap[SamplerStartIndex + 4];

    MaterialProperties MatProps;
    MatProps.BaseColor = baseColorFactor * BaseColorTexture.Sample(BaseColorSampler, UVSET(BASECOLOR_UV_OFFSET, flags));
    float2 metallicRoughness = metallicRoughnessFactor *
        MetallicRoughnessTexture.Sample(MetallicRoughnessSampler, UVSET(METALLICROUGHNESS_UV_OFFSET, flags)).bg;
    metallicRoughness.y = max(0.001, metallicRoughness.y);
    MatProps.Metallic = metallicRoughness.x;
    MatProps.Roughness = metallicRoughness.y;
    MatProps.Occlusion = OcclusionTexture.Sample(OcclusionSampler, UVSET(OCCLUSION_UV_OFFSET, flags));
    MatProps.Emissive = emissiveFactor * EmissiveTexture.Sample(EmissiveSampler, UVSET(EMISSIVE_UV_OFFSET, flags));
    MatProps.Normal = ComputeNormal(vsOutput, NormalTexture, NormalSampler);
    return MatProps;
}

#endif