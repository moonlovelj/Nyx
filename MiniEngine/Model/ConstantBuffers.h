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
// Author:   James Stanard
//

#pragma once

#include "../Core/Math/Matrix3.h"
#include "../Core/Math/Matrix4.h"
#include "glTFLoader.h"
#include <cstdint>

struct InstanceConstants
{
    float BBoxMin[3];
    float BBoxMax[3];
    uint32_t MeshBufferIdx;
    uint32_t JointBufferIdx;
};

// Per-instance
struct MeshConstants
{
    Math::Matrix4 World;         // Local to world
    Math::Matrix3 WorldIT;       // Local normal to world normal
};

// The order of textures for PBR materials
enum { kBaseColor, kMetallicRoughness, kOcclusion, kEmissive, kNormal, kSpecular, kSpecularColor, kNumTextures };
static_assert((uint32_t)kNumTextures == (uint32_t)glTF::Material::eMaterialTexture::kNumTextures);

// Per-model
struct MaterialConstants
{
    float baseColorFactor[4]; // default=[1,1,1,1]
    float emissiveFactor[3]; // default=[0,0,0]
    float normalTextureScale; // default=1
    float metallicFactor; // default=1
    float roughnessFactor; // default=1
    union
    {
        uint32_t flags;
        struct
        {
            // UV0 or UV1 for each texture
            uint32_t baseColorUV : 1;
            uint32_t metallicRoughnessUV : 1;
            uint32_t occlusionUV : 1;
            uint32_t emissiveUV : 1;
            uint32_t normalUV : 1;

            // Three special modes
            uint32_t twoSided : 1;
            uint32_t alphaTest : 1;
            uint32_t alphaBlend : 1;

            uint32_t _pad : 8;

            uint32_t alphaRef : 16; // half float
        };
    };

    uint32_t TextureStartIndex;
    uint32_t SamplerStartIndex;
};

__declspec(align(256)) struct GlobalConstants
{
    Math::Matrix4 ViewMatrix;
    Math::Vector4 ViewSpaceFrustumPlanes[6];
    Math::Matrix4 ViewProjMatrix;
    Math::Matrix4 ProjMatrix;
    Math::Matrix4 InverseViewProjMatrix;
	Math::Matrix4 PrevViewMatrix;
	Math::Matrix4 PrevViewProjMatrix;
    Math::Matrix4 PrevProjMatrix;
    Math::Matrix4 SunShadowMatrix;
    Math::Vector3 ViewerPos;
    Math::Vector3 PrevViewerPos;
    Math::Vector3 SunDirection;
    Math::Vector3 SunIntensity;
    float ShadowTexelSize[4];
    float InvTileDim[4];
    float HZBSizeAndInv[4];
    uint32_t TileCount[4];
    uint32_t FirstLightIndex[4];
    uint32_t ViewportWidth;
    uint32_t ViewportHeight;
	float InvViewportWidth;
	float InvViewportHeight;
    uint32_t FrameIndexMod2;
    uint32_t IBLLutTextureSize;
    uint32_t IBLSpecularLDMapMipCount;
    uint32_t BindlessResourcesBaseIndex;
};
