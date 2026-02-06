#ifndef __IBL_HLSLI__
#define __IBL_HLSLI__

#include "Math.hlsli"

float3 ConvertCubePixelToDir(uint X, uint Y, uint Face, uint TextureSize)
{
    // Input: x,y pixel coordinates, width/height per face, face index [0..5]
    // Output: direction vector dir (float3)

    float U = ((float)X + 0.5) / TextureSize; // 0~1
    float V = ((float)Y + 0.5) / TextureSize; // 0~1

    // Map to [-1,1] coordinates, center-aligned
    float FX = 2.0 * U - 1.0;
    float FY = 2.0 * V - 1.0;

    float3 Dir;
    switch (Face)
    {
    case 0:
        Dir = float3(1, -FY, -FX);
        break; // +X
    case 1:
        Dir = float3(-1, -FY, FX);
        break; // -X
    case 2:
        Dir = float3(FX, 1, FY);
        break; // +Y
    case 3:
        Dir = float3(FX, -1, -FY);
        break; // -Y
    case 4:
        Dir = float3(FX, -FY, 1);
        break; // +Z
    case 5:
        Dir = float3(-FX, -FY, -1);
        break; // -Z
    }

    Dir = normalize(Dir);

    return Dir;
}

float ComputeMipLevel(float SampleCount, float PDF, float TextureSize)
{
    //float omegaS = 1.0 / (SampleCount * PDF);
    //float omegaP = 4.0 * PI / (6.0 * TextureSize * TextureSize);
    //float mipLevel = 0.5 * log2(omegaS / omegaP);
    float mipLevel = 0.5 * log2((6.0 * TextureSize * TextureSize) / (SampleCount * PDF * 4.0 * PI));
    return mipLevel;
}

float3 GetOffSpecularPeakReflectionDir(float3 Normal, float3 ReflectionVector, float Roughness)
{
    float a = Roughness * Roughness;
    return lerp(Normal, ReflectionVector, (1 - a) * (sqrt(1 - a) + a));
}

float ComputeIBLMipFromRoughness(float Roughness, float CubemapMaxMip)
{
    // Heuristic that maps roughness to mip level
    float LevelFrom1x1 = 1.0f - 1.2f * log2(max(Roughness, 0.001));
    return CubemapMaxMip - 1 - LevelFrom1x1;
}

float ComputeIBLRoughnessFromMip(float Mip, float CubemapMaxMip)
{
    float LevelFrom1x1 = CubemapMaxMip - 1 - Mip;
    return exp2((1.0f - LevelFrom1x1) / 1.2f);
}

#endif
