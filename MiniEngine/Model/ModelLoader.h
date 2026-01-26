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

#pragma once

#include "Model.h"
#include "Animation.h"
#include "ConstantBuffers.h"
#include "../Core/Math/BoundingSphere.h"
#include "../Core/Math/BoundingBox.h"
#include "glTFLoader.h"

#include <cstdint>
#include <vector>

namespace glTF { class Asset; struct Mesh; }

#define CURRENT_MINI_FILE_VERSION 20

namespace Renderer
{
    using namespace Math;

	struct PendingGroupWrite
	{
		std::wstring TempSourcePath;
		uint64_t SourceOffset;
		uint64_t SizeBytes;

		uint32_t BaseGroupIndexPatchValue; // 需要加到 RefineGroupIndex 上的值
	};

	struct GlobalStreamingContext {
		//const std::string FileName = "geometry_build_temp.bin";
		//std::ofstream TempGeoFile;          // 临时文件流

		uint64_t TotalGeometrySize = 0;     // 已写入的总字节数
		uint32_t CurrentPageIndex = 0;      // 当前 Page 索引
		uint32_t CurrentOffsetInPage = 0;   // 当前 Page 内的偏移

		std::vector<char> ZeroBuffer;

		// 延迟写入队列
		std::vector<PendingGroupWrite> PendingWrites;
		// 需要保持生命周期的临时文件列表 (用于最后清理)
		std::vector<std::wstring> TempFilesToClean;

		GlobalStreamingContext() {
			ZeroBuffer.assign(Renderer::kPageSizeInBytes, 0);
			// 创建临时文件
			//TempGeoFile.open(FileName, std::ios::binary | std::ios::out | std::ios::trunc);
		}

		~GlobalStreamingContext() {
			//if (TempGeoFile.is_open()) TempGeoFile.close();
			for (const auto& f : TempFilesToClean) _wremove(f.c_str());
		}
	};

    // Unaligned mirror of MaterialConstants
    struct MaterialConstantData
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

    // Used at load time to construct descriptor tables
    struct MaterialTextureData
    {
        uint16_t stringIdx[kNumTextures];
        uint32_t addressModes;
    };

    // All of the information that needs to be written to a .mini data file
    struct ModelData
    {
        BoundingSphere m_BoundingSphere;
        AxisAlignedBox m_BoundingBox;

		std::vector<GroupMetadata> m_GroupInfos;
		std::vector<HierarchyNode> m_Nodes;
		std::vector<PageMetadata> m_Pages;

        std::vector<unsigned char> m_AnimationKeyFrameData;
        std::vector<AnimationCurve> m_AnimationCurves;
        std::vector<AnimationSet> m_Animations;
        std::vector<uint16_t> m_JointIndices;
        std::vector<Matrix4> m_JointIBMs;
        std::vector<MaterialTextureData> m_MaterialTextures;
        std::vector<MaterialConstantData> m_MaterialConstants;
        std::vector<Mesh*> m_Meshes;
        std::vector<GraphNode> m_SceneGraph;
        std::vector<std::string> m_TextureNames;
        std::vector<uint8_t> m_TextureOptions;
        std::vector<CameraData> m_Cameras;

		uint64_t m_TriangleCount = 0;
    };

    struct FileHeader
    {
        char     id[4];   // "MINI"
        uint32_t version; // CURRENT_MINI_FILE_VERSION
        uint32_t numNodes;
        uint32_t numMeshes;
        uint32_t numMaterials;
        uint32_t numTextures;

		uint64_t geometryBlobOffset;
		uint64_t geometryBlobSize;
		uint32_t groupCount;
		uint32_t hierarchyNodeCount;
        uint32_t pageCount;

        uint32_t stringTableSize;
        uint32_t keyFrameDataSize;      // Animation data
        uint32_t numAnimationCurves;
        uint32_t numAnimations;
        uint32_t numJoints;     // All joints for all skins
        uint32_t numCameras;
        float    boundingSphere[4];
        float    minPos[3];
        float    maxPos[3];
    };

    void CompileMesh(
		ModelData& modelData,
        const cgltf_data* data,
        const cgltf_mesh& srcMesh,
		uint32_t matrixIdx,
		const Matrix4& localToObject,
		Math::BoundingSphere& boundingSphere,
		Math::AxisAlignedBox& boundingBox,
        GlobalStreamingContext& streamCtx
    );

    bool BuildModel( ModelData& model, const glTF::GltfAsset& asset, GlobalStreamingContext& streamCtx, int sceneIdx = -1 );
    bool SaveModel( const std::wstring& filePath, const ModelData& model, GlobalStreamingContext& streamCtx);
    
    std::shared_ptr<Model> LoadModel( const std::wstring& filePath, bool forceRebuild = false );
}