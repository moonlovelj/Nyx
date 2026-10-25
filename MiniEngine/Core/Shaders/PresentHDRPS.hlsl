//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:  James Stanard 
//

#include "PresentRS.hlsli"
#include "HDRDisplayMapping.hlsli"

Texture2D<float3> MainBuffer : register(t0);

cbuffer CB0 : register(b0)
{
    float PaperWhiteRatio; // PaperWhite / 10000.0
    float MaxBrightness;
}

[RootSignature(Present_RootSig)]
float3 main( float4 position : SV_Position, float2 uv : TexCoord0 ) : SV_Target0
{
    return MapSceneToHDR10(MainBuffer[(int2)position.xy], PaperWhiteRatio, MaxBrightness);
}
