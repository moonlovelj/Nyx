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

#include "Model.h"
#include "Renderer.h"
#include "ConstantBuffers.h"
#include "CommandBucketer.h"

#include <cstdio>
#include <cstdlib>
#include <new>

using namespace Math;
using namespace Renderer;

void Model::Destroy()
{
    m_BoundingSphere = BoundingSphere(kZero);
    m_MaterialConstants.Destroy();
    m_NumNodes = 0;
    m_NumMeshes = 0;
    m_SceneGraph = nullptr;
}

uint32_t Model::GetNumTotalDraws() const
{
    uint32_t totalDraws = 0;
    for(const auto& meshPtr : m_Meshes)
    {
        totalDraws += meshPtr->numDraws;
	}
    return totalDraws;
}

void ModelInstance::SetupInstanceData(InstanceConstants* instanceContants) const
{
    if (!m_Model) return;

	uint32_t localInstanceIdx = 0;
    for (const auto& meshPtr : m_Model->m_Meshes)
    {
        for (size_t i = 0; i < meshPtr->numDraws; i++)
        {
            InstanceConstants& data = instanceContants[localInstanceIdx++];
            data.MeshBufferIdx = meshPtr->matrixIdx + m_Alloc.meshConstantBase;
			data.JointBufferIdx = 0XFFFFFFFF;
			//std::memcpy(data.BoundingSphere, meshPtr->draw[i].boundingSphere, sizeof(float) * 4);
            std::memcpy(data.BBoxMin, meshPtr->draw[i].boundingBoxMin, sizeof(float) * 3);
            std::memcpy(data.BBoxMax, meshPtr->draw[i].boundingBoxMax, sizeof(float) * 3);
        }
    }
}

ModelInstance::ModelInstance( std::shared_ptr<const Model> sourceModel )
    : m_Model(sourceModel), m_Locator(kIdentity)
{
    //static_assert((_alignof(MeshConstants) & 255) == 0, "CBVs need 256 byte alignment");

    //DestroyMeshIndirectCommands();

    if (sourceModel == nullptr)
    {
        m_BoundingSphereTransforms = nullptr;
        m_AnimGraph = nullptr;
        m_AnimState.clear();
        m_Cameras.clear();
    }
    else
    {
		m_Alloc = InstanceResourceManager::Allocate(
			m_Model->m_NumNodes,
			m_Model->m_NumJoints
		);

        m_BoundingSphereTransforms.reset(new AffineTransform[sourceModel->m_NumNodes]);

        if (sourceModel->m_NumAnimations > 0)
        {
            m_AnimGraph.reset(new GraphNode[sourceModel->m_NumNodes]);
            std::memcpy(m_AnimGraph.get(), sourceModel->m_SceneGraph.get(), sourceModel->m_NumNodes * sizeof(GraphNode));
            m_AnimState.resize(sourceModel->m_NumAnimations);
        }
        else
        {
            m_AnimGraph = nullptr;
            m_AnimState.clear();
        }

		m_Cameras.clear();
		for (size_t i = 0; i < sourceModel->m_NumCameras; i++)
		{
			const CameraData& cameraData = sourceModel->m_Cameras[i];
			if (cameraData.type == CameraData::kPerspective)
			{
				Camera* camera = new Camera();
				camera->SetPerspectiveMatrix(cameraData.yfov, cameraData.aspectRatio, cameraData.znear, cameraData.zfar);
				m_Cameras.emplace(cameraData.matrixIdx, camera);
			}
			else
			{
				ASSERT(false, "Not support load orthographic camera");
			}
		}

        GatherDrawItems();
    }
}

ModelInstance::ModelInstance( const ModelInstance& modelInstance )
    : ModelInstance(modelInstance.m_Model)
{
}

ModelInstance& ModelInstance::operator=( std::shared_ptr<const Model> sourceModel )
{
    m_Model = sourceModel;
    m_Locator = UniformTransform(kIdentity);

    //DestroyMeshIndirectCommands();

    if (sourceModel == nullptr)
    {
        m_BoundingSphereTransforms = nullptr;
        m_AnimGraph = nullptr;
        m_AnimState.clear();
        m_Cameras.clear();
    }
    else
    {
		m_Alloc = InstanceResourceManager::Allocate(
			m_Model->m_NumNodes,
			m_Model->m_NumJoints
		);

        m_BoundingSphereTransforms.reset(new AffineTransform[sourceModel->m_NumNodes]);

        if (sourceModel->m_NumAnimations > 0)
        {
            m_AnimGraph.reset(new GraphNode[sourceModel->m_NumNodes]);
            std::memcpy(m_AnimGraph.get(), sourceModel->m_SceneGraph.get(), sourceModel->m_NumNodes * sizeof(GraphNode));
            m_AnimState.resize(sourceModel->m_NumAnimations);
        }
        else
        {
            m_AnimGraph = nullptr;
            m_AnimState.clear();
        }

        m_Cameras.clear();
        for (size_t i = 0; i < sourceModel->m_NumCameras; i++)
        {
            const CameraData& cameraData = sourceModel->m_Cameras[i];
            if (cameraData.type == CameraData::kPerspective)
            {
                Camera* camera = new Camera();
                camera->SetPerspectiveMatrix(cameraData.yfov, cameraData.aspectRatio, cameraData.znear, cameraData.zfar);
                m_Cameras.emplace(cameraData.matrixIdx, camera);
            }
            else
            {
                ASSERT(false, "Not support load orthographic camera");
            }
        }

        GatherDrawItems();
    }
    return *this;
}

void ModelInstance::Update(float deltaTime, MeshConstants* meshConstantsCPU, Joint* jointCPU)
{
	ASSERT(meshConstantsCPU != nullptr, "Must provide a CPU-side mesh constants buffer");
	ASSERT(jointCPU != nullptr, "Must provide a CPU-side joint buffer");

    if (m_Model == nullptr)
        return;

    static const size_t kMaxStackDepth = 32;

    size_t stackIdx = 0;
    Matrix4 matrixStack[kMaxStackDepth];
    Matrix4 ParentMatrix = Matrix4((AffineTransform)m_Locator);

    MeshConstants* cb = meshConstantsCPU;//(MeshConstants*)m_MeshConstantsCPU.Map();

    if (m_AnimGraph)
    {
        UpdateAnimations(deltaTime);

        for (uint32_t i = 0; i < m_Model->m_NumNodes; ++i)
        {
            GraphNode& node = m_AnimGraph[i];

            // Regenerate the 3x3 matrix if it has scale or rotation
            if (node.staleMatrix)
            {
                node.staleMatrix = false;
                node.xform.Set3x3(Matrix3(node.rotation) * Matrix3::MakeScale(node.scale));
            }
        }
    }

    const GraphNode* sceneGraph = m_AnimGraph ? m_AnimGraph.get() : m_Model->m_SceneGraph.get();

    // Traverse the scene graph in depth first order.  This is the same as linear order
    // for how the nodes are stored in memory.  Uses a matrix stack instead of recursion.
    for (const GraphNode* Node = sceneGraph; ; ++Node)
    {
        Matrix4 xform = Node->xform;
        if (!Node->skeletonRoot)
            xform = ParentMatrix * xform;

        // Concatenate the transform with the parent's matrix and update the matrix list
        {
            // Scoped so that I don't forget that I'm pointing to write-combined memory and
            // should not read from it.
            MeshConstants& cbv = cb[Node->matrixIdx];
            cbv.World = xform;
            cbv.WorldIT = InverseTranspose(xform.Get3x3());

            m_BoundingSphereTransforms[Node->matrixIdx] = AffineTransform(
                (Vector3)xform.GetX(),
                (Vector3)xform.GetY(),
                (Vector3)xform.GetZ(),
                (Vector3)xform.GetW());
       }

        // If the next node will be a descendent, replace the parent matrix with our new matrix
        if (Node->hasChildren)
        {
            // ...but if we have siblings, make sure to backup our current parent matrix on the stack
            if (Node->hasSibling)
            {
                ASSERT(stackIdx < kMaxStackDepth, "Overflowed the matrix stack");
                matrixStack[stackIdx++] = ParentMatrix;
            }
            ParentMatrix = xform;
        }
        else if (!Node->hasSibling)
        {
            // There are no more siblings.  If the stack is empty, we are done.  Otherwise, pop
            // a matrix off the stack and continue.
            if (stackIdx == 0)
                break;

            ParentMatrix = matrixStack[--stackIdx];
        }
    }

    // Update cameras
	for (auto it = m_Cameras.begin(); it != m_Cameras.end(); ++it) 
    {
        it->second->SetTransform(m_BoundingSphereTransforms[it->first]);
	}

    // Update skeletal joints
    Joint* jb = (Joint*)jointCPU;//m_MeshJointsCPU.Map();
    for (uint32_t i = 0; i < m_Model->m_NumJoints; ++i)
    {
        Joint& joint = jb[i];
        joint.posXform = cb[m_Model->m_JointIndices[i]].World * m_Model->m_JointIBMs[i];
        joint.nrmXform = InverseTranspose(joint.posXform.Get3x3());
    }
}

void ModelInstance::Resize( float newRadius )
{
    if (m_Model == nullptr)
        return;

    m_Locator.SetScale(newRadius / m_Model->m_BoundingSphere.GetRadius());
}

Vector3 ModelInstance::GetCenter() const
{
    if (m_Model == nullptr)
        return Vector3(kOrigin);

    return m_Locator * m_Model->m_BoundingSphere.GetCenter();
}

Scalar ModelInstance::GetRadius() const
{
    if (m_Model == nullptr)
        return Scalar(kZero);

    return m_Locator.GetScale() * m_Model->m_BoundingSphere.GetRadius();
}

Math::BoundingSphere ModelInstance::GetBoundingSphere() const
{
    if (m_Model == nullptr)
        return BoundingSphere(kZero);

    return m_Locator * m_Model->m_BoundingSphere;
}

Math::OrientedBox ModelInstance::GetBoundingBox() const
{
    if (m_Model == nullptr)
        return AxisAlignedBox(Vector3(kZero), Vector3(kZero));

    return m_Locator * m_Model->m_BoundingBox;
}

void ModelInstance::GatherDrawItems() const
{
    if (m_Model)
    {
		uint32_t localInstanceID = m_Alloc.instanceID * m_Model->GetNumTotalDraws();
        for (const auto& mesh : m_Model->m_Meshes)
        {
            for (uint32_t i = 0; i < mesh->numDraws; i++)
            {
                 DrawCommandManager::AddDrawItem({
                     localInstanceID++,
                     mesh->draw[i].rootNodeIndex,
                    });
            }
        }
    }
}
