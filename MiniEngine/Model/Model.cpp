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
#include "GPUDriven/ExecuteIndirect.h"

#include <cstdio>
#include <cstdlib>
#include <new>

using namespace Math;
using namespace Renderer;

void Model::Destroy()
{
    m_BoundingSphere = BoundingSphere(kZero);
    m_DataBuffer.Destroy();
    m_MaterialConstants.Destroy();
    m_NumNodes = 0;
    m_NumMeshes = 0;
    m_MeshData = nullptr;
    m_SceneGraph = nullptr;
}

void Model::Render(
    MeshSorter& sorter,
    const GpuBuffer& meshConstants,
    const AffineTransform sphereTransforms[],
    const std::vector<GPUDriven::IndirectArgsBufferWarp>& indirectArgsBuffers,
    const GpuBuffer& meshJoints) const
{
    // Pointer to current mesh
    const uint8_t* pMesh = m_MeshData.get();

    const Frustum& frustum = sorter.GetViewFrustum();
    const AffineTransform& viewMat = (const AffineTransform&)sorter.GetViewMatrix();

    for (uint32_t i = 0; i < m_NumMeshes; ++i)
    {
        const Mesh& mesh = *(const Mesh*)pMesh;

        const AffineTransform& sphereXform = sphereTransforms[mesh.meshCBV];
        Scalar scaleXSqr = LengthSquare((Vector3)sphereXform.GetX());
        Scalar scaleYSqr = LengthSquare((Vector3)sphereXform.GetY());
        Scalar scaleZSqr = LengthSquare((Vector3)sphereXform.GetZ());
        Scalar sphereScale = Sqrt(Max(Max(scaleXSqr, scaleYSqr), scaleZSqr));

		BoundingSphere sphereLS((const XMFLOAT4*)mesh.bounds);
		BoundingSphere sphereWS = BoundingSphere(sphereXform * sphereLS.GetCenter(), sphereScale * sphereLS.GetRadius());
        BoundingSphere sphereVS = BoundingSphere(viewMat * sphereWS.GetCenter(), sphereWS.GetRadius());

        if (frustum.IntersectSphere(sphereVS))
        {
            float distance = -sphereVS.GetCenter().GetZ() - sphereVS.GetRadius();
            sorter.AddMesh(mesh, distance,
                meshConstants.GetGpuVirtualAddress() + sizeof(MeshConstants) * mesh.meshCBV,
                m_MaterialConstants.GetGpuVirtualAddress() + sizeof(MaterialConstants) * mesh.materialCBV,
                m_DataBuffer.GetGpuVirtualAddress(), indirectArgsBuffers[i], mesh.numJoints > 0 ? meshJoints.GetGpuVirtualAddress() + sizeof(Joint) * mesh.startJoint : D3D12_GPU_VIRTUAL_ADDRESS_NULL);
        }

        pMesh += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
}

void ModelInstance::Render(MeshSorter& sorter) const
{
    if (m_Model != nullptr)
    {
        //const Frustum& frustum = sorter.GetWorldFrustum();
        m_Model->Render(sorter, m_MeshConstantsGPU, m_BoundingSphereTransforms.get(), m_MeshIndirectArgsBuffers,
            m_MeshJointsGPU);
    }
}

ModelInstance::ModelInstance( std::shared_ptr<const Model> sourceModel )
    : m_Model(sourceModel), m_Locator(kIdentity)
{
    static_assert((_alignof(MeshConstants) & 255) == 0, "CBVs need 256 byte alignment");

    DestroyMeshIndirectCommands();

    if (sourceModel == nullptr)
    {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
        m_BoundingSphereTransforms = nullptr;
        m_AnimGraph = nullptr;
        m_AnimState.clear();
        m_Cameras.clear();
		m_MeshJointsCPU.Destroy();
		m_MeshJointsGPU.Destroy();
    }
    else
    {
        m_MeshConstantsCPU.Create(L"Mesh Constant Upload Buffer", sourceModel->m_NumNodes * sizeof(MeshConstants));
        m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));

        m_BoundingSphereTransforms.reset(new AffineTransform[sourceModel->m_NumNodes]);

        m_MeshJointsCPU.Create(L"Mesh Joints Upload Buffer", std::max(sourceModel->m_NumJoints, 1u) * sizeof(Joint));
        m_MeshJointsGPU.Create(L"Mesh Joints GPU Buffer", std::max(sourceModel->m_NumJoints, 1u), sizeof(Joint));

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

        CreateMeshIndirectCommands();
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

    DestroyMeshIndirectCommands();

    if (sourceModel == nullptr)
    {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
        m_BoundingSphereTransforms = nullptr;
        m_AnimGraph = nullptr;
        m_AnimState.clear();
        m_Cameras.clear();
		m_MeshJointsCPU.Destroy();
        m_MeshJointsGPU.Destroy();
    }
    else
    {
        m_MeshConstantsCPU.Create(L"Mesh Constant Upload Buffer", sourceModel->m_NumNodes * sizeof(MeshConstants));
        m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));

        m_BoundingSphereTransforms.reset(new AffineTransform[sourceModel->m_NumNodes]);

		m_MeshJointsCPU.Create(L"Mesh Joints Upload Buffer", std::max(sourceModel->m_NumJoints, 1u) * sizeof(Joint));
		m_MeshJointsGPU.Create(L"Mesh Joints GPU Buffer", std::max(sourceModel->m_NumJoints, 1u), sizeof(Joint));

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

        CreateMeshIndirectCommands();
    }
    return *this;
}

void ModelInstance::Update(GraphicsContext& gfxContext, float deltaTime)
{
    if (m_Model == nullptr)
        return;

    static const size_t kMaxStackDepth = 32;

    size_t stackIdx = 0;
    Matrix4 matrixStack[kMaxStackDepth];
    Matrix4 ParentMatrix = Matrix4((AffineTransform)m_Locator);

    MeshConstants* cb = (MeshConstants*)m_MeshConstantsCPU.Map();

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
    Joint* jb = (Joint*)m_MeshJointsCPU.Map();
    for (uint32_t i = 0; i < m_Model->m_NumJoints; ++i)
    {
        Joint& joint = jb[i];
        joint.posXform = cb[m_Model->m_JointIndices[i]].World * m_Model->m_JointIBMs[i];
        joint.nrmXform = InverseTranspose(joint.posXform.Get3x3());
    }
    m_MeshJointsCPU.Unmap();

    m_MeshConstantsCPU.Unmap();

    gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
    gfxContext.GetCommandList()->CopyBufferRegion(m_MeshConstantsGPU.GetResource(), 0, m_MeshConstantsCPU.GetResource(), 0, m_MeshConstantsCPU.GetBufferSize());
    gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);

	gfxContext.TransitionResource(m_MeshJointsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
	gfxContext.GetCommandList()->CopyBufferRegion(m_MeshJointsGPU.GetResource(), 0, m_MeshJointsCPU.GetResource(), 0, m_MeshJointsCPU.GetBufferSize());
	gfxContext.TransitionResource(m_MeshJointsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
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

void ModelInstance::CreateMeshIndirectCommands()
{
    if (m_Model)
    {
        m_MeshIndirectArgsBuffers.reserve(m_Model->m_NumMeshes);
		const uint8_t* pMesh = m_Model->m_MeshData.get();
        for (uint32_t i = 0; i < m_Model->m_NumMeshes; i++)
        {
            const Mesh& mesh = *(const Mesh*)pMesh;
            void* cmdsMemory = ::operator new(sizeof(GPUDriven::IndirectCommand) * mesh.numDraws * 2, std::align_val_t(16));
            GPUDriven::IndirectCommand* cmds = static_cast<GPUDriven::IndirectCommand*>(cmdsMemory);

            D3D12_GPU_VIRTUAL_ADDRESS MeshCBAddress = m_MeshConstantsGPU.GetGpuVirtualAddress() + sizeof(MeshConstants) * mesh.meshCBV;
            D3D12_GPU_VIRTUAL_ADDRESS MaterialCBAddress = m_Model->m_MaterialConstants.GetGpuVirtualAddress() + sizeof(MaterialConstants) * mesh.materialCBV;

            for (uint32_t j = 0; j < mesh.numDraws; j++)
            {
                GPUDriven::IndirectCommand& cmd = cmds[j];

                cmd.meshCBAddress = MeshCBAddress;
                cmd.materialCBAddress = MaterialCBAddress;
				cmd.vertexBufferView.BufferLocation = m_Model->m_DataBuffer.GetGpuVirtualAddress() + mesh.vbOffset;
                cmd.vertexBufferView.SizeInBytes = mesh.vbSize;
				cmd.vertexBufferView.StrideInBytes = mesh.vbStride;
				cmd.indexBufferView.BufferLocation = m_Model->m_DataBuffer.GetGpuVirtualAddress() + mesh.ibOffset;
                cmd.indexBufferView.SizeInBytes = mesh.ibSize;
                cmd.indexBufferView.Format = (DXGI_FORMAT)mesh.ibFormat;
                if (mesh.numJoints > 0)
                {
					cmd.meshJointsAddress = m_MeshJointsGPU.GetGpuVirtualAddress() + sizeof(Joint) * mesh.startJoint;
                }
                else
                {
					cmd.meshJointsAddress = D3D12_GPU_VIRTUAL_ADDRESS_NULL;
                }

                cmd.drawArguments.IndexCountPerInstance = mesh.draw[j].primCount;
                cmd.drawArguments.InstanceCount = 1;
                cmd.drawArguments.StartIndexLocation = mesh.draw[j].startIndex;
				cmd.drawArguments.BaseVertexLocation = mesh.draw[j].baseVertex;
				cmd.drawArguments.StartInstanceLocation = 0;

				bool alphaTest = (mesh.psoFlags & PSOFlags::kAlphaTest) == PSOFlags::kAlphaTest;
				uint32_t stride = alphaTest ? 16u : 12u;
				if (mesh.numJoints > 0)
					stride += 16;
                GPUDriven::IndirectCommand& cmdZPass = cmds[j+ mesh.numDraws];
                cmdZPass = cmd;
                cmdZPass.vertexBufferView.BufferLocation = m_Model->m_DataBuffer.GetGpuVirtualAddress() + mesh.vbDepthOffset;
                cmdZPass.vertexBufferView.SizeInBytes = mesh.vbDepthSize;
                cmdZPass.vertexBufferView.StrideInBytes = stride;

            }
            std::shared_ptr<IndirectArgsBuffer> argsBuffer = std::make_shared<IndirectArgsBuffer>();
			std::wstring name = L"Model Mesh ";
			name += std::to_wstring(i);
            argsBuffer->Create(name.c_str(), mesh.numDraws * 2, sizeof(GPUDriven::IndirectCommand), cmdsMemory);
            m_MeshIndirectArgsBuffers.push_back({ argsBuffer, mesh.numDraws});
            pMesh += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
            ::operator delete(cmdsMemory, std::align_val_t(16));
        }
    }
}

void ModelInstance::DestroyMeshIndirectCommands()
{
    for (size_t i = 0; i < m_MeshIndirectArgsBuffers.size(); i++)
    {
        m_MeshIndirectArgsBuffers[i].indirectArgsBuffer->Destroy();
    }

    m_MeshIndirectArgsBuffers.clear();
}