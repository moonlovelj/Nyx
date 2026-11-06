#include "PostEffectsRS.hlsli"

StructuredBuffer<float> Exposure : register(t0);
RWTexture2D<float> FrameExposure : register(u0);

[RootSignature(PostEffects_RootSig)]
[numthreads(1, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    FrameExposure[uint2(0, 0)] = Exposure[0];
}
