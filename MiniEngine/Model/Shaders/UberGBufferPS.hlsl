#include "Common.hlsli"
#include "ViewMode.hlsli"
#include "CommonResources.hlsli"

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float3 sunShadowCoord : TEXCOORD3;
    uint meshletIndex : TEXCOORD4;
};


struct MRT
{
    float4 Color : SV_Target0;
    float4 GBufferA : SV_Target1;
    float4 GBufferB : SV_Target2;
    float4 GBufferC : SV_Target3;
    float4 GBufferD : SV_Target4;
};

// Flag helpers
static const uint BASECOLOR_UV_OFFSET = 0;
static const uint METALLICROUGHNESS_UV_OFFSET = 1;
static const uint OCCLUSION_UV_OFFSET = 2;
static const uint EMISSIVE_UV_OFFSET = 3;
static const uint NORMAL_UV_OFFSET = 4;

float2 ParseUV(in VSOutput vsOutput, uint offset, uint flags, uint psoFlags)
{
    if (psoFlags & PSO_HAS_UV1)
    {
        return lerp(vsOutput.uv0, vsOutput.uv1, (flags >> offset) & 1);
    }
    
    return vsOutput.uv0;
}

float3 ComputeNormal(VSOutput vsOutput, Texture2D<float3> NormalTexture, SamplerState NormalSampler)
{
    MeshletConstant meshletConstant = GetMeshletConstantSRV(vsOutput.meshletIndex);
    MaterialConstant materilConstant = GetMaterialConstantSRV(meshletConstant.MaterialConstantsIndex);
    float normalTextureScale = materilConstant.normalTextureScale;
    uint flags = materilConstant.flags;

    float3 normal = normalize(vsOutput.normal);
    
    if (!(meshletConstant.psoFlags & PSO_HAS_TANGENT))
    {
        return normal;
    }

    // Construct tangent frame
    float3 tangent = normalize(vsOutput.tangent.xyz);
    float3 bitangent = normalize(cross(normal, tangent)) * vsOutput.tangent.w;
    float3x3 tangentFrame = float3x3(tangent, bitangent, normal);

    // Read normal map and convert to SNORM (TODO:  convert all normal maps to R8G8B8A8_SNORM?)
    normal = NormalTexture.Sample(NormalSampler, ParseUV(vsOutput, NORMAL_UV_OFFSET, flags, meshletConstant.psoFlags)) * 2.0 - 1.0;

    // glTF spec says to normalize N before and after scaling, but that's excessive
    normal = normalize(normal * float3(normalTextureScale, normalTextureScale, 1));

    // Multiply by transpose (reverse order)
    return mul(normal, tangentFrame);
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
    MeshletConstant meshletConstant = GetMeshletConstantSRV(vsOutput.meshletIndex);
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
    
    if (meshletConstant.psoFlags & PSO_ALPHA_TEST)
    {
        float cutoff = f16tof32(materilConstant.flags >> 16);
        if (BaseColorTexture.Sample(BaseColorSampler, vsOutput.uv0).a < cutoff)
        {
            discard;
        }
    }

    MaterialProperties MatProps;
    MatProps.BaseColor = baseColorFactor * BaseColorTexture.Sample(BaseColorSampler, ParseUV(vsOutput, BASECOLOR_UV_OFFSET, flags, meshletConstant.psoFlags));
    float2 metallicRoughness = metallicRoughnessFactor *
        MetallicRoughnessTexture.Sample(MetallicRoughnessSampler, ParseUV(vsOutput, METALLICROUGHNESS_UV_OFFSET, flags, meshletConstant.psoFlags)).bg;
    metallicRoughness.y = max(0.001, metallicRoughness.y);
    MatProps.Metallic = metallicRoughness.x;
    MatProps.Roughness = metallicRoughness.y;
    MatProps.Occlusion = OcclusionTexture.Sample(OcclusionSampler, ParseUV(vsOutput, OCCLUSION_UV_OFFSET, flags, meshletConstant.psoFlags));
    MatProps.Emissive = emissiveFactor * EmissiveTexture.Sample(EmissiveSampler, ParseUV(vsOutput, EMISSIVE_UV_OFFSET, flags, meshletConstant.psoFlags));
    MatProps.Normal = ComputeNormal(vsOutput, NormalTexture, NormalSampler);
    return MatProps;
}

[RootSignature(Renderer_RootSig)]
MRT main(VSOutput vsOutput)
{
    MaterialProperties MatProps = GetMaterialProperties(vsOutput);

    MRT mrt;
    mrt.Color = float4(MatProps.Emissive, 1.0f);
    mrt.GBufferA = float4(MatProps.Normal, 1.0);
    mrt.GBufferB = MatProps.BaseColor;
    mrt.GBufferC = float4(MatProps.Metallic, MatProps.Roughness, MatProps.Occlusion, 0.f);
    mrt.GBufferD.a = ViewMode;
    
    if (ViewMode == VIEW_MODE_SHOW_MESHLET_LOD)
    {
        MeshletConstant meshletConstant = GetMeshletConstantSRV(vsOutput.meshletIndex);
        mrt.GBufferD.rgb = Uint32ToColorR16G16B16(meshletConstant.lodLevel);
    }
    else if (ViewMode == VIEW_MODE_SHOW_MESHLET_ID)
    {
        mrt.GBufferD.rgb = Uint32ToColorR16G16B16(vsOutput.meshletIndex);
    }
    //else if (ViewMode == VIEW_MODE_SHOW_TRIANGLE)
    //{
    //    mrt.GBufferD.rgb = Uint32ToColorR16G16B16(primID);
    //}
    else
    {
        mrt.GBufferD.rgb = 0;
    }

    return mrt;
}

