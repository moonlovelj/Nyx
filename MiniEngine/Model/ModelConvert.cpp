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
#include "glTF.h"
#include "TextureConvert.h"
#include "MeshConvert.h"
#include "TextureManager.h"
#include "GraphicsCommon.h"
#include "../Core/Utility.h"
#include "../Core/Math/Common.h"
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

namespace Renderer
{
	// 解码 DXGI_FORMAT_R10G10B10A2_UNORM 到 float3 法线（[-1,1]）
	inline Math::Vector3 DecodeNormal_R10G10B10A2(uint32_t packed)
	{
		constexpr float inv10 = 1.0f / 1023.0f;
		const uint32_t r = (packed) & 0x3FFu;
		const uint32_t g = (packed >> 10) & 0x3FFu;
		const uint32_t b = (packed >> 20) & 0x3FFu;

		float x = float(r) * inv10;
		float y = float(g) * inv10;
		float z = float(b) * inv10;

		x = x * 2.0f - 1.0f;
		y = y * 2.0f - 1.0f;
		z = z * 2.0f - 1.0f;

		const float len2 = x * x + y * y + z * z;
		if (len2 > 0.0f)
		{
			const float invLen = 1.0f / std::sqrt(len2);
			x *= invLen; y *= invLen; z *= invLen;
		}
		return Math::Vector3(x, y, z);
	}
}

void Renderer::CompileMesh(
    std::vector<Mesh*>& meshList,
    std::vector<unsigned char>& bufferMemory,
    glTF::Mesh& srcMesh,
    uint32_t matrixIdx,
    const Matrix4& localToObject,
    BoundingSphere& boundingSphere,
    AxisAlignedBox& boundingBox
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

    std::vector<Primitive> primitives(srcMesh.primitives.size());
    for (uint32_t i = 0; i < primitives.size(); ++i)
    {
        OptimizeMesh(primitives[i], srcMesh.primitives[i], localToObject);
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

	uint32_t curVBOffset = 0;
    for (auto& iter : renderMeshes)
    {
		size_t vbSize = 0;
		size_t vbDepthSize = 0;
		size_t ibSize = 0;
		size_t totalDrawsAfterSplit = 0;
		uint32_t totalBufferSize = 0;

		size_t meshletVBSize = 0; // 存储meshlet的unique顶点，实际是顶点buffer的索引
		size_t meshletIBSize = 0; // 存储meshlet的局部三角形索引，也就是meshletVB的索引

		// Compute local space bounding sphere for all submeshes
		BoundingSphere collectiveSphere(kZero);

        MeshletBuildSettings settings;
        std::vector<std::vector<TempMeshlet>> preInfo;
        for (Primitive* draw : iter.second)
        {
			const uint32_t vertexCount = static_cast<uint32_t>(draw->VB->size() / draw->vertexStride);
			std::vector<RawVertex> rawVertices;
            rawVertices.reserve(vertexCount);
            for (size_t i = 0; i < vertexCount; i++)
            {
                const unsigned char* vertexPtr = &draw->VB->data()[draw->vertexStride * i];
				const float* position = reinterpret_cast<const float*>(vertexPtr);
				RawVertex rv;
				rv.Position = Vector3(position[0], position[1], position[2]);
				const uint32_t* normalPtr = reinterpret_cast<const uint32_t*>(vertexPtr + 12);
                rv.Normal = DecodeNormal_R10G10B10A2(normalPtr[0]);
                rawVertices.push_back(rv);
            }

            std::vector<uint32_t> indices32;
			indices32.resize(draw->primCount);
            if (draw->index32 != 0)
            {
                const uint32_t* src = reinterpret_cast<const uint32_t*>(draw->IB->data());
                std::memcpy(indices32.data(), src, draw->primCount * sizeof(uint32_t));
            }
            else
            {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(draw->IB->data());
                for (uint32_t i = 0; i < draw->primCount; ++i)
                    indices32[i] = uint32_t(src[i]);
            }

			
            std::vector<TempMeshlet> buildResult = MeshletBuilder::Build(rawVertices, indices32, settings);
            //Utility::Printf(L"buildResult \n");
            //for (const auto& lod : buildResult.LODs)
            //{
            //    Utility::Printf(L"LOD: %d, num of meshlet: %d \n", lod.LODLevel, (uint32_t)lod.MeshletIndices.size());
            //}

			// 汇总统计
			vbSize += draw->VB->size();
			vbDepthSize += draw->DepthVB->size();

			// 计算所有 LOD 的 IB 总大小
            for (const auto& m : buildResult)
            {
				ASSERT(m.Triangles.size() % 3 == 0);
                ibSize += m.Triangles.size() * 4;
				meshletVBSize += m.Vertices.size() * 4;
				meshletIBSize += (m.Triangles.size() / 3 * 4);// 为了兼容byteaddressbuffer的4字节对齐
            }
				
			collectiveSphere = collectiveSphere.Union(draw->m_BoundsLS);
			totalDrawsAfterSplit += buildResult.size();

            preInfo.emplace_back(std::move(buildResult));
        }

		ibSize = Math::AlignUp(ibSize, 4);
        meshletIBSize = Math::AlignUp(meshletIBSize, 4);
		totalBufferSize = (uint32_t)(vbSize + vbDepthSize + ibSize + meshletVBSize + meshletIBSize);

		Utility::ByteArray stagingBuffer;
		stagingBuffer.reset(new std::vector<unsigned char>(totalBufferSize));
		uint8_t* uploadMem = reinterpret_cast<uint8_t*>(stagingBuffer->data());
		
        // 分配 Mesh（使用 meshlet 粒度的 draw 数）
        size_t numDraws = totalDrawsAfterSplit;
        Mesh* mesh = (Mesh*)malloc(sizeof(Mesh) + sizeof(Mesh::Draw) * (numDraws - 1));

        mesh->bounds[0] = collectiveSphere.GetCenter().GetX();
        mesh->bounds[1] = collectiveSphere.GetCenter().GetY();
        mesh->bounds[2] = collectiveSphere.GetCenter().GetZ();
        mesh->bounds[3] = collectiveSphere.GetRadius();
        mesh->vbOffset = (uint32_t)bufferMemory.size();
        mesh->vbSize = (uint32_t)vbSize;
        mesh->vbDepthOffset = (uint32_t)(bufferMemory.size() + vbSize);
        mesh->vbDepthSize = (uint32_t)vbDepthSize;
        mesh->ibOffset = (uint32_t)(bufferMemory.size() + vbSize + vbDepthSize);
        mesh->ibSize = (uint32_t)ibSize;
        mesh->vbStride = (uint8_t)iter.second[0]->vertexStride;
        mesh->ibFormat = uint8_t(iter.second[0]->index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT);
        mesh->meshCBV = (uint16_t)matrixIdx;
        mesh->materialCBV = iter.second[0]->materialIdx;
        mesh->psoFlags = iter.second[0]->psoFlags;
        mesh->pso = 0xFFFF;
        if (srcMesh.skin >= 0)
        {
            mesh->numJoints = 0xFFFF;
            mesh->startJoint = (uint16_t)srcMesh.skin;
        }
        else
        {
            mesh->numJoints = 0;
            mesh->startJoint = 0xFFFF;
        }

        mesh->numDraws = (uint16_t)numDraws;

		// ========== 填充 Draw ==========
        uint32_t drawIdx = 0;
        uint32_t curPrimVBOffset = 0;
        uint32_t curVertOffset = 0;
        uint32_t curMeshletIBOffset = 0;
        uint32_t curIndexOffset = 0;
        uint32_t curPrimDepthVBOffset = 0;
		uint32_t curMeshletVBOffset = 0;
		uint32_t curMeshletPrimOffset = 0;

		for (size_t iPrim = 0; iPrim < preInfo.size(); ++iPrim)
		{
			const std::vector<TempMeshlet>& info = preInfo[iPrim];

			for (const auto& m : info)
			{
				Mesh::Draw& d = mesh->draw[drawIdx];

				d.primCount = (uint32_t)m.Triangles.size();
				d.baseVertex = curVertOffset;
				d.startIndex = curIndexOffset;
				std::memcpy(d.bounds, m.Sphere, sizeof(d.bounds));
                if (mesh->psoFlags & PSOFlags::kHasSkin)
                {
					d.bounds[3] *= 2.f; // TODO 蒙皮的包围球需要更精确计算
                }

				d.meshletVertexCount = (uint32_t)m.Vertices.size();
                d.meshletVertexOffset = (uint32_t)(bufferMemory.size() + vbSize + vbDepthSize + ibSize + curMeshletVBOffset);

				d.meshletPrimCount = (uint32_t)m.Triangles.size();
				d.meshletPrimOffset = (uint32_t)(bufferMemory.size() + vbSize + vbDepthSize + ibSize + meshletVBSize + curMeshletPrimOffset);

				// LOD 数据
				d.parentError = m.ParentError;
				std::memcpy(d.parentBounds, m.ParentBounds, sizeof(d.parentBounds));
				d.lodError = m.LodError;
				std::memcpy(d.lodBounds, m.LodBounds, sizeof(d.lodBounds));
				d.lodLevel = m.LODLevel;

				std::memcpy(uploadMem + vbSize + vbDepthSize + ibSize + curMeshletVBOffset,
					m.Vertices.data(), m.Vertices.size() * 4);

				std::vector<uint8_t> meshletTriangles8(m.Triangles.size() / 3 * 4);
                for (size_t t = 0; t < m.Triangles.size() / 3; ++t)
                {
                    meshletTriangles8[t * 4 + 0] = m.Triangles[t * 3 + 0];
                    meshletTriangles8[t * 4 + 1] = m.Triangles[t * 3 + 1];
                    meshletTriangles8[t * 4 + 2] = m.Triangles[t * 3 + 2];
                    meshletTriangles8[t * 4 + 3] = 0; // padding
				}
				std::memcpy(uploadMem + vbSize + vbDepthSize + ibSize + meshletVBSize + curMeshletPrimOffset,
                    meshletTriangles8.data(), meshletTriangles8.size());

				++drawIdx;
				curIndexOffset += (uint32_t)m.Triangles.size();
				curMeshletVBOffset += (uint32_t)m.Vertices.size() * 4;
                curMeshletPrimOffset += (uint32_t)m.Triangles.size() / 3 * 4;
			}

			// 拷贝 VB
			std::memcpy(uploadMem + curPrimVBOffset,
                iter.second[iPrim]->VB->data(), iter.second[iPrim]->VB->size());
			curPrimVBOffset += (uint32_t)iter.second[iPrim]->VB->size();
			curVertOffset += (uint32_t)(iter.second[iPrim]->VB->size() / iter.second[iPrim]->vertexStride);

			// 拷贝 DepthVB
			std::memcpy(uploadMem + vbSize + curPrimDepthVBOffset,
                iter.second[iPrim]->DepthVB->data(), iter.second[iPrim]->DepthVB->size());
			curPrimDepthVBOffset += (uint32_t)iter.second[iPrim]->DepthVB->size();

			// 拷贝所有 LOD 的 IB
            std::vector<uint32_t> meshletIndices32;
            meshletIndices32.reserve(settings.MaxMeshletTriangles * 3);
            for (const auto& m : info)
			{
                meshletIndices32.clear();
                for (const auto& localIndex : m.Triangles)
                {
                    meshletIndices32.push_back(m.Vertices[localIndex]);
                }
				std::memcpy(uploadMem + vbSize + vbDepthSize + curMeshletIBOffset,
					meshletIndices32.data(), meshletIndices32.size() * 4);
				curMeshletIBOffset += (uint32_t)meshletIndices32.size() * 4;
			}
		}

        meshList.push_back(mesh);

		bufferMemory.insert(bufferMemory.end(), stagingBuffer->begin(), stagingBuffer->end());

		curVBOffset += totalBufferSize;
    }
}


static uint32_t WalkGraph(
    std::vector<GraphNode>& sceneGraph,
    BoundingSphere& modelBSphere,
    AxisAlignedBox& modelBBox,
    std::vector<Mesh*>& meshList,
    std::vector<unsigned char>& bufferMemory,
    std::vector<CameraData>& cameraData,
    const std::vector<glTF::Node*>& siblings,
    uint32_t curPos,
    const Matrix4& xform
    )
{
    size_t numSiblings = siblings.size();

    for (size_t i = 0; i < numSiblings; ++i)
    {
        glTF::Node* curNode = siblings[i];
        GraphNode& thisGraphNode = sceneGraph[curPos];
        thisGraphNode.hasChildren = 0;
        thisGraphNode.hasSibling = 0;
        thisGraphNode.matrixIdx = curPos;
        thisGraphNode.skeletonRoot = curNode->skeletonRoot;
        curNode->linearIdx = curPos;

        // They might not be used, but we have space to hold the neutral values which could be
        // useful when updating the matrix via animation.
        std::memcpy((float*)&thisGraphNode.scale, curNode->scale, sizeof(curNode->scale));
        std::memcpy((float*)&thisGraphNode.rotation, curNode->rotation, sizeof(curNode->rotation));

        if (curNode->hasMatrix)
        {
            std::memcpy((float*)&thisGraphNode.xform, curNode->matrix, sizeof(curNode->matrix));
        }
        else
        {
            thisGraphNode.xform = Matrix4(
                Matrix3(thisGraphNode.rotation) * Matrix3::MakeScale(thisGraphNode.scale),
                Vector3(*(const XMFLOAT3*)curNode->translation)
            );
        }

        const Matrix4 LocalXform = xform * thisGraphNode.xform;

        if (!curNode->pointsToCamera && curNode->mesh != nullptr)
        {
            BoundingSphere sphereOS;
            AxisAlignedBox boxOS;
            CompileMesh(meshList, bufferMemory, *curNode->mesh, curPos, LocalXform, sphereOS, boxOS);
            modelBSphere = modelBSphere.Union(sphereOS);
            modelBBox.AddBoundingBox(boxOS);
        }
        else if (curNode->pointsToCamera && curNode->camera != nullptr)
        {
            CameraData camera;
			camera.aspectRatio = curNode->camera->aspectRatio;
			camera.yfov = curNode->camera->yfov;
			camera.znear = curNode->camera->znear;
			camera.zfar = curNode->camera->zfar;
            camera.matrixIdx = curPos;
            camera.type = curNode->camera->type == glTF::Camera::kPerspective ? CameraData::kPerspective : CameraData::kOrthographic;
            cameraData.emplace_back(camera);
        }

        uint32_t nextPos = curPos + 1;

        if (curNode->children.size() > 0)
        {
            thisGraphNode.hasChildren = 1;
            nextPos = WalkGraph(sceneGraph, modelBSphere, modelBBox, meshList, bufferMemory, cameraData, curNode->children, nextPos, LocalXform);
        }

        // Are there more siblings?
        if (i + 1 < numSiblings)
        {
            thisGraphNode.hasSibling = 1;
        }
        
        curPos = nextPos;
    }

    return curPos;
}

inline void CompileTexture(const std::wstring& basePath, const std::string& fileName, uint8_t flags)
{
    CompileTextureOnDemand(basePath + Utility::UTF8ToWideString(fileName), flags);
}

inline void SetTextureOptions(std::map<std::string, uint8_t>& optionsMap, glTF::Texture* texture, uint8_t options)
{
    if (texture && texture->source && optionsMap.find(texture->source->path) == optionsMap.end())
        optionsMap[texture->source->path] = options;
}

void BuildMaterials(ModelData& model, const glTF::Asset& asset)
{
    //static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

    // Replace texture filename extensions with "DDS" in the string table
    model.m_TextureNames.resize(asset.m_images.size());
    for (size_t i = 0; i < asset.m_images.size(); ++i)
        model.m_TextureNames[i] = asset.m_images[i].path;

    std::map<std::string, uint8_t> textureOptions;

    const uint32_t numMaterials = (uint32_t)asset.m_materials.size();

    model.m_MaterialConstants.resize(numMaterials);
    model.m_MaterialTextures.resize(numMaterials);

    for (uint32_t i = 0; i < numMaterials; ++i)
    {
        const glTF::Material& srcMat = asset.m_materials[i];

        MaterialConstantData& material = model.m_MaterialConstants[i];
        material.baseColorFactor[0] = srcMat.baseColorFactor[0];
        material.baseColorFactor[1] = srcMat.baseColorFactor[1];
        material.baseColorFactor[2] = srcMat.baseColorFactor[2];
        material.baseColorFactor[3] = srcMat.baseColorFactor[3];
        material.emissiveFactor[0] = srcMat.emissiveFactor[0];
        material.emissiveFactor[1] = srcMat.emissiveFactor[1];
        material.emissiveFactor[2] = srcMat.emissiveFactor[2];
        material.normalTextureScale = srcMat.normalTextureScale;
        material.metallicFactor = srcMat.metallicFactor;
        material.roughnessFactor = srcMat.roughnessFactor;
        material.flags = srcMat.flags;

        MaterialTextureData& dstMat = model.m_MaterialTextures[i];
        dstMat.addressModes = 0;

        for (uint32_t ti = 0; ti < kNumTextures; ++ti)
        {
            dstMat.stringIdx[ti] = 0xFFFF;

            if (srcMat.textures[ti] != nullptr)
            {
                if (srcMat.textures[ti]->source != nullptr)
                {
                    dstMat.stringIdx[ti] = uint16_t(srcMat.textures[ti]->source - asset.m_images.data());
                }

                if (srcMat.textures[ti]->sampler != nullptr)
                {
                    dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapS << (ti * 4);
                    dstMat.addressModes |= srcMat.textures[ti]->sampler->wrapT << (ti * 4 + 2);
                }
                else
                {
                    dstMat.addressModes |= 0x5 << (ti * 4);
                }
            }
            else
            {
                dstMat.addressModes |= 0x5 << (ti * 4);
            }
        }

        SetTextureOptions(textureOptions, srcMat.textures[kBaseColor], TextureOptions(true, srcMat.alphaBlend | srcMat.alphaTest));
        SetTextureOptions(textureOptions, srcMat.textures[kMetallicRoughness], TextureOptions(false));
        SetTextureOptions(textureOptions, srcMat.textures[kOcclusion], TextureOptions(false));
        SetTextureOptions(textureOptions, srcMat.textures[kEmissive], TextureOptions(true));
        SetTextureOptions(textureOptions, srcMat.textures[kNormal], TextureOptions(false));
    }

    model.m_TextureOptions.clear();
    for (auto name : model.m_TextureNames)
    {
        auto iter = textureOptions.find(name);
        if (iter != textureOptions.end())
        {
            model.m_TextureOptions.push_back(iter->second);
            CompileTextureOnDemand(asset.m_basePath + Utility::UTF8ToWideString(iter->first), iter->second);
        }
        else
            model.m_TextureOptions.push_back(0xFF);
    }
    ASSERT(model.m_TextureOptions.size() == model.m_TextureNames.size());
}

void BuildAnimations(ModelData& model, const glTF::Asset& asset)
{
    size_t numAnimations = asset.m_animations.size();
    if (numAnimations == 0)
        return;

    model.m_Animations.resize(numAnimations);
    uint32_t animIdx = 0;

    for (const glTF::Animation& anim : asset.m_animations)
    {
        AnimationSet& animSet = model.m_Animations[animIdx++];
        animSet.duration = 0.0f;
        animSet.firstCurve = (uint32_t)model.m_AnimationCurves.size();
        animSet.numCurves = (uint32_t)anim.m_channels.size();

        for (size_t i = 0; i < animSet.numCurves; ++i)
        {
            const glTF::AnimChannel& channel = anim.m_channels[i];
            const glTF::AnimSampler& sampler = *channel.m_sampler;

            ASSERT(channel.m_target->linearIdx >= 0);

            AnimationCurve curve;
            curve.targetNode = channel.m_target->linearIdx;
            curve.targetPath = channel.m_path;
            curve.interpolation = sampler.m_interpolation;
            curve.keyFrameOffset = model.m_AnimationKeyFrameData.size();
            curve.keyFrameFormat = std::min<uint32_t>(sampler.m_output->componentType, AnimationCurve::kFloat);
            curve.numSegments = sampler.m_output->count - 1.0f;

            // In glTF, stride==0 means "packed tightly"
            if (sampler.m_output->stride == 0)
            {
                uint32_t numComponents = sampler.m_output->type + 1;
                uint32_t bytesPerComponent = sampler.m_output->componentType / 2 + 1;
                curve.keyFrameStride = numComponents * bytesPerComponent / 4;
            }
            else
            {
                ASSERT(sampler.m_output->stride <= 16 && sampler.m_output->stride % 4 == 0);
                curve.keyFrameStride = sampler.m_output->stride / 4;
            }

            // Determine start and stop time stamps
            const float* timeStamps = (float*)sampler.m_input->dataPtr;
            curve.startTime = timeStamps[0];

            const float endTime = timeStamps[sampler.m_output->count - 1];
            curve.rangeScale = curve.numSegments / (endTime - curve.startTime);

            animSet.duration = std::max<float>(animSet.duration, endTime);

            // Append this curve data
            model.m_AnimationKeyFrameData.insert(
                model.m_AnimationKeyFrameData.end(),
                sampler.m_output->dataPtr,
                sampler.m_output->dataPtr + sampler.m_output->count * curve.keyFrameStride * 4);

            model.m_AnimationCurves.push_back(curve);
        }
    }
}

void BuildSkins(ModelData& model, const glTF::Asset& asset)
{
    size_t numSkins = asset.m_skins.size();
    if (numSkins == 0)
        return;

    std::vector<std::pair<uint16_t, uint16_t>> skinMap;
    skinMap.reserve(asset.m_skins.size());

    for (const glTF::Skin& skin : asset.m_skins)
    {
        // Record offset and joint count
        uint16_t numJoints = (uint16_t)skin.joints.size();
        uint16_t curOffset = (uint16_t)model.m_JointIndices.size();
        skinMap.push_back(std::make_pair(curOffset, numJoints));

        // Append remapped joint indices
        for (glTF::Node* joint : skin.joints)
        {
            ASSERT(joint->linearIdx >= 0, "Skin joint not present in node hierarchy");
            model.m_JointIndices.push_back((uint16_t)joint->linearIdx);
        }

        // Append IBMs
        Matrix4* IBMstart = (Matrix4*)skin.inverseBindMatrices->dataPtr;
        Matrix4* IBMend = IBMstart + skin.inverseBindMatrices->count;
        ASSERT(skin.inverseBindMatrices->count == numJoints);
        model.m_JointIBMs.insert(model.m_JointIBMs.end(), IBMstart, IBMend);
    }

    // Assign skinned meshes the proper joint offset and count
    for (Mesh* mesh : model.m_Meshes)
    {
        if (mesh->numJoints != 0)
        {
            std::pair<uint16_t, uint16_t> offsetAndCount = skinMap[mesh->startJoint];
            mesh->startJoint = offsetAndCount.first;
            mesh->numJoints = offsetAndCount.second;
        }
    }
}

bool Renderer::BuildModel(ModelData& model, const glTF::Asset& asset, int sceneIdx)
{
    BuildMaterials(model, asset);

    // Generate scene graph and meshes
    model.m_SceneGraph.resize(asset.m_nodes.size());
    const glTF::Scene* scene = sceneIdx < 0 ? asset.m_scene : &asset.m_scenes[sceneIdx];
    if (scene == nullptr)
        return false;

    // Aggregate all of the vertex and index buffers in this unified buffer
    std::vector<unsigned char>& bufferMemory = model.m_GeometryData;

    model.m_BoundingSphere = BoundingSphere(kZero);
    model.m_BoundingBox = AxisAlignedBox(kZero);
    uint32_t numNodes = WalkGraph(model.m_SceneGraph, model.m_BoundingSphere, model.m_BoundingBox, model.m_Meshes, bufferMemory, model.m_Cameras, scene->nodes, 0, Matrix4(kIdentity));
    model.m_SceneGraph.resize(numNodes);

    BuildAnimations(model, asset);
    BuildSkins(model, asset);

    return true;
}

bool Renderer::SaveModel(const std::wstring& filePath, const ModelData& data)
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
    header.meshDataSize = 0;
    for (const Mesh* mesh : data.m_Meshes)
        header.meshDataSize += (uint32_t)sizeof(Mesh) + (mesh->numDraws - 1) * (uint32_t)sizeof(Mesh::Draw);
    header.numTextures = (uint32_t)data.m_TextureNames.size();
    header.stringTableSize = 0;
    for (const std::string& str : data.m_TextureNames)
        header.stringTableSize += (uint32_t)str.size() + 1;
    header.geometrySize = (uint32_t)data.m_GeometryData.size();
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
    outFile.write((char*)data.m_GeometryData.data(), header.geometrySize);
    outFile.write((char*)data.m_SceneGraph.data(), header.numNodes * sizeof(GraphNode));
    for (const Mesh* mesh : data.m_Meshes)
        outFile.write((char*)mesh, sizeof(Mesh) + (mesh->numDraws - 1) * sizeof(Mesh::Draw));
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

    return true;
}
