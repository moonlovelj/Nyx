// Runtime shader used to validate recursive Program reflection and binding.

#ifndef PROGRAM_SMOKE_TEST_BONUS
#error PROGRAM_SMOKE_TEST_BONUS must be supplied through ProgramDesc::AddDefine().
#endif

struct SmokeTextureParams
{
    Texture2D<float4> Textures[2];
    SamplerState DynamicSampler;
    SamplerState StaticSampler;
};

struct SmokeLayerParams
{
    Texture2D<float4> PrimaryTexture;
    Texture2D<float4> SecondaryTexture;
    SamplerState Sampler;
    uint Bias;
};

struct SmokeMaterialParams
{
    uint BaseValue;
    SmokeTextureParams TextureParams;
    SmokeLayerParams Layers[2];
};

struct SmokeFrameParams
{
    float3 Direction;
    float ScalarAfterDirection;
    uint Offset;
    float3x3 Basis;
    bool Enabled;
};

struct SmokeNestedInnerParams
{
    uint Value;
    Texture2D<float4> Texture;
};

struct SmokeNestedOuterParams
{
    ParameterBlock<SmokeNestedInnerParams> Inner;
    Texture2D<float4> OuterTexture;
};

ParameterBlock<SmokeMaterialParams> g_Material;
ConstantBuffer<SmokeFrameParams> g_Frame;
ParameterBlock<SmokeNestedOuterParams> g_Nested;
StructuredBuffer<uint> g_Input;
StructuredBuffer<uint> g_InputArray[2];
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint value = g_Material.BaseValue +
        g_Material.Layers[1].Bias +
        g_Frame.Offset +
        g_Nested.Inner.Value +
        g_Input[0] +
        dispatchThreadID.x + 1 +
        PROGRAM_SMOKE_TEST_BONUS;
    float3 transformedDirection = mul(g_Frame.Basis, float3(1.0, 0.0, 0.0));
    value += uint(
        g_Frame.Direction.x +
        g_Frame.Direction.y +
        g_Frame.Direction.z +
        g_Frame.ScalarAfterDirection +
        transformedDirection.x +
        transformedDirection.y +
        transformedDirection.z);
    value += g_Frame.Enabled ? 1u : 0u;
    if (dispatchThreadID.x == 0xffffffff)
    {
        uint textureIndex = dispatchThreadID.y & 1;
        float4 uv = float4(0.0, 0.0, 0.0, 0.0);
        value += asuint(g_Material.TextureParams.Textures[textureIndex]
            .SampleLevel(g_Material.TextureParams.DynamicSampler, uv.xy, 0.0).x);
        value += asuint(g_Material.TextureParams.Textures[textureIndex]
            .SampleLevel(g_Material.TextureParams.StaticSampler, uv.xy, 0.0).x);
        value += asuint(g_Material.Layers[textureIndex].PrimaryTexture
            .SampleLevel(g_Material.Layers[textureIndex].Sampler, uv.xy, 0.0).x);
        value += asuint(g_Material.Layers[textureIndex].SecondaryTexture
            .SampleLevel(g_Material.Layers[textureIndex].Sampler, uv.xy, 0.0).x);
        value += asuint(g_Nested.Inner.Texture.Load(int3(0, 0, 0)).x);
        value += asuint(g_Nested.OuterTexture.Load(int3(0, 0, 0)).x);
        value += g_InputArray[textureIndex][0];
    }
    g_Output[dispatchThreadID.x] = value;
}
