#include "Common.hlsli"
#include "MaterialCommon.hlsli"

struct MRT
{
    float4 Color : SV_Target0;
    float4 GBufferA : SV_Target1;
    float4 GBufferB : SV_Target2;
    float4 GBufferC : SV_Target3;
    float4 GBufferD : SV_Target4;
};

[RootSignature(Renderer_RootSig)]
MRT main(VSOutput vsOutput)
{
    MaterialProperties MatProps = GetMaterialProperties(vsOutput);

    MRT mrt;
    mrt.Color = float4(MatProps.Emissive, 1.0f);
    mrt.GBufferA = float4(MatProps.Normal, 1.0);
    mrt.GBufferB = MatProps.BaseColor;
    mrt.GBufferC = float4(MatProps.Metallic, MatProps.Roughness, MatProps.Occlusion, 0.f);
    mrt.GBufferD = 0;

    return mrt;
}

