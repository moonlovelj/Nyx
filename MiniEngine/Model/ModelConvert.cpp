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
#include "glTFLoader.h"
#include "TextureConvert.h"
#include "MeshConvert.h"
#include "TextureManager.h"
#include "GraphicsCommon.h"
#include "../Core/Utility.h"
#include "../Core/Math/Common.h"
#include "../Core/Math/Quaternion.h"
#include "lz4.h"

#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <execution>
#include <omp.h>
#include <atomic>
#include <limits>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <execution>

using namespace DirectX;
using namespace Math;
using namespace Renderer;
using namespace Graphics;

#include "MeshletBuilder.h"
#include "MeshletStructs.h"

// Compiled primitive metadata (no vertex data, references only)
struct CompiledPrimitive
{
	uint32_t rootNodeIndex;         // Start index in the global BVH node array
	uint32_t indexCount;            // Index count (for stats or debugging)
	//Math::BoundingSphere boundsLS;  // Local-space bounding sphere (Local Space)
    AxisAlignedBox bboxLS;          // Local-space bounding box (Local Space)
	uint16_t materialIdx;
	uint16_t psoFlags;
	uint16_t vertexStride;
};

// Compiled mesh metadata
struct CompiledMeshData
{
	std::vector<CompiledPrimitive> primitives;
	Math::BoundingSphere boundsLS;  // Local-space bounding sphere for the whole mesh
	Math::AxisAlignedBox bboxLS;    // Local-space bounding box for the whole mesh
};

// Intermediate build results (for parallel -> serial transfer)
struct MeshBuildResult
{
	const cgltf_mesh* sourceMesh;
	CompiledMeshData meshData;
	// Logical build results not yet serialized to disk, using local zero-based indices
	std::vector<MeshletBuildProducts> logicProducts;
	std::wstring tempFilePath;
};

// Collected requests
struct MeshInstanceRequest
{
	const cgltf_mesh* mesh;
	uint32_t nodeIndex;
	Matrix4 worldXform;
};

// Geometry cache: avoid parsing/building the same glTF mesh repeatedly
using MeshCache = std::unordered_map<const cgltf_mesh*, CompiledMeshData>;
// Node map: for animation and skinning association
using NodeMap = std::unordered_map<const cgltf_node*, uint32_t>;

/**
 * Compile mesh geometry in parallel.
 * Build meshlets in parallel in memory.
 * Serially patch indices and write to disk.
 */
static void ParallelCompileMeshes(
	ModelData& modelData,
	MeshCache& meshCache,
	const cgltf_data* data,
	const std::vector<MeshInstanceRequest>& requests,
	GlobalStreamingContext& streamCtx
)
{
	// Collect unique mesh tasks
	std::vector<const cgltf_mesh*> uniqueMeshes;
	{
		std::unordered_set<const cgltf_mesh*> seen;
		for (const auto& req : requests)
		{
			if (seen.insert(req.mesh).second)
				uniqueMeshes.push_back(req.mesh);
		}
	}

	std::vector<MeshBuildResult> buildResults(uniqueMeshes.size());

	// Auxiliary indices for parallel traversal
	std::vector<size_t> taskIndices(uniqueMeshes.size());
	for (size_t i = 0; i < taskIndices.size(); ++i) taskIndices[i] = i;

	Utility::Printf("Compiling %zu unique meshes in parallel...\n", uniqueMeshes.size());

	// Atomic counter for parallel progress
	std::atomic<uint32_t> processedCount = 0;
	const uint32_t totalMeshes = (uint32_t)uniqueMeshes.size();

	// Run meshlet build in parallel (compute bounds)
	//int maxThreads = std::max(1, (int)omp_get_max_threads());
	int workerCount = (int)omp_get_max_threads();//std::min((int)omp_get_max_threads(), 8);

	#pragma omp parallel for schedule(dynamic, 1) num_threads(workerCount)
	for (int taskIndex = 0; taskIndex < taskIndices.size(); taskIndex++)
	{
		const cgltf_mesh* srcMesh = uniqueMeshes[taskIndex];
		MeshBuildResult& result = buildResults[taskIndex];
		result.sourceMesh = srcMesh;

		// Create a unique temp file for this task
		wchar_t tempPath[MAX_PATH];
		GetTempPathW(MAX_PATH, tempPath);
		wchar_t tempFileName[MAX_PATH];
		// Use taskIndex as part of the unique identifier
		swprintf_s(tempFileName, L"%sNYX_TASK_%d.tmp", tempPath, taskIndex);
		result.tempFilePath = tempFileName;

		// Open the task's temp file stream (binary write)
		std::ofstream localTempFile(result.tempFilePath, std::ios::out | std::ios::binary | std::ios::trunc);
		uint64_t localFileCursor = 0;

		// Initialize
		result.meshData.boundsLS = BoundingSphere(kZero);
		result.meshData.bboxLS = AxisAlignedBox(kZero);

		Matrix4 identityXform(kIdentity);
		std::vector<Primitive> primitives(srcMesh->primitives_count);
		BoundingSphere sphereAccum(kZero);
		AxisAlignedBox bboxAccum(kZero);

		// Optimization and bounds calculation
		for (uint32_t p = 0; p < primitives.size(); ++p)
		{
			OptimizeMesh(primitives[p], data, srcMesh->primitives[p], identityXform);
			sphereAccum = sphereAccum.Union(primitives[p].m_BoundsOS);
			bboxAccum.AddBoundingBox(primitives[p].m_BBoxOS);
		}
		result.meshData.boundsLS = sphereAccum;
		result.meshData.bboxLS = bboxAccum;

		// Grouping
		std::map<uint32_t, std::vector<Primitive*>> renderGroups;
		for (auto& prim : primitives)
			renderGroups[prim.hash].push_back(&prim);

		// Meshlet build
		for (auto& iter : renderGroups)
		{
			const auto& groupPrims = iter.second;
			if (groupPrims.empty()) continue;

			MeshletBuildArgs buildArgs = {};
			buildArgs.meshBufferIndex = 0;
			buildArgs.materialBufferIndex = groupPrims[0]->materialIdx;
			buildArgs.psoFlags = groupPrims[0]->psoFlags;
			// Global offsets are unknown in parallel; set to 0 and patch later
			buildArgs.baseGroupIndex = 0;
			buildArgs.baseNodeIndex = 0;

			for (Primitive* draw : groupPrims)
			{
				buildArgs.vertexStride = draw->vertexStride;
				buildArgs.vertexCount = static_cast<uint32_t>(draw->VB->size() / draw->vertexStride);
				buildArgs.VBData = draw->VB->data();
				buildArgs.IBData = draw->IB->data();
				buildArgs.indexCount = draw->primCount;

				// --- Build meshlets ---
				auto products = MeshletBuilder::Build(buildArgs);

				for (auto& group : products.Groups)
				{
					if (!group.Blob.empty())
					{
						group.TempFileOffset = localFileCursor; 
						localTempFile.write(reinterpret_cast<const char*>(group.Blob.data()), group.Blob.size());
						localFileCursor += group.Blob.size();
						// Free memory
						std::vector<uint8_t>().swap(group.Blob);
					}
				}

				CompiledPrimitive finalPrim = {};
				finalPrim.rootNodeIndex = 0; // Patch later
				finalPrim.materialIdx = draw->materialIdx;
				finalPrim.psoFlags = draw->psoFlags;
				finalPrim.vertexStride = draw->vertexStride;
				finalPrim.indexCount = draw->primCount;
				//finalPrim.boundsLS = draw->m_BoundsLS;
                finalPrim.bboxLS = draw->m_BBoxLS;

				result.meshData.primitives.push_back(finalPrim);
				result.logicProducts.push_back(std::move(products));
			}
		}

		localTempFile.close();

		// Update and print progress
		uint32_t current = ++processedCount;
		uint32_t step = std::max(1u, totalMeshes / 20);
		if (current % step == 0 || current == totalMeshes)
		{
			float percent = (float)current / totalMeshes * 100.0f;
			Utility::Printf("Compiling Meshes: %u/%u (%.1f%%)\n", current, totalMeshes, percent);
		}
	};

	// Serial commit and patch
	for (auto& result : buildResults)
	{
		streamCtx.TempFilesToClean.push_back(result.tempFilePath);

		size_t primIndex = 0;
		for (auto& products : result.logicProducts)
		{
			CompiledPrimitive& finalPrim = result.meshData.primitives[primIndex++];

			// Get current global base offsets
			const uint32_t baseGroupIndex = static_cast<uint32_t>(modelData.m_GroupInfos.size());
			const uint32_t baseNodeIndex = static_cast<uint32_t>(modelData.m_Nodes.size());

			finalPrim.rootNodeIndex = baseNodeIndex;

			// Process binary blob (geometry groups)
			for (auto& group : products.Groups)
			{
				const uint32_t groupSize = group.Metadata.SizeBytes;
				if (streamCtx.CurrentOffsetInPage + groupSize > Renderer::kPageSizeInBytes)
				{
					uint32_t paddingSize = Renderer::kPageSizeInBytes - streamCtx.CurrentOffsetInPage;
					if (paddingSize > 0 && paddingSize < Renderer::kPageSizeInBytes)
					{
						PendingGroupWrite padJob = {};
						padJob.TempSourcePath = L""; // Empty means padding
						padJob.SizeBytes = paddingSize;
						streamCtx.PendingWrites.push_back(padJob);

						streamCtx.TotalGeometrySize += paddingSize;
					}

					PageMetadata page = { static_cast<uint32_t>(modelData.m_GroupInfos.size()), 0 };
					modelData.m_Pages.push_back(page);
					streamCtx.CurrentPageIndex = static_cast<uint32_t>(modelData.m_Pages.size() - 1);
					streamCtx.CurrentOffsetInPage = 0;
				}
				else if (modelData.m_Pages.empty())
				{
					modelData.m_Pages.push_back({ 0, 0 });
				}

				modelData.m_Pages.back().GroupCount++;

				GroupMetadata metadata = group.Metadata;
				metadata.PageIndex = streamCtx.CurrentPageIndex;
				metadata.OffsetInPage = streamCtx.CurrentOffsetInPage;
				modelData.m_GroupInfos.push_back(metadata);

				// Record the actual write to perform later
				PendingGroupWrite job;
				job.TempSourcePath = result.tempFilePath; // Which file holds the data
				job.SourceOffset = group.TempFileOffset;  // Offset within the file
				job.SizeBytes = groupSize;                // Size
				job.BaseGroupIndexPatchValue = baseGroupIndex; // Patch value
				streamCtx.PendingWrites.push_back(job);


				streamCtx.CurrentOffsetInPage += groupSize;
				streamCtx.TotalGeometrySize += groupSize;
			}

			// Process and patch BVH nodes
			for (auto& node : products.Hierarchy)
			{
				if (node.Internal.IsGroup == 0) // Internal Node
				{
					// Patch child node indices in the global node array
					node.Internal.ChildStartIndex += baseNodeIndex;
				}
				else // Leaf Node
				{
					// Patch cluster group indices in the global group array
					node.Leaf.GroupIndex += baseGroupIndex;
				}
			}
			modelData.m_Nodes.insert(modelData.m_Nodes.end(), products.Hierarchy.begin(), products.Hierarchy.end());
			modelData.m_TriangleCount += finalPrim.indexCount / 3;
		}

		// Store in cache
		meshCache[result.sourceMesh] = std::move(result.meshData);

		//localTempFileIn.close();
		//_wremove(result.tempFilePath.c_str());
	}
}

/**
 * Instantiate a mesh.
 * Create a Renderer::Mesh tied to a specific scene graph node (matrixIdx) and reuse compiled geometry.
 */
static void InstantiateMesh(
	ModelData& modelData,
	const CompiledMeshData& cachedData,
	uint32_t matrixIdx
)
{
	size_t numDraws = cachedData.primitives.size();
	if (numDraws == 0) return;

	// Allocate compact Mesh struct memory
	// Using malloc here matches modelData's pointer storage design. In very large scenes, if instances are numerous (millions),
	// this heap allocation can cause fragmentation. But to stay compatible with the existing Model structure, this is a necessary compromise.
	// In extreme cases, consider storing meshes in ModelData using a LinearAllocator or std::deque<Mesh>.
	size_t meshStructSize = sizeof(Mesh) + sizeof(Mesh::Draw) * (numDraws - 1);
	Mesh* mesh = (Mesh*)malloc(meshStructSize);

	mesh->numDraws = (uint32_t)numDraws;
	mesh->matrixIdx = static_cast<uint16_t>(matrixIdx);
	mesh->padding = 0;

	for (size_t i = 0; i < numDraws; ++i)
	{
		const auto& prim = cachedData.primitives[i];
		mesh->draw[i].rootNodeIndex = prim.rootNodeIndex;

		//mesh->draw[i].boundingSphere[0] = prim.boundsLS.GetCenter().GetX();
		//mesh->draw[i].boundingSphere[1] = prim.boundsLS.GetCenter().GetY();
		//mesh->draw[i].boundingSphere[2] = prim.boundsLS.GetCenter().GetZ();
		//mesh->draw[i].boundingSphere[3] = prim.boundsLS.GetRadius();
        mesh->draw[i].boundingBoxMin[0] = prim.bboxLS.GetMin().GetX();
        mesh->draw[i].boundingBoxMin[1] = prim.bboxLS.GetMin().GetY();
        mesh->draw[i].boundingBoxMin[2] = prim.bboxLS.GetMin().GetZ();
        mesh->draw[i].boundingBoxMax[0] = prim.bboxLS.GetMax().GetX();
        mesh->draw[i].boundingBoxMax[1] = prim.bboxLS.GetMax().GetY();
        mesh->draw[i].boundingBoxMax[2] = prim.bboxLS.GetMax().GetZ();
	}

	modelData.m_Meshes.push_back(mesh);
}

static uint32_t WalkGraph(
	ModelData& modelData,
	const cgltf_data* data,
    std::vector<GraphNode>& sceneGraph,
    BoundingSphere& modelBSphere,
    AxisAlignedBox& modelBBox,
    const cgltf_node* curNode,
    uint32_t curPos,
    const Matrix4& xform,
    GlobalStreamingContext& streamCtx,
	NodeMap& nodeMap,
	MeshCache& meshCache,
	std::vector<MeshInstanceRequest>& meshRequests
    )
{
	nodeMap[curNode] = curPos;

	GraphNode& node = sceneGraph[curPos];
	node.hasChildren = (curNode->children_count > 0);
	node.matrixIdx = curPos;

	if (curNode->has_matrix) {
		memcpy(&node.xform, curNode->matrix, sizeof(float) * 16);
	}
	else {
		Vector3 translation = curNode->has_translation ? Vector3(curNode->translation[0], curNode->translation[1], curNode->translation[2]) : Vector3(kOrigin);
		Vector3 scale = curNode->has_scale ? Vector3(curNode->scale[0], curNode->scale[1], curNode->scale[2]) : Vector3(kOne);
		Quaternion rotation = curNode->has_rotation ?
			Quaternion(DirectX::XMVectorSet(curNode->rotation[0], curNode->rotation[1], curNode->rotation[2], curNode->rotation[3])) :
			Quaternion(kIdentity);
		node.xform = Matrix4(Matrix3(rotation) * Matrix3::MakeScale(scale), translation);
		node.rotation = rotation;
		node.scale = XMFLOAT3(curNode->scale[0], curNode->scale[1], curNode->scale[2]);
	}

    const Matrix4 worldXform = xform * node.xform;
	if (curNode->mesh) 
	{
		// Collect tasks for later parallel processing
		MeshInstanceRequest req;
		req.mesh = curNode->mesh;
		req.nodeIndex = curPos;
		req.worldXform = worldXform;
		meshRequests.push_back(req);
	}

	if (curNode->camera) {
		CameraData cam;
		if (curNode->camera->type == cgltf_camera_type_perspective) {
			cam.type = CameraData::kPerspective;
			cam.yfov = curNode->camera->data.perspective.yfov;
			cam.aspectRatio = curNode->camera->data.perspective.aspect_ratio;
			cam.znear = curNode->camera->data.perspective.znear;
			cam.zfar = curNode->camera->data.perspective.zfar;
		}
		cam.matrixIdx = curPos;
		modelData.m_Cameras.push_back(cam);
	}

	uint32_t nextPos = curPos + 1;
	for (size_t i = 0; i < curNode->children_count; ++i) {
		const uint32_t childStartPos = nextPos;
		nextPos = WalkGraph(modelData, data, sceneGraph, modelBSphere, modelBBox, curNode->children[i], nextPos, worldXform, streamCtx, nodeMap, meshCache, meshRequests);
		if (i < curNode->children_count - 1)
			sceneGraph[childStartPos].hasSibling = 1;
	}

	return nextPos;
}

inline void CompileTexture(const std::wstring& basePath, const std::string& fileName, uint8_t flags)
{
    CompileTextureOnDemand(basePath + Utility::UTF8ToWideString(fileName), flags);
}

void BuildMaterials(ModelData& model, const glTF::GltfAsset& asset)
{
	cgltf_data* data = asset.m_Data;

	model.m_TextureNames.resize(data->images_count);

	for (size_t i = 0; i < data->images_count; ++i)
	{
		if (data->images[i].uri)
			model.m_TextureNames[i] = data->images[i].uri;
		else
			model.m_TextureNames[i] = "";
	}

	std::map<std::string, uint8_t> textureOptions;
	model.m_MaterialConstants.resize(data->materials_count);
	model.m_MaterialTextures.resize(data->materials_count);

	for (size_t i = 0; i < data->materials_count; ++i)
	{
		const cgltf_material& srcMat = data->materials[i];
		MaterialConstantData& matConst = model.m_MaterialConstants[i];
		MaterialTextureData& matTex = model.m_MaterialTextures[i];

		// Initialize defaults
		matConst.baseColorFactor[0] = 1.0f;
		matConst.baseColorFactor[1] = 1.0f;
		matConst.baseColorFactor[2] = 1.0f;
		matConst.baseColorFactor[3] = 1.0f;
		matConst.metallicFactor = 1.0f;
		matConst.roughnessFactor = 1.0f;
		matConst.emissiveFactor[0] = 0.0f;
		matConst.emissiveFactor[1] = 0.0f;
		matConst.emissiveFactor[2] = 0.0f;
		matConst.flags = 0;
		matTex.addressModes = 0;

		for (int j = 0; j < kNumTextures; ++j) matTex.stringIdx[j] = 0xFFFF;

		const uint32_t defaultAddressMode = 0x5;
		auto mapGltfSamplerAddressModeToDX12 = [](cgltf_sampler* gltfSampler, glTF::Material::eMaterialTexture mTex)
			{
				uint32_t modeSValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				if (gltfSampler)
				{
					switch (gltfSampler->wrap_s)
					{
					case cgltf_wrap_mode_clamp_to_edge:
						modeSValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
						break;
					case cgltf_wrap_mode_mirrored_repeat:
						modeSValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
						break;
					default:
						modeSValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_WRAP;
						break;
					}
				}

				uint32_t modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				if (gltfSampler)
				{
					switch (gltfSampler->wrap_t)
					{
					case cgltf_wrap_mode_clamp_to_edge:
						modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
						break;
					case cgltf_wrap_mode_mirrored_repeat:
						modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
						break;
					default:
						modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_WRAP;
						break;
					}
				}

				const uint32_t finalValue = (modeTValue << 2 | modeSValue) << ((uint32_t)mTex * 4);
				return finalValue;
			};

		// PBR parameter mapping
		if (srcMat.has_pbr_metallic_roughness)
		{
			const auto& pbr = srcMat.pbr_metallic_roughness;
			memcpy(matConst.baseColorFactor, pbr.base_color_factor, sizeof(float) * 4);
			matConst.metallicFactor = pbr.metallic_factor;
			matConst.roughnessFactor = pbr.roughness_factor;

			if (pbr.base_color_texture.texture) 
			{
				matTex.stringIdx[glTF::Material::kBaseColor] = (uint16_t)cgltf_image_index(data, pbr.base_color_texture.texture->image);
				matConst.baseColorUV = (uint32_t)pbr.base_color_texture.texcoord;
				matTex.addressModes |= mapGltfSamplerAddressModeToDX12(
					pbr.base_color_texture.texture->sampler,
					glTF::Material::kBaseColor);
			}
			else
			{
				matTex.addressModes |= (defaultAddressMode << ((uint32_t)glTF::Material::kBaseColor * 4));
			}

			if (pbr.metallic_roughness_texture.texture) 
			{
				matTex.stringIdx[glTF::Material::kMetallicRoughness] = (uint16_t)cgltf_image_index(data, pbr.metallic_roughness_texture.texture->image);
				matConst.metallicRoughnessUV = (uint32_t)pbr.metallic_roughness_texture.texcoord;
				matTex.addressModes |= mapGltfSamplerAddressModeToDX12(
					pbr.metallic_roughness_texture.texture->sampler,
					glTF::Material::kMetallicRoughness);
			}
			else
			{
				matTex.addressModes |= (defaultAddressMode << ((uint32_t)glTF::Material::kMetallicRoughness * 4));
			}

			if (srcMat.normal_texture.texture) 
			{
				matTex.stringIdx[glTF::Material::kNormal] = (uint16_t)cgltf_image_index(data, srcMat.normal_texture.texture->image);
				matConst.normalTextureScale = srcMat.normal_texture.scale;
				matConst.normalUV = (uint32_t)srcMat.normal_texture.texcoord;
				matTex.addressModes |= mapGltfSamplerAddressModeToDX12(
					srcMat.normal_texture.texture->sampler,
					glTF::Material::kNormal);
			}
			else
			{
				matTex.addressModes |= (defaultAddressMode << ((uint32_t)glTF::Material::kNormal * 4));
			}

			if (srcMat.emissive_texture.texture)
			{
				matTex.stringIdx[glTF::Material::kEmissive] = (uint16_t)cgltf_image_index(data, srcMat.emissive_texture.texture->image);
				memcpy(matConst.emissiveFactor, srcMat.emissive_factor, sizeof(float) * 3);
				matConst.emissiveUV = (uint32_t)srcMat.emissive_texture.texcoord;
				matTex.addressModes |= mapGltfSamplerAddressModeToDX12(
					srcMat.emissive_texture.texture->sampler,
					glTF::Material::kEmissive);
			}
			else
			{
				matTex.addressModes |= (defaultAddressMode << ((uint32_t)glTF::Material::kEmissive * 4));
			}

			if (srcMat.occlusion_texture.texture)
			{
				matTex.stringIdx[glTF::Material::kOcclusion] = (uint16_t)cgltf_image_index(data, srcMat.occlusion_texture.texture->image);
				matConst.occlusionUV = (uint32_t)srcMat.occlusion_texture.texcoord;
				matTex.addressModes |= mapGltfSamplerAddressModeToDX12(
					srcMat.occlusion_texture.texture->sampler,
					glTF::Material::kOcclusion);
			}
			else
			{
				matTex.addressModes |= (defaultAddressMode << ((uint32_t)glTF::Material::kOcclusion * 4));
			}

			if (srcMat.specular.specular_texture.texture)
			{
				matTex.stringIdx[glTF::Material::kSpecular] = (uint16_t)cgltf_image_index(data, srcMat.specular.specular_texture.texture->image);
				matTex.addressModes |= mapGltfSamplerAddressModeToDX12(
					srcMat.specular.specular_texture.texture->sampler,
					glTF::Material::kSpecular);
			}
			else
			{
				matTex.addressModes |= (defaultAddressMode << ((uint32_t)glTF::Material::kSpecular * 4));
			}

			if (srcMat.specular.specular_color_texture.texture)
			{
				matTex.stringIdx[glTF::Material::kSpecularColor] = (uint16_t)cgltf_image_index(data, srcMat.specular.specular_color_texture.texture->image);
				matTex.addressModes |= mapGltfSamplerAddressModeToDX12(
					srcMat.specular.specular_color_texture.texture->sampler,
					glTF::Material::kSpecularColor);
			}
			else
			{
				matTex.addressModes |= (defaultAddressMode << ((uint32_t)glTF::Material::kSpecularColor * 4));
			}

			if (srcMat.double_sided) matConst.twoSided = 1;
			if (srcMat.alpha_mode == cgltf_alpha_mode_blend) matConst.alphaBlend = 1;
			if (srcMat.alpha_mode == cgltf_alpha_mode_mask) {
				matConst.alphaTest = 1;
				matConst.alphaRef = F32ToF16(srcMat.alpha_cutoff);
			}

			auto RegisterOpt = [&](int slot, bool srgb) {
				if (matTex.stringIdx[slot] != 0xFFFF)
					textureOptions[model.m_TextureNames[matTex.stringIdx[slot]]] = TextureOptions(srgb, (srcMat.alpha_mode != cgltf_alpha_mode_opaque));
				};
			RegisterOpt(glTF::Material::kBaseColor, true);
			RegisterOpt(glTF::Material::kMetallicRoughness, false);
			RegisterOpt(glTF::Material::kEmissive, true);
			RegisterOpt(glTF::Material::kNormal, false);
			RegisterOpt(glTF::Material::kOcclusion, false);
			RegisterOpt(glTF::Material::kSpecular, false);
			RegisterOpt(glTF::Material::kSpecularColor, true);
		}
	}

	for (size_t i = 0; i < model.m_TextureNames.size(); ++i)
	{
		auto it = textureOptions.find(model.m_TextureNames[i]);
		if (it != textureOptions.end())
		{
			model.m_TextureOptions.push_back(it->second);
			CompileTextureOnDemand(asset.m_BasePath + Utility::UTF8ToWideString(it->first), it->second);
		}
		else
		{
			model.m_TextureOptions.push_back(0xFF);
		}
	}
}

void BuildAnimations(ModelData& model, const glTF::GltfAsset& asset, const NodeMap& nodeMap)
{
	cgltf_data* data = asset.m_Data;
	if (data->animations_count == 0) return;

    model.m_Animations.resize(data->animations_count);

    for (size_t i = 0; i < data->animations_count; ++i)
    {
		const cgltf_animation& srcAnim = data->animations[i];
		AnimationSet& animSet = model.m_Animations[i];
		animSet.duration = 0.0f;
		animSet.firstCurve = (uint32_t)model.m_AnimationCurves.size();
		animSet.numCurves = 0;

        for (size_t j = 0; j < srcAnim.channels_count; ++j)
        {
			const cgltf_animation_channel& channel = srcAnim.channels[j];
			const cgltf_animation_sampler& sampler = *channel.sampler;

			AnimationCurve curve;
			auto it = nodeMap.find(channel.target_node);
			if (it != nodeMap.end())
			{
				curve.targetNode = it->second;
			}
			else
			{
				continue;
			}

			switch (channel.target_path) 
            {
			case cgltf_animation_path_type_translation: curve.targetPath = glTF::AnimChannel::kTranslation; break;
			case cgltf_animation_path_type_rotation:    curve.targetPath = glTF::AnimChannel::kRotation; break;
			case cgltf_animation_path_type_scale:       curve.targetPath = glTF::AnimChannel::kScale; break;
			case cgltf_animation_path_type_weights:     curve.targetPath = glTF::AnimChannel::kWeights; break;
			default: continue;
			}

			// Interpolation mapping
			switch (sampler.interpolation) 
            {
			case cgltf_interpolation_type_linear:       curve.interpolation = glTF::AnimSampler::kLinear; break;
			case cgltf_interpolation_type_step:         curve.interpolation = glTF::AnimSampler::kStep; break;
			case cgltf_interpolation_type_cubic_spline: curve.interpolation = glTF::AnimSampler::kCubicSpline; break;
			default: curve.interpolation = glTF::AnimSampler::kLinear; break;
			}

			// Keyframe data offset and format
			curve.keyFrameOffset = (uint32_t)model.m_AnimationKeyFrameData.size();
			curve.keyFrameFormat = glTF::GltfAsset::MapComponentType(sampler.output->component_type);
			curve.numSegments = (float)(sampler.input->count - 1);

			// Compute stride (float count)
			uint32_t numComps = (uint32_t)cgltf_num_components(sampler.output->type);
			curve.keyFrameStride = numComps; // Assume float; adjust if short integers

			// Extract timestamps to compute duration
			std::vector<float> times(sampler.input->count);
			cgltf_accessor_unpack_floats(sampler.input, times.data(), times.size());
			curve.startTime = times.front();
			float endTime = times.back();
			if (endTime - curve.startTime < 1e-6f)
				curve.rangeScale = 0.0f;
			else
				curve.rangeScale = curve.numSegments / (endTime - curve.startTime);
			animSet.duration = std::max(animSet.duration, endTime);

			// Extract keyframe data
			size_t dataSize = sampler.output->count * cgltf_calc_size(sampler.output->type, sampler.output->component_type);
			const uint8_t* srcData = (const uint8_t*)sampler.output->buffer_view->buffer->data + sampler.output->offset + sampler.output->buffer_view->offset;

			size_t currentByteSize = model.m_AnimationKeyFrameData.size();
			model.m_AnimationKeyFrameData.resize(currentByteSize + dataSize);
			memcpy(model.m_AnimationKeyFrameData.data() + currentByteSize, srcData, dataSize);

			model.m_AnimationCurves.push_back(curve);
			animSet.numCurves++;
        }
    }
}

void BuildSkins(ModelData& model, const glTF::GltfAsset& asset, const NodeMap& nodeMap)
{
	cgltf_data* data = asset.m_Data;
	if (data->skins_count == 0)
		return;

	std::vector<std::pair<uint16_t, uint16_t>> skinMap;
	skinMap.reserve(data->skins_count);

	for (size_t i = 0; i < data->skins_count; ++i)
	{
		const cgltf_skin& skin = data->skins[i];
		ASSERT(skin.joints_count == skin.inverse_bind_matrices->count);
        // Record offset and joint count
        uint16_t numJoints = (uint16_t)skin.joints_count;
        uint16_t curOffset = (uint16_t)model.m_JointIndices.size();
        skinMap.push_back(std::make_pair(curOffset, numJoints));

        // Append remapped joint indices
		for (size_t j = 0; j < skin.joints_count; ++j)
		{
			cgltf_node* joint = skin.joints[j];
			auto it = nodeMap.find(joint);
			if (it != nodeMap.end())
			{
				model.m_JointIndices.push_back((uint16_t)it->second);
			}
			else
			{
				model.m_JointIndices.push_back(0);
				Utility::Printf("Error: Skin joint not found in scene graph.\n");
			}
		}

		if (skin.inverse_bind_matrices) 
		{
			size_t count = skin.inverse_bind_matrices->count;
			std::vector<float> buffer(count * 16);
			cgltf_accessor_unpack_floats(skin.inverse_bind_matrices, buffer.data(), buffer.size());

			Matrix4* matBuffer = (Matrix4*)buffer.data();
			for (uint16_t k = 0; k < numJoints; ++k)
			{
				if (k < count)
					model.m_JointIBMs.push_back(matBuffer[k]);
				else
					model.m_JointIBMs.push_back(Matrix4(kIdentity));
			}
		}
		else 
		{
			model.m_JointIBMs.insert(model.m_JointIBMs.end(), numJoints, Matrix4(kIdentity));
		}
    }

    // Assign skinned meshes the proper joint offset and count
    //for (Mesh* mesh : model.m_Meshes)
    //{
    //    if (mesh->numJoints != 0)
    //    {
    //        std::pair<uint16_t, uint16_t> offsetAndCount = skinMap[mesh->startJoint];
    //        mesh->startJoint = offsetAndCount.first;
    //        mesh->numJoints = offsetAndCount.second;
    //    }
    //}
}

bool Renderer::BuildModel(ModelData& model, const glTF::GltfAsset& asset, GlobalStreamingContext& streamCtx, int sceneIdx)
{
    BuildMaterials(model, asset);

	cgltf_data* data = asset.m_Data;

    // Generate scene graph and meshes
	model.m_SceneGraph.resize(data->nodes_count);

	NodeMap nodeMap;
	MeshCache meshCache;

	const cgltf_scene* scene = sceneIdx < 0 ? data->scene : &data->scenes[sceneIdx];
	if (scene == nullptr)
		return false;

    std::vector<unsigned char> bufferMemory;

    model.m_BoundingSphere = BoundingSphere(kZero);
    model.m_BoundingBox = AxisAlignedBox(kZero);

	std::vector<MeshInstanceRequest> meshRequests;
	meshRequests.reserve(data->nodes_count); // Reserve capacity estimate

	// Traverse scene root nodes
	uint32_t currentPos = 0;
	if (scene->nodes_count > 0)
	{
		for (size_t i = 0; i < scene->nodes_count; ++i)
		{
			const uint32_t rootStartPos = currentPos;
			uint32_t nextPos = WalkGraph(model, data, model.m_SceneGraph, model.m_BoundingSphere, model.m_BoundingBox, scene->nodes[i], currentPos, Matrix4(kIdentity), streamCtx, nodeMap, meshCache, meshRequests);
			if (i < scene->nodes_count - 1)
				model.m_SceneGraph[rootStartPos].hasSibling = 1;

			currentPos = nextPos;
		}
	}

	model.m_SceneGraph.resize(currentPos);

	// Compile geometry in parallel (heavy lifting)
	if (!meshRequests.empty())
	{
		ParallelCompileMeshes(model, meshCache, data, meshRequests, streamCtx);
	}
	Utility::Printf("Total triangles built: %llu\n", model.m_TriangleCount);

	// Instantiate and compute bounds
	if (!meshRequests.empty())
	{
		BoundingSphere modelBSphere(kZero);
		AxisAlignedBox modelBBox(kZero);

		for (const auto& req : meshRequests)
		{
			auto it = meshCache.find(req.mesh);
			if (it == meshCache.end()) continue; // Should not happen

			const CompiledMeshData& cachedMesh = it->second;

			InstantiateMesh(model, cachedMesh, req.nodeIndex);

			BoundingSphere lsSphere = cachedMesh.boundsLS;
			Vector3 worldCenter = Vector3(req.worldXform * lsSphere.GetCenter());

			Vector3 xAxis = Vector3(req.worldXform.GetX().GetX(), req.worldXform.GetX().GetY(), req.worldXform.GetX().GetZ());
			Vector3 yAxis = Vector3(req.worldXform.GetY().GetX(), req.worldXform.GetY().GetY(), req.worldXform.GetY().GetZ());
			Vector3 zAxis = Vector3(req.worldXform.GetZ().GetX(), req.worldXform.GetZ().GetY(), req.worldXform.GetZ().GetZ());
			float maxScale = std::max(std::max((float)Length(xAxis), (float)Length(yAxis)), (float)Length(zAxis));

			BoundingSphere worldSphere(worldCenter, lsSphere.GetRadius() * maxScale);
			modelBSphere = modelBSphere.Union(worldSphere);

			AxisAlignedBox lsBox = cachedMesh.bboxLS;
			Vector3 minP = lsBox.GetMin();
			Vector3 maxP = lsBox.GetMax();
			Vector3 corners[8] = {
				{minP.GetX(), minP.GetY(), minP.GetZ()}, {minP.GetX(), minP.GetY(), maxP.GetZ()},
				{minP.GetX(), maxP.GetY(), minP.GetZ()}, {minP.GetX(), maxP.GetY(), maxP.GetZ()},
				{maxP.GetX(), minP.GetY(), minP.GetZ()}, {maxP.GetX(), minP.GetY(), maxP.GetZ()},
				{maxP.GetX(), maxP.GetY(), minP.GetZ()}, {maxP.GetX(), maxP.GetY(), maxP.GetZ()}
			};
			for (int k = 0; k < 8; ++k) modelBBox.AddPoint(Vector3(req.worldXform * corners[k]));
		}

		model.m_BoundingSphere = modelBSphere;
		model.m_BoundingBox = modelBBox;
	}
    BuildAnimations(model, asset, nodeMap);
    BuildSkins(model, asset, nodeMap);

    return true;
}

bool Renderer::SaveModel(const std::wstring& filePath, const ModelData& data, GlobalStreamingContext& streamCtx)
{
    FileHeader header;
    std::memcpy(header.id, "MINI", 4);
    header.version = CURRENT_MINI_FILE_VERSION;
	auto CleanupTempFiles = [&]()
		{
			if (streamCtx.TempFilesToClean.empty()) return;
			Utility::Printf("Deleting temp files...\n");
			#pragma omp parallel for schedule(dynamic)
			for (int i = 0; i < (int)streamCtx.TempFilesToClean.size(); ++i)
			{
				_wremove(streamCtx.TempFilesToClean[i].c_str());
			}
			streamCtx.TempFilesToClean.clear();
		};

	auto CheckedU32 = [&](size_t value, const char* label, uint32_t& out) -> bool
		{
			if (value > std::numeric_limits<uint32_t>::max())
			{
				Utility::Printf("Error: %s count overflow.\n", label);
				return false;
			}
			out = static_cast<uint32_t>(value);
			return true;
		};

	if (!CheckedU32(data.m_SceneGraph.size(), "Scene graph node", header.numNodes)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_Meshes.size(), "Mesh", header.numMeshes)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_MaterialConstants.size(), "Material", header.numMaterials)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_TextureNames.size(), "Texture", header.numTextures)) { CleanupTempFiles(); return false; }

	// Cluster data
	if (!CheckedU32(data.m_GroupInfos.size(), "Group", header.groupCount)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_Nodes.size(), "Hierarchy node", header.hierarchyNodeCount)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_Pages.size(), "Page", header.pageCount)) { CleanupTempFiles(); return false; }

	uint64_t stringTableSize = 0;
	for (const std::string& str : data.m_TextureNames)
	{
		const uint64_t add = static_cast<uint64_t>(str.size()) + 1u;
		if (stringTableSize > std::numeric_limits<uint32_t>::max() - add)
		{
			Utility::Printf("Error: Texture string table size overflow.\n");
			CleanupTempFiles();
			return false;
		}
		stringTableSize += add;
	}
	header.stringTableSize = static_cast<uint32_t>(stringTableSize);

	if (!CheckedU32(data.m_AnimationCurves.size(), "Animation curve", header.numAnimationCurves)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_Animations.size(), "Animation", header.numAnimations)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_JointIndices.size(), "Joint", header.numJoints)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_Cameras.size(), "Camera", header.numCameras)) { CleanupTempFiles(); return false; }
	if (!CheckedU32(data.m_AnimationKeyFrameData.size(), "Animation key frame data", header.keyFrameDataSize)) { CleanupTempFiles(); return false; }

    header.boundingSphere[0] = data.m_BoundingSphere.GetCenter().GetX();
    header.boundingSphere[1] = data.m_BoundingSphere.GetCenter().GetY();
    header.boundingSphere[2] = data.m_BoundingSphere.GetCenter().GetZ();
    header.boundingSphere[3] = data.m_BoundingSphere.GetRadius();
    header.minPos[0] = data.m_BoundingBox.GetMin().GetX();
    header.minPos[1] = data.m_BoundingBox.GetMin().GetY();
    header.minPos[2] = data.m_BoundingBox.GetMin().GetZ();
    header.maxPos[0] = data.m_BoundingBox.GetMax().GetX();
    header.maxPos[1] = data.m_BoundingBox.GetMax().GetY();
    header.maxPos[2] = data.m_BoundingBox.GetMax().GetZ();

	// -------------------------------------------------------------
	// Compute total size of header and metadata
	// -------------------------------------------------------------
	uint64_t metadataSize = sizeof(FileHeader);
	auto AddSize = [&](uint64_t add, const char* label) -> bool
		{
			if (add > std::numeric_limits<uint64_t>::max() - metadataSize)
			{
				Utility::Printf("Error: %s size overflow.\n", label);
				return false;
			}
			metadataSize += add;
			return true;
		};

	if (!AddSize(static_cast<uint64_t>(header.numNodes) * sizeof(GraphNode), "Scene graph")) { CleanupTempFiles(); return false; }
	for (const Mesh* mesh : data.m_Meshes)
	{
		if (!mesh || mesh->numDraws == 0)
		{
			Utility::Printf("Error: Mesh with zero draws encountered during save.\n");
			CleanupTempFiles();
			return false;
		}
		const uint64_t meshSize = sizeof(Mesh) + static_cast<uint64_t>(mesh->numDraws - 1) * sizeof(Mesh::Draw);
		if (!AddSize(meshSize, "Mesh data")) { CleanupTempFiles(); return false; }
	}
	if (!AddSize(static_cast<uint64_t>(header.numMaterials) * sizeof(MaterialConstantData), "Material constants")) { CleanupTempFiles(); return false; }
	if (!AddSize(static_cast<uint64_t>(header.numMaterials) * sizeof(MaterialTextureData), "Material textures")) { CleanupTempFiles(); return false; }
	if (!AddSize(header.stringTableSize, "Texture names")) { CleanupTempFiles(); return false; }
	if (!AddSize(static_cast<uint64_t>(header.numTextures) * sizeof(uint8_t), "Texture options")) { CleanupTempFiles(); return false; }
	if (header.numAnimations > 0) {
		if (!AddSize(header.keyFrameDataSize, "Animation keyframes")) { CleanupTempFiles(); return false; }
		if (!AddSize(static_cast<uint64_t>(header.numAnimationCurves) * sizeof(AnimationCurve), "Animation curves")) { CleanupTempFiles(); return false; }
		if (!AddSize(static_cast<uint64_t>(header.numAnimations) * sizeof(AnimationSet), "Animation sets")) { CleanupTempFiles(); return false; }
	}
	if (header.numJoints) {
		if (!AddSize(static_cast<uint64_t>(header.numJoints) * sizeof(uint16_t), "Joint indices")) { CleanupTempFiles(); return false; }
		if (!AddSize(static_cast<uint64_t>(header.numJoints) * sizeof(Matrix4), "Joint matrices")) { CleanupTempFiles(); return false; }
	}
	if (header.numCameras) {
		if (!AddSize(static_cast<uint64_t>(header.numCameras) * sizeof(CameraData), "Cameras")) { CleanupTempFiles(); return false; }
	}
	if (header.groupCount > 0 && !AddSize(static_cast<uint64_t>(header.groupCount) * sizeof(GroupMetadata), "Group metadata")) { CleanupTempFiles(); return false; }
	if (header.hierarchyNodeCount > 0 && !AddSize(static_cast<uint64_t>(header.hierarchyNodeCount) * sizeof(HierarchyNode), "Hierarchy nodes")) { CleanupTempFiles(); return false; }
	if (header.pageCount > 0 && !AddSize(static_cast<uint64_t>(header.pageCount) * sizeof(PageMetadata), "Page metadata")) { CleanupTempFiles(); return false; }
	if (header.pageCount > 0 && !AddSize(static_cast<uint64_t>(header.pageCount) * sizeof(PageCompressionInfo), "Page compression info")) { CleanupTempFiles(); return false; }

	// Compute geometry blob offset
	const uint32_t pageSize = Renderer::kPageSizeInBytes;
	const uint64_t geometryBlobOffset = metadataSize;
	if (streamCtx.TotalGeometrySize > std::numeric_limits<uint64_t>::max() - (pageSize - 1))
	{
		Utility::Printf("Error: Geometry size alignment overflow.\n");
		CleanupTempFiles();
		return false;
	}
	const uint64_t uncompressedSize = Math::AlignUp(streamCtx.TotalGeometrySize, (size_t)pageSize);
	if (streamCtx.TotalGeometrySize > 0 && header.pageCount == 0)
	{
		Utility::Printf("Error: Geometry data exists but page count is zero.\n");
		CleanupTempFiles();
		return false;
	}

	header.geometryBlobOffset = geometryBlobOffset;
	header.geometryBlobSize = 0;
	header.geometryUncompressedSize = uncompressedSize;
	header.geometryCompressionType = Renderer::kGeometryCompressionLZ4;

	std::vector<PageCompressionInfo> pageCompressionInfos;
	if (header.pageCount > 0)
		pageCompressionInfos.resize(header.pageCount);

	// -------------------------------------------------------------
	// Write header + metadata (reserve compression table)
	// -------------------------------------------------------------
	uint64_t maxCompressedSize = 0;
	if (header.pageCount > 0)
	{
		const uint64_t maxPerPage = static_cast<uint64_t>(LZ4_compressBound((int)pageSize));
		if (maxPerPage == 0 || header.pageCount > (std::numeric_limits<uint64_t>::max() / maxPerPage))
		{
			Utility::Printf("Error: Compressed geometry size overflow.\n");
			CleanupTempFiles();
			return false;
		}
		maxCompressedSize = maxPerPage * header.pageCount;
	}

	HANDLE hFile = INVALID_HANDLE_VALUE;
	HANDLE hMapping = NULL;
	uint8_t* pMappedData = nullptr;

	auto CleanupFileHandles = [&]()
		{
			if (pMappedData) UnmapViewOfFile(pMappedData);
			if (hMapping) CloseHandle(hMapping);
			if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
			pMappedData = nullptr;
			hMapping = NULL;
			hFile = INVALID_HANDLE_VALUE;
		};

	hFile = CreateFileW(filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) { CleanupTempFiles(); return false; }

	// Pre-extend file size
	if (geometryBlobOffset > std::numeric_limits<uint64_t>::max() - maxCompressedSize)
	{
		Utility::Printf("Error: Final file size overflow.\n");
		CleanupFileHandles();
		CleanupTempFiles();
		return false;
	}
	const uint64_t finalFileSize = geometryBlobOffset + maxCompressedSize; // allocate worst-case size
	LARGE_INTEGER liSize;
	liSize.QuadPart = finalFileSize;
	if (!SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN) || !SetEndOfFile(hFile))
	{
		Utility::Printf("Error: Failed to resize output file.\n");
		CleanupFileHandles();
		CleanupTempFiles();
		return false;
	}

	hMapping = CreateFileMappingW(hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
	if (!hMapping) {
		CleanupFileHandles();
		CleanupTempFiles();
		return false;
	}

	pMappedData = (uint8_t*)MapViewOfFile(hMapping, FILE_MAP_WRITE, 0, 0, 0);
	if (!pMappedData) {
		CleanupFileHandles();
		CleanupTempFiles();
		return false;
	}

	// Write header and metadata
	// -------------------------------------------------------------
	uint8_t* pCursor = pMappedData;
	auto Write = [&](const void* src, size_t size) 
		{
			std::memcpy(pCursor, src, size);
			pCursor += size;
		};

	auto WriteZeros = [&](uint64_t size) {
		constexpr size_t kZeroChunk = 64 * 1024;
		static const std::vector<char> kZeros(kZeroChunk, 0);
		while (size > 0)
		{
			const size_t chunk = (size_t)std::min<uint64_t>(size, kZeros.size());
			Write(kZeros.data(), chunk);
			size -= chunk;
		}
		};

	Write(&header, sizeof(FileHeader));
	Write(data.m_SceneGraph.data(), header.numNodes * sizeof(GraphNode));
	for (const Mesh* mesh : data.m_Meshes) {
		const size_t meshSize = sizeof(Mesh) + sizeof(Mesh::Draw) * (mesh->numDraws - 1);
		Write(mesh, meshSize);
	}
	Write(data.m_MaterialConstants.data(), header.numMaterials * sizeof(MaterialConstantData));
	Write(data.m_MaterialTextures.data(), header.numMaterials * sizeof(MaterialTextureData));
	for (uint32_t i = 0; i < header.numTextures; ++i) {
		const std::string& str = data.m_TextureNames[i];
		Write(str.c_str(), str.size() + 1);
	}
	Write(data.m_TextureOptions.data(), header.numTextures * sizeof(uint8_t));

	if (header.numAnimations > 0) {
		Write(data.m_AnimationKeyFrameData.data(), header.keyFrameDataSize);
		Write(data.m_AnimationCurves.data(), header.numAnimationCurves * sizeof(AnimationCurve));
		Write(data.m_Animations.data(), header.numAnimations * sizeof(AnimationSet));
	}
	if (header.numJoints) {
		Write(data.m_JointIndices.data(), header.numJoints * sizeof(uint16_t));
		Write(data.m_JointIBMs.data(), header.numJoints * sizeof(Matrix4));
	}
	if (header.numCameras) {
		Write(data.m_Cameras.data(), header.numCameras * sizeof(CameraData));
	}
	if (header.groupCount > 0) Write(data.m_GroupInfos.data(), header.groupCount * sizeof(GroupMetadata));
	if (header.hierarchyNodeCount > 0) Write(data.m_Nodes.data(), header.hierarchyNodeCount * sizeof(HierarchyNode));
	if (header.pageCount > 0) Write(data.m_Pages.data(), header.pageCount * sizeof(PageMetadata));

	uint64_t pageCompressionInfoOffset = 0;
	if (header.pageCount > 0)
	{
		pageCompressionInfoOffset = (uint64_t)(pCursor - pMappedData);
		WriteZeros(header.pageCount * sizeof(PageCompressionInfo));
	}

	// Verify metadata size matches expected blob offset
	const uint64_t currentOffset = (uint64_t)(pCursor - pMappedData);
	if (currentOffset != geometryBlobOffset)
	{
		Utility::Printf("Error: Metadata size mismatch.\n");
		CleanupFileHandles();
		CleanupTempFiles();
		return false;
	}

	// -------------------------------------------------------------
	// Compress geometry pages in parallel (low memory, write directly to disk)
	// -------------------------------------------------------------
	std::atomic<uint64_t> compressedWriteOffset{ 0 };
	if (streamCtx.TotalGeometrySize > 0)
	{
		std::atomic<bool> hadError{ false };
		uint8_t* pBlobBase = pMappedData + geometryBlobOffset;

		struct PageJobSpan
		{
			uint32_t JobIndex;
			uint32_t OffsetInPage;
		};

		const uint32_t pageCount = header.pageCount;
		std::vector<std::vector<PageJobSpan>> pageJobs(pageCount);

		uint64_t runningOffset = 0;
		bool jobMapError = false;
		for (uint32_t jobIndex = 0; jobIndex < streamCtx.PendingWrites.size(); ++jobIndex)
		{
			const auto& job = streamCtx.PendingWrites[jobIndex];
			const uint32_t pageIdx = static_cast<uint32_t>(runningOffset / pageSize);
			const uint32_t offsetInPage = static_cast<uint32_t>(runningOffset % pageSize);
			if (pageIdx < pageCount && offsetInPage + job.SizeBytes <= pageSize)
			{
				pageJobs[pageIdx].push_back({ jobIndex, offsetInPage });
			}
			else
			{
				Utility::Printf("Error: Pending write %u exceeds page bounds.\n", jobIndex);
				jobMapError = true;
				break;
			}
			runningOffset += job.SizeBytes;
		}
		if (jobMapError || runningOffset != streamCtx.TotalGeometrySize)
		{
			Utility::Printf("Error: Pending write size mismatch.\n");
			CleanupFileHandles();
			CleanupTempFiles();
			return false;
		}

		std::atomic<uint32_t> completedPages = 0;
		const uint32_t reportStep = std::max(1u, pageCount / 20);

		Utility::Printf("Compressing geometry pages (Total: %llu MB)...\n", streamCtx.TotalGeometrySize / (1024 * 1024));
		#pragma omp parallel for schedule(dynamic)
		for (int pageIdx = 0; pageIdx < (int)pageCount; ++pageIdx)
		{
			if (hadError.load(std::memory_order_relaxed)) continue;
			std::vector<uint8_t> pageBuffer(pageSize, 0);

			bool pageError = false;
			for (const auto& span : pageJobs[pageIdx])
			{
				const auto& job = streamCtx.PendingWrites[span.JobIndex];
				uint8_t* pDest = pageBuffer.data() + span.OffsetInPage;

				if (job.TempSourcePath.empty())
				{
					std::memset(pDest, 0, job.SizeBytes);
				}
				else
				{
					std::ifstream localStream(job.TempSourcePath, std::ios::in | std::ios::binary);
					if (!localStream) 
					{ 
						Utility::Printf("Error: Failed to open temp file %s for reading.\n", Utility::WideStringToUTF8(job.TempSourcePath).c_str());
						pageError = true;
						break; 
					}

					localStream.seekg(job.SourceOffset);
					localStream.read(reinterpret_cast<char*>(pDest), job.SizeBytes);
					if (!localStream) 
					{ 
						Utility::Printf("Error: Failed to read data from temp file %s.\n", Utility::WideStringToUTF8(job.TempSourcePath).c_str());
						pageError = true;
						break; 
					}

					if (job.BaseGroupIndexPatchValue > 0)
					{
						auto* gHeader = reinterpret_cast<GroupHeader*>(pDest);
						auto* mHeaders = reinterpret_cast<MeshletHeader*>(pDest + sizeof(GroupHeader));
						for (uint32_t m = 0; m < gHeader->MeshletCount; ++m)
						{
							if (mHeaders[m].RefineGroupIndex != 0xFFFFFFFF)
							{
								mHeaders[m].RefineGroupIndex += job.BaseGroupIndexPatchValue;
							}
						}
					}
				}
			}

			if (pageError)
			{
				hadError.store(true, std::memory_order_relaxed);
				continue;
			}

			std::vector<char> compressed((size_t)LZ4_compressBound((int)pageSize));
			const int compressedSize = LZ4_compress_default(
				reinterpret_cast<const char*>(pageBuffer.data()),
				compressed.data(),
				(int)pageSize,
				(int)compressed.size());
			if (compressedSize <= 0)
			{
				Utility::Printf("Error: LZ4 compression failed on page %d.\n", pageIdx);
				hadError.store(true, std::memory_order_relaxed);
				continue;
			}

			const uint64_t pageOffset = compressedWriteOffset.fetch_add((uint64_t)compressedSize);
			if (pageOffset + (uint64_t)compressedSize > maxCompressedSize)
			{
				Utility::Printf("Error: Compressed data exceeds reserved size on page %d.\n", pageIdx);
				hadError.store(true, std::memory_order_relaxed);
				continue;
			}
			pageCompressionInfos[pageIdx].CompressedOffset = pageOffset;
			pageCompressionInfos[pageIdx].CompressedSize = (uint32_t)compressedSize;

			std::memcpy(pBlobBase + pageOffset, compressed.data(), compressedSize);

			const uint32_t done = ++completedPages;
			if ((done % reportStep) == 0 || done == pageCount)
			{
				const float percent = (float)done / (float)pageCount * 100.0f;
				Utility::Printf("Compressing Pages: %u/%u (%.1f%%)\n", done, pageCount, percent);
			}
		}

		if (hadError.load(std::memory_order_relaxed))
		{
			Utility::Printf("Error: Geometry compression failed. Aborting save.\n");
			CleanupFileHandles();
			CleanupTempFiles();
			return false;
		}
	}

	header.geometryBlobSize = compressedWriteOffset.load();

	Utility::Printf("Total compressed geometry size: %llu MB\n", compressedWriteOffset.load() / (1024 * 1024));

	// Write back header and compression table
	pCursor = pMappedData;
	Write(&header, sizeof(FileHeader));
	if (header.pageCount > 0)
	{
		pCursor = pMappedData + pageCompressionInfoOffset;
		Write(pageCompressionInfos.data(), header.pageCount * sizeof(PageCompressionInfo));
	}

	if (!FlushViewOfFile(pMappedData, 0))
	{
		Utility::Printf("Failed to flush view of file: %d\n", GetLastError());
	}

	UnmapViewOfFile(pMappedData);
	pMappedData = nullptr;
	CloseHandle(hMapping);
	hMapping = NULL;

	LARGE_INTEGER finalliSize;
	finalliSize.QuadPart = geometryBlobOffset + compressedWriteOffset.load();
	if (!SetFilePointerEx(hFile, finalliSize, NULL, FILE_BEGIN)) {
		Utility::Printf("SetFilePointerEx failed: %lu\n", GetLastError());
	}
	if (!SetEndOfFile(hFile)) {
		Utility::Printf("SetEndOfFile failed: %lu\n", GetLastError());
	}

	FlushFileBuffers(hFile);

	CleanupFileHandles();

	// Clean up temporary files
	CleanupTempFiles();

	Utility::Printf("Build geometry result:\n");
	Utility::Printf("Group count: %d\n", header.groupCount);
	Utility::Printf("BVH node count: %d\n", header.hierarchyNodeCount);
	Utility::Printf("Page count: %d\n", header.pageCount);

    return true;
}
