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
#include "MeshOptimizer/MeshOptimizer.h"
#include "metis.h"

#include <fstream>
#include <map>
#include <unordered_map>

using namespace DirectX;
using namespace Math;
using namespace Renderer;
using namespace Graphics;

#include "MeshletBuilder.h"

void Renderer::CompileMesh(
	ModelData& modelData,
	const cgltf_data* data,
	const cgltf_mesh& srcMesh,
	uint32_t matrixIdx,
	const Matrix4& localToObject,
	Math::BoundingSphere& boundingSphere,
	Math::AxisAlignedBox& boundingBox,
    GlobalStreamingContext& streamCtx
    )
{
    // We still have a lot of work to do.  Now that we know about all of the primitives in this mesh
    // and have standardized their vertex buffer streams, we must set out to identify which primitives
    // have the same vertex format and material.  These can share a PSO and Vertex/Index buffer views.
    // There may be more than one draw call per group due to 16-bit indices.

    size_t totalVertexSize = 0;
    size_t totalDepthVertexSize = 0;
    size_t totalIndexSize = 0;

    BoundingSphere sphereOS(kZero);
    AxisAlignedBox bboxOS(kZero);

    std::vector<Primitive> primitives(srcMesh.primitives_count);
    for (uint32_t i = 0; i < primitives.size(); ++i)
    {
		OptimizeMesh(primitives[i], data, srcMesh.primitives[i], localToObject);
        sphereOS = sphereOS.Union(primitives[i].m_BoundsOS);
        bboxOS.AddBoundingBox(primitives[i].m_BBoxOS);
    }

    boundingSphere = sphereOS;
    boundingBox = bboxOS;

    std::map<uint32_t, std::vector<Primitive*>> renderMeshes;
    for (auto& prim : primitives)
    {
        uint32_t hash = prim.hash;
        renderMeshes[hash].push_back(&prim);
        totalVertexSize += prim.VB->size();
        totalDepthVertexSize += prim.DepthVB->size();
        totalIndexSize += Math::AlignUp(prim.IB->size(), 4);
    }

    uint32_t baseGroupIndex = static_cast<uint32_t>(modelData.m_GroupInfos.size());
    uint32_t baseNodeIndex = static_cast<uint32_t>(modelData.m_Nodes.size());
	MeshletBuildArgs buildArgs = {};
	buildArgs.meshBufferIndex = static_cast<uint16_t>(matrixIdx);
	ASSERT(matrixIdx <= 0xFFFF, "Too many scene graph nodes for meshlet system.");

    for (auto& iter : renderMeshes)
    {
        buildArgs.materialBufferIndex = iter.second[0]->materialIdx;
        buildArgs.psoFlags = iter.second[0]->psoFlags;

		size_t numDraws = iter.second.size();
		Mesh* mesh = (Mesh*)malloc(sizeof(Mesh) + sizeof(Mesh::Draw) * (numDraws - 1));
        mesh->numDraws = (uint32_t)numDraws;
		mesh->matrixIdx = static_cast<uint16_t>(matrixIdx);

        for (size_t primitiveIndex = 0; primitiveIndex < iter.second.size(); primitiveIndex++)
        {
            Primitive* draw = iter.second[primitiveIndex];
			buildArgs.vertexStride = draw->vertexStride;
			const uint32_t vertexCount = static_cast<uint32_t>(draw->VB->size() / draw->vertexStride);
			buildArgs.VBData = draw->VB->data();
			buildArgs.IBData = draw->IB->data();
			buildArgs.vertexCount = vertexCount;
			buildArgs.indexCount = draw->primCount;
			buildArgs.baseGroupIndex = baseGroupIndex;
			buildArgs.baseNodeIndex = baseNodeIndex;
#if defined(_DEBUG)
			buildArgs.settings.bOutputDebugInfo = true;
            Utility::Printf("Start build meshlets of primitive with %llu triangles", buildArgs.indexCount / 3);
#endif
            // 构建
            const auto buildResult = MeshletBuilder::Build(buildArgs);
            // 记录根节点
			mesh->draw[primitiveIndex].rootNodeIndex = baseNodeIndex;

			mesh->draw[primitiveIndex].boundingSphere[0] = draw->m_BoundsLS.GetCenter().GetX();
			mesh->draw[primitiveIndex].boundingSphere[1] = draw->m_BoundsLS.GetCenter().GetY();
			mesh->draw[primitiveIndex].boundingSphere[2] = draw->m_BoundsLS.GetCenter().GetZ();
			mesh->draw[primitiveIndex].boundingSphere[3] = draw->m_BoundsLS.GetRadius();

            for (uint32_t buildGroupIndex = 0; buildGroupIndex < buildResult.Groups.size(); ++buildGroupIndex)
            {
				const auto& group = buildResult.Groups[buildGroupIndex];
				const uint32_t groupSize = static_cast<uint32_t>(group.Blob.size());
				ASSERT(groupSize % 4 == 0, "Group blob must be 4-byte aligned");
				ASSERT(groupSize <= Renderer::kPageSizeInBytes, "Single group exceeds page size limit.");

				if (streamCtx.CurrentOffsetInPage + groupSize > Renderer::kPageSizeInBytes)
				{
					// 空间不足，执行 Page 填充（Padding）
					uint32_t paddingSize = Renderer::kPageSizeInBytes - streamCtx.CurrentOffsetInPage;
					if (paddingSize > 0 && paddingSize < Renderer::kPageSizeInBytes)
					{
						streamCtx.TempGeoFile.write(streamCtx.ZeroBuffer.data(), paddingSize);
						streamCtx.TotalGeometrySize += paddingSize;
					}

					// 开启新 Page
					PageMetadata page;
					page.StartGroupIndex = static_cast<uint32_t>(modelData.m_GroupInfos.size());
					page.GroupCount = 0; // 后面会自增
					modelData.m_Pages.push_back(page);

					streamCtx.CurrentPageIndex = static_cast<uint32_t>(modelData.m_Pages.size() - 1);
					streamCtx.CurrentOffsetInPage = 0;
				}
				else if (modelData.m_Pages.empty()) 
				{
                    // 初始化第一个 Page
					PageMetadata page = { 0, 0 };
					modelData.m_Pages.push_back(page);
				}

				// 更新 Page 元数据
				modelData.m_Pages.back().GroupCount++;

				// 填充 GroupMetadata (用于 Runtime)
				GroupMetadata metadata = group.Metadata;
				metadata.PageIndex = streamCtx.CurrentPageIndex;
				metadata.OffsetInPage = streamCtx.CurrentOffsetInPage;
				modelData.m_GroupInfos.push_back(metadata);

				// 直接写盘
				streamCtx.TempGeoFile.write(reinterpret_cast<const char*>(group.Blob.data()), groupSize);

				streamCtx.CurrentOffsetInPage += groupSize;
				streamCtx.TotalGeometrySize += groupSize;
            }

			baseGroupIndex += (uint32_t)buildResult.Groups.size();
			baseNodeIndex += (uint32_t)buildResult.Hierarchy.size();

			modelData.m_Nodes.insert(modelData.m_Nodes.end(), buildResult.Hierarchy.begin(), buildResult.Hierarchy.end());
            modelData.m_TriangleCount += buildArgs.indexCount / 3;
        }

        //if (srcMesh.skin >= 0)
        //{
        //    mesh->numJoints = 0xFFFF;
        //    mesh->startJoint = (uint16_t)srcMesh.skin;
        //}
        //else
        //{
        //    mesh->numJoints = 0;
        //    mesh->startJoint = 0xFFFF;
        //}

		modelData.m_Meshes.push_back(mesh);
    }

	Utility::Printf("Already build triangles count : %llu\n", modelData.m_TriangleCount);
}

using NodeMap = std::unordered_map<const cgltf_node*, uint32_t>;

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
	NodeMap& nodeMap
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

	if (curNode->mesh) {
		BoundingSphere sphereOS;
		AxisAlignedBox boxOS;
		CompileMesh(modelData, data, *curNode->mesh, curPos, worldXform, sphereOS, boxOS, streamCtx);
		modelBSphere = modelBSphere.Union(sphereOS);
		modelBBox.AddBoundingBox(boxOS);
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
		nextPos = WalkGraph(modelData, data, sceneGraph, modelBSphere, modelBBox, curNode->children[i], nextPos, worldXform, streamCtx, nodeMap);
		if (i < curNode->children_count - 1)
			sceneGraph[childStartPos].hasSibling = 1;
	}

	return nextPos;
}

inline void CompileTexture(const std::wstring& basePath, const std::string& fileName, uint8_t flags)
{
    CompileTextureOnDemand(basePath + Utility::UTF8ToWideString(fileName), flags);
}

//inline void SetTextureOptions(std::map<std::string, uint8_t>& optionsMap, glTF::Texture* texture, uint8_t options)
//{
//    if (texture && texture->source && optionsMap.find(texture->source->path) == optionsMap.end())
//        optionsMap[texture->source->path] = options;
//}

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

		// 初始化默认值
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
					case cgltf_wrap_mode_mirrored_repeat:
						modeSValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
					default:
						modeSValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_WRAP;
					}
				}

				uint32_t modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				if (gltfSampler)
				{
					switch (gltfSampler->wrap_t)
					{
					case cgltf_wrap_mode_clamp_to_edge:
						modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
					case cgltf_wrap_mode_mirrored_repeat:
						modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
					default:
						modeTValue = (uint32_t)D3D12_TEXTURE_ADDRESS_MODE_WRAP;
					}
				}

				const uint32_t finalValue = (modeTValue << 2 | modeSValue) << ((uint32_t)mTex * 4);
				return finalValue;
			};

		// PBR 参数映射
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
				matConst.alphaRef = static_cast<uint16_t>(srcMat.alpha_cutoff * 255.0f); // 简单映射
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

			// 插值方式映射
			switch (sampler.interpolation) 
            {
			case cgltf_interpolation_type_linear:       curve.interpolation = glTF::AnimSampler::kLinear; break;
			case cgltf_interpolation_type_step:         curve.interpolation = glTF::AnimSampler::kStep; break;
			case cgltf_interpolation_type_cubic_spline: curve.interpolation = glTF::AnimSampler::kCubicSpline; break;
			default: curve.interpolation = glTF::AnimSampler::kLinear; break;
			}

			// 关键帧数据偏移与格式
			curve.keyFrameOffset = (uint32_t)model.m_AnimationKeyFrameData.size();
			curve.keyFrameFormat = glTF::GltfAsset::MapComponentType(sampler.output->component_type);
			curve.numSegments = (float)(sampler.input->count - 1);

			// 计算 Stride (float 数量)
			uint32_t numComps = (uint32_t)cgltf_num_components(sampler.output->type);
			curve.keyFrameStride = numComps; // 假设是 float，如果是短整型需调整

			// 提取时间戳计算 Duration
			std::vector<float> times(sampler.input->count);
			cgltf_accessor_unpack_floats(sampler.input, times.data(), times.size());
			curve.startTime = times.front();
			float endTime = times.back();
			if (endTime - curve.startTime < 1e-6f)
				curve.rangeScale = 0.0f;
			else
				curve.rangeScale = curve.numSegments / (endTime - curve.startTime);
			animSet.duration = std::max(animSet.duration, endTime);

			// 提取关键帧数据
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

	const cgltf_scene* scene = sceneIdx < 0 ? data->scene : &data->scenes[sceneIdx];
	if (scene == nullptr)
		return false;

    // Aggregate all of the vertex and index buffers in this unified buffer
    //std::vector<unsigned char>& bufferMemory = model.m_GeometryData;

    std::vector<unsigned char> bufferMemory;

    model.m_BoundingSphere = BoundingSphere(kZero);
    model.m_BoundingBox = AxisAlignedBox(kZero);

	// 遍历 Scene 的根节点
	uint32_t currentPos = 0;
	if (scene->nodes_count > 0)
	{
		for (size_t i = 0; i < scene->nodes_count; ++i)
		{
			const uint32_t rootStartPos = currentPos;
			uint32_t nextPos = WalkGraph(model, data, model.m_SceneGraph, model.m_BoundingSphere, model.m_BoundingBox, scene->nodes[i], currentPos, Matrix4(kIdentity), streamCtx, nodeMap);
			if (i < scene->nodes_count - 1)
				model.m_SceneGraph[rootStartPos].hasSibling = 1;

			currentPos = nextPos;
		}
	}

	model.m_SceneGraph.resize(currentPos);

    BuildAnimations(model, asset, nodeMap);
    BuildSkins(model, asset, nodeMap);

    return true;
}

bool Renderer::SaveModel(const std::wstring& filePath, const ModelData& data, GlobalStreamingContext& streamCtx)
{
    std::ofstream outFile(filePath, std::ios::out | std::ios::binary);
    if (!outFile)
        return false;

    FileHeader header;
    std::memcpy(header.id, "MINI", 4);
    header.version = CURRENT_MINI_FILE_VERSION;
    header.numNodes = (uint32_t)data.m_SceneGraph.size();
    header.numMeshes = (uint32_t)data.m_Meshes.size();
    header.numMaterials = (uint32_t)data.m_MaterialConstants.size();
    header.numTextures = (uint32_t)data.m_TextureNames.size();

	// Cluster 数据
	header.groupCount = (uint32_t)data.m_GroupInfos.size();
	header.hierarchyNodeCount = (uint32_t)data.m_Nodes.size();
	header.pageCount = (uint32_t)data.m_Pages.size();

    header.stringTableSize = 0;
    for (const std::string& str : data.m_TextureNames)
        header.stringTableSize += (uint32_t)str.size() + 1;
    header.keyFrameDataSize = (uint32_t)data.m_AnimationKeyFrameData.size();
    header.numAnimationCurves = (uint32_t)data.m_AnimationCurves.size();
    header.numAnimations = (uint32_t)data.m_Animations.size();
    header.numJoints = (uint32_t)data.m_JointIndices.size();
    header.numCameras = (uint32_t)data.m_Cameras.size();
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

    outFile.write((char*)&header, sizeof(FileHeader));
    outFile.write((char*)data.m_SceneGraph.data(), header.numNodes * sizeof(GraphNode));

    for (const Mesh* mesh : data.m_Meshes)
    {
		const size_t meshSize = sizeof(Mesh) + sizeof(Mesh::Draw) * (mesh->numDraws - 1);
		outFile.write(reinterpret_cast<const char*>(mesh), meshSize);
    }

    outFile.write((char*)data.m_MaterialConstants.data(), header.numMaterials * sizeof(MaterialConstantData));
    outFile.write((char*)data.m_MaterialTextures.data(), header.numMaterials * sizeof(MaterialTextureData));
    for (uint32_t i = 0; i < header.numTextures; ++i)
        outFile << data.m_TextureNames[i] << '\0';
    outFile.write((char*)data.m_TextureOptions.data(), header.numTextures * sizeof(uint8_t));

    if (header.numAnimations > 0)
    {
        ASSERT(header.keyFrameDataSize > 0 && header.numAnimationCurves > 0);
        outFile.write((char*)data.m_AnimationKeyFrameData.data(), header.keyFrameDataSize);
        outFile.write((char*)data.m_AnimationCurves.data(), header.numAnimationCurves * sizeof(AnimationCurve));
        outFile.write((char*)data.m_Animations.data(), header.numAnimations * sizeof(AnimationSet));
    }
    else
    {
        ASSERT(header.keyFrameDataSize == 0 && header.numAnimationCurves == 0);
    }

    if (header.numJoints)
    {
        ASSERT(header.numJoints == (uint32_t)data.m_JointIBMs.size());
        outFile.write((char*)data.m_JointIndices.data(), header.numJoints * sizeof(uint16_t));
        outFile.write((char*)data.m_JointIBMs.data(), header.numJoints * sizeof(Matrix4));
    }

    if (header.numCameras)
    {
        outFile.write((char*)data.m_Cameras.data(), header.numCameras * sizeof(CameraData));
    }

	// --- Cluster LOD Data ---
	// Group Infos
	if (header.groupCount > 0)
		outFile.write((char*)data.m_GroupInfos.data(), header.groupCount * sizeof(GroupMetadata));

	// BVH Nodes
	if (header.hierarchyNodeCount > 0)
		outFile.write((char*)data.m_Nodes.data(), header.hierarchyNodeCount * sizeof(HierarchyNode));

	// Page Metadatas
    if (header.pageCount > 0)
		outFile.write((char*)data.m_Pages.data(), header.pageCount * sizeof(PageMetadata));

	const std::streampos currentPos = outFile.tellp();
	// 强制跳转到下一个 256KB 边界以保证整个 Blob 的页对齐
	const uint64_t alignedOffset = Math::AlignUp(static_cast<uint64_t>(currentPos), Renderer::kPageSizeInBytes);
	uint32_t startPadding = static_cast<uint32_t>(alignedOffset - static_cast<uint64_t>(currentPos));
	if (startPadding > 0)
	{
		std::vector<char> pad(startPadding, 0);
		outFile.write(pad.data(), startPadding);
	}

	header.geometryBlobOffset = alignedOffset;
	header.geometryBlobSize = streamCtx.TotalGeometrySize;

	if (streamCtx.TotalGeometrySize > 0)
	{
		streamCtx.TempGeoFile.close(); 
		std::ifstream tempIn(streamCtx.FileName, std::ios::binary);
		if (tempIn)
		{
			constexpr size_t copyBufferSize = 16 * 1024 * 1024;
			std::unique_ptr<char[]> buffer(new char[copyBufferSize]);
			while (tempIn.read(buffer.get(), copyBufferSize) || tempIn.gcount() > 0)
			{
				outFile.write(buffer.get(), tempIn.gcount());
			}
			tempIn.close();
			
		}
		const uint32_t pageSize = Renderer::kPageSizeInBytes;
		const size_t   blobSize = streamCtx.TotalGeometrySize;
		const size_t   alignedSize = Math::AlignUp(blobSize, pageSize);
		if (alignedSize != blobSize)
		{
			// 最后一页剩余空间被零填充，方便页对齐读取
			std::vector<uint8_t> padded(alignedSize - blobSize, 0);
			outFile.write(reinterpret_cast<const char*>(padded.data()), padded.size());
			header.geometryBlobSize = static_cast<uint64_t>(alignedSize);
		}
	}

	outFile.seekp(0, std::ios::beg);
	outFile.write(reinterpret_cast<const char*>(&header), sizeof(FileHeader));

    Utility::Printf("Build geometry result:\n");
    Utility::Printf("Group count: %d\n", header.groupCount);
	Utility::Printf("BVH node count: %d\n", header.hierarchyNodeCount);
	Utility::Printf("Page count: %d\n", header.pageCount);

    return true;
}
