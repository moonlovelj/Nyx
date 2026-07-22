// Reflection-driven root-signature layout coverage.

struct LayoutConstants
{
    uint Value;
};

struct LayoutRootConstants
{
    uint2 Value;
};

ConstantBuffer<LayoutConstants> g_CBV : register(b2, space1);
ParameterBlock<LayoutRootConstants> g_Root : register(b3, space1);

StructuredBuffer<uint> g_RootSRV : register(t3, space2);
RWStructuredBuffer<uint> g_RootUAV : register(u4, space2);

Texture2D<float4> g_TextureA : register(t0, space0);
Texture2D<float4> g_TextureB[2] : register(t1, space0);
Texture2D<float4> g_TextureInAnotherSpace : register(t0, space3);

RWStructuredBuffer<uint> g_UAVA : register(u0, space0);
RWStructuredBuffer<uint> g_UAVB[2] : register(u1, space0);

SamplerState g_SamplerA : register(s0, space0);
SamplerState g_SamplerB[2] : register(s1, space0);
SamplerState g_StaticSampler : register(s3, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint value = g_CBV.Value + g_Root.Value.x + g_Root.Value.y + g_RootSRV[0];
    value += asuint(g_TextureA.Load(int3(0, 0, 0)).x);
    value += asuint(g_TextureB[dispatchThreadID.x & 1].Load(int3(0, 0, 0)).x);
    value += asuint(g_TextureInAnotherSpace.SampleLevel(g_StaticSampler, float2(0.0), 0.0).x);
    value += asuint(g_TextureA.SampleLevel(g_SamplerA, float2(0.0), 0.0).x);
    value += asuint(g_TextureB[0].SampleLevel(g_SamplerB[1], float2(0.0), 0.0).x);
    g_UAVA[0] = value;
    g_UAVB[0][0] = value + 1;
    g_UAVB[1][0] = value + 2;
    g_RootUAV[0] = value + 3;
}
