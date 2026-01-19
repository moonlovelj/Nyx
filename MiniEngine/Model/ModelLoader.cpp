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
// Author(s):  James Stanard
//             Chuck Walbourn (ATG)
//
// This code depends on DirectXTex
//

#include "ModelLoader.h"
#include "Renderer.h"
#include "Model.h"
#include "glTFLoader.h"
#include "ModelH3D.h"
#include "TextureManager.h"
#include "TextureConvert.h"
#include "GraphicsCommon.h"

#include <fstream>
#include <unordered_map>

using namespace Renderer;
using namespace Graphics;

std::unordered_map<uint32_t, uint32_t> g_SamplerPermutations;

D3D12_CPU_DESCRIPTOR_HANDLE GetSampler(uint32_t addressModes)
{
    SamplerDesc samplerDesc;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(addressModes & 0x3);
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(addressModes >> 2);
    return samplerDesc.CreateDescriptor();
}

std::vector<uint32_t> LoadMaterials(Model& model,
    const std::vector<MaterialTextureData>& materialTextures,
    const std::vector<std::wstring>& textureNames,
    const std::vector<uint8_t>& textureOptions,
    const std::wstring& basePath)
{
    //static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

    // Load textures
    const uint32_t numTextures = (uint32_t)textureNames.size();
    model.textures.resize(numTextures);
    for (size_t ti = 0; ti < numTextures; ++ti)
    {
        std::wstring originalFile = basePath + textureNames[ti];
        CompileTextureOnDemand(originalFile, textureOptions[ti]);

        std::wstring ddsFile = Utility::RemoveExtension(originalFile) + L".dds";
        model.textures[ti] = TextureManager::LoadDDSFromFile(ddsFile);
    }

    // Generate descriptor tables and record offsets for each material
    const uint32_t numMaterials = (uint32_t)materialTextures.size();
    std::vector<uint32_t> tableOffsets(numMaterials);

    for (uint32_t matIdx = 0; matIdx < numMaterials; ++matIdx)
    {
        const MaterialTextureData& srcMat = materialTextures[matIdx];

        DescriptorHandle TextureHandles = Renderer::s_TextureHeap.Alloc(kNumTextures);
        uint32_t SRVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(TextureHandles);

        uint32_t DestCount = kNumTextures;
        uint32_t SourceCounts[kNumTextures] = { 1, 1, 1, 1, 1, 1 };

        D3D12_CPU_DESCRIPTOR_HANDLE DefaultTextures[kNumTextures] =
        {
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kBlackTransparent2D),
            GetDefaultTexture(kDefaultNormalMap),
            GetDefaultTexture(kWhiteOpaque2D)
        };

        D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[kNumTextures];
        for (uint32_t j = 0; j < kNumTextures; ++j)
        {
            if (srcMat.stringIdx[j] == 0xffff)
                SourceTextures[j] = DefaultTextures[j];
            else
                SourceTextures[j] = model.textures[srcMat.stringIdx[j]].GetSRV();
        }

        g_Device->CopyDescriptors(1, &TextureHandles, &DestCount,
            DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // See if this combination of samplers has been used before.  If not, allocate more from the heap
        // and copy in the descriptors.
        uint32_t addressModes = srcMat.addressModes;
        auto samplerMapLookup = g_SamplerPermutations.find(addressModes);

        if (samplerMapLookup == g_SamplerPermutations.end())
        {
            DescriptorHandle SamplerHandles = Renderer::s_SamplerHeap.Alloc(kNumTextures);
            uint32_t SamplerDescriptorTable = Renderer::s_SamplerHeap.GetOffsetOfHandle(SamplerHandles);
            g_SamplerPermutations[addressModes] = SamplerDescriptorTable;
            tableOffsets[matIdx] = SRVDescriptorTable | SamplerDescriptorTable << 16;

            D3D12_CPU_DESCRIPTOR_HANDLE SourceSamplers[kNumTextures];
            for (uint32_t j = 0; j < kNumTextures; ++j)
            {
                SourceSamplers[j] = GetSampler(addressModes & 0xF);
                addressModes >>= 4;
            }
            g_Device->CopyDescriptors(1, &SamplerHandles, &DestCount,
                DestCount, SourceSamplers, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        else
        {
            tableOffsets[matIdx] = SRVDescriptorTable | samplerMapLookup->second << 16;
        }
    }

    // Update table offsets for each mesh
	//uint8_t* meshPtr = model.m_MeshData.get();
	//for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
	//{
	//	Mesh& mesh = *(Mesh*)meshPtr;
	//	uint32_t offsetPair = tableOffsets[mesh.materialCBV];
	//	mesh.srvTable = offsetPair & 0xFFFF;
	//	mesh.samplerTable = offsetPair >> 16;
	//	mesh.pso = Renderer::GetPSO(mesh.psoFlags);
	//	meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
	//}

    return tableOffsets;
}

std::shared_ptr<Model> Renderer::LoadModel(const std::wstring& filePath, bool forceRebuild)
{
    const std::wstring miniFileName = Utility::RemoveExtension(filePath) + L".mini";
    const std::wstring fileName = Utility::RemoveBasePath(filePath);

    struct _stat64 sourceFileStat;
    struct _stat64 miniFileStat;
    std::ifstream inFile;
    FileHeader header;

    bool sourceFileMissing = _wstat64(filePath.c_str(), &sourceFileStat) == -1;
    bool miniFileMissing = _wstat64(miniFileName.c_str(), &miniFileStat) == -1;

    if (sourceFileMissing)
        forceRebuild = false;

    if (sourceFileMissing && miniFileMissing)
    {
        Utility::Printf("Error: Could not find %ws\n", fileName.c_str());
        return nullptr;
    }

    bool needBuild = forceRebuild;

    // Check if .mini file exists and it is newer than source file
    if (miniFileMissing || !sourceFileMissing && sourceFileStat.st_mtime > miniFileStat.st_mtime)
        needBuild = true;

    // Check if it's an older version of .mini
    if (!needBuild)
    {
        inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
        inFile.read((char*)&header, sizeof(FileHeader));
        if (strncmp(header.id, "MINI", 4) != 0 || header.version != CURRENT_MINI_FILE_VERSION)
        {
            Utility::Printf("Model version deprecated.  Rebuilding %ws...\n", fileName.c_str());
            needBuild = true;
            inFile.close();
        }
    }

    if (needBuild)
    {
        if (sourceFileMissing)
        {
            Utility::Printf("Error: Could not find %ws\n", fileName.c_str());
            return nullptr;
        }

        ModelData modelData;
        GlobalStreamingContext streamCtx;

        const std::wstring fileExt = Utility::ToLower(Utility::GetFileExtension(filePath));

        if (fileExt == L"gltf" || fileExt == L"glb")
        {
            glTF::GltfAsset asset(filePath);
            if (!BuildModel(modelData, asset, streamCtx))
                return nullptr;
        }
        else if (fileExt == L"h3d")
        {
            ModelH3D modelh3d;
            const std::wstring basePath = Utility::GetBasePath(filePath);
            if (!modelh3d.Load(filePath) || !modelh3d.BuildModel(modelData, streamCtx, basePath))
                return nullptr;
        }
        else
        {
            Utility::Printf(L"Unsupported model file extension: %ws\n", fileExt.c_str());
            return nullptr;
        }

        if (!SaveModel(miniFileName, modelData, streamCtx))
            return nullptr;

        inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
        inFile.read((char*)&header, sizeof(FileHeader));
    }

    if (!inFile)
        return nullptr;

    ASSERT(strncmp(header.id, "MINI", 4) == 0 && header.version == CURRENT_MINI_FILE_VERSION);

    std::wstring basePath = Utility::GetBasePath(filePath);

    std::shared_ptr<Model> model(new Model);

    model->m_StreamingFilePath = miniFileName;
	model->m_GeometryBlobOffsetInFile = header.geometryBlobOffset;

    model->m_NumNodes = header.numNodes;
    model->m_SceneGraph.reset(new GraphNode[header.numNodes]);
    model->m_NumMeshes = header.numMeshes;


    inFile.read((char*)model->m_SceneGraph.get(), header.numNodes * sizeof(GraphNode));

	for (uint32_t i = 0; i < header.numMeshes; ++i)
	{
		// 先读固定头部，拿到 numDraws
		Mesh tempHead{};
		inFile.read(reinterpret_cast<char*>(&tempHead), sizeof(Mesh));
		if (!inFile) return nullptr;

		// 计算真实字节数并分配
		const size_t meshSize = sizeof(Mesh) + sizeof(Mesh::Draw) * (tempHead.numDraws - 1);
		Mesh* mesh = static_cast<Mesh*>(std::malloc(meshSize));
		if (!mesh) return nullptr;

		// 将已读头部复制到目标内存
		std::memcpy(mesh, &tempHead, sizeof(Mesh));

		// 读取剩余的 Draw 数组数据（如果有）
		const size_t remaining = meshSize - sizeof(Mesh);
		if (remaining > 0)
		{
			inFile.read(reinterpret_cast<char*>(mesh) + sizeof(Mesh), remaining);
			if (!inFile) { std::free(mesh); return nullptr; }
		}

		model->m_Meshes.push_back(mesh);
	}

    UploadBuffer materialConstants;
	if (header.numMaterials > 0)
	{
		materialConstants.Create(L"Material Constant Upload", header.numMaterials * sizeof(MaterialConstants));
		MaterialConstants* materialCBV = (MaterialConstants*)materialConstants.Map();
		for (uint32_t i = 0; i < header.numMaterials; ++i)
		{
			inFile.read((char*)materialCBV, sizeof(MaterialConstantData));
			materialCBV++;
		}
		materialConstants.Unmap();
	}

    // Read material texture and sampler properties so we can load the material
    std::vector<MaterialTextureData> materialTextures(header.numMaterials);
    inFile.read((char*)materialTextures.data(), header.numMaterials * sizeof(MaterialTextureData));

    std::vector<std::wstring> textureNames(header.numTextures);
    for (uint32_t i = 0; i < header.numTextures; ++i)
    {
        std::string utf8TextureName;
        std::getline(inFile, utf8TextureName, '\0');
        textureNames[i] = Utility::UTF8ToWideString(utf8TextureName);
    }

    std::vector<uint8_t> textureOptions(header.numTextures);
    inFile.read((char*)textureOptions.data(), header.numTextures * sizeof(uint8_t));

    std::vector<uint32_t> tableOffsets = LoadMaterials(*model, materialTextures, textureNames, textureOptions, basePath);
    if (header.numMaterials > 0)
    {
        MaterialConstants* materialCBV = (MaterialConstants*)materialConstants.Map();
        for (uint32_t i = 0; i < header.numMaterials; ++i)
        {
			const uint32_t offsetPair = tableOffsets[i];
            materialCBV[i].TextureStartIndex = offsetPair & 0xFFFF;
            materialCBV[i].SamplerStartIndex = offsetPair >> 16;
        }

        materialConstants.Unmap();
        model->m_MaterialConstants.Create(L"Material Constants", header.numMaterials, sizeof(MaterialConstants), materialConstants);
    }

    model->m_BoundingSphere = BoundingSphere(*(XMFLOAT4*)header.boundingSphere);
    model->m_BoundingBox = AxisAlignedBox(Vector3(*(XMFLOAT3*)header.minPos), Vector3(*(XMFLOAT3*)header.maxPos));

    // Load animation data
    model->m_NumAnimations = header.numAnimations;

    if (header.numAnimations > 0)
    {
        ASSERT(header.keyFrameDataSize > 0 && header.numAnimationCurves > 0);
        model->m_KeyFrameData.reset(new uint8_t[header.keyFrameDataSize]);
        inFile.read((char*)model->m_KeyFrameData.get(), header.keyFrameDataSize);
        model->m_CurveData.reset(new AnimationCurve[header.numAnimationCurves]);
        inFile.read((char*)model->m_CurveData.get(), header.numAnimationCurves * sizeof(AnimationCurve));
        model->m_Animations.reset(new AnimationSet[header.numAnimations]);
        inFile.read((char*)model->m_Animations.get(), header.numAnimations * sizeof(AnimationSet));
    }

    model->m_NumJoints = header.numJoints;

    if (header.numJoints > 0)
    {
        model->m_JointIndices.reset(new uint16_t[header.numJoints]);
        inFile.read((char*)model->m_JointIndices.get(), header.numJoints * sizeof(uint16_t));
        model->m_JointIBMs.reset(new Matrix4[header.numJoints]);
        inFile.read((char*)model->m_JointIBMs.get(), header.numJoints * sizeof(Matrix4));
    }

    model->m_NumCameras = header.numCameras;
    if (header.numCameras > 0)
    {
        model->m_Cameras.reset(new CameraData[header.numCameras]);
        inFile.read((char*)model->m_Cameras.get(), header.numCameras * sizeof(CameraData));
    }

	// --- 读取 Cluster LOD Data ---
	// Group Infos (目录表)
	if (header.groupCount > 0)
	{
		model->m_GroupMetadatas.resize(header.groupCount);
		inFile.read((char*)model->m_GroupMetadatas.data(), header.groupCount * sizeof(GroupMetadata));
	}

	// BVH Nodes
	if (header.hierarchyNodeCount > 0)
	{
		model->m_Nodes.resize(header.hierarchyNodeCount);
		inFile.read((char*)model->m_Nodes.data(), header.hierarchyNodeCount * sizeof(HierarchyNode));
	}

	// Page Metadatas
    if (header.pageCount > 0)
    {
        model->m_PageMetadatas.resize(header.pageCount);
        inFile.read((char*)model->m_PageMetadatas.data(), header.pageCount * sizeof(PageMetadata));
	}

    return model;
}
