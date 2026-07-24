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

#include "../Core/GpuBuffer.h"
#include "../Core/VectorMath.h"
#include "../Core/Camera.h"
#include "../Core/CommandContext.h"
#include "../Core/UploadBuffer.h"
#include "../Core/TextureManager.h"
#include "../Core/HierarchicalDepthBuffer.h"
#include "CommandBucketer.h"
#include "Shaders/BindlessIndices.h.slang"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>

class GraphicsPSO;
class RootSignature;
class DescriptorHeap;
class ShadowCamera;
class ShadowBuffer;
class Program;
struct Mesh;
struct Joint;

namespace Renderer
{
    using namespace Math;

    struct ViewConstants
    {
        ViewConstants()
            : ViewMatrix(kIdentity)
            , ProjMatrix(kIdentity)
            , ViewProjMatrix(kIdentity)
            , InverseViewProjMatrix(kIdentity)
            , PrevViewMatrix(kIdentity)
            , PrevProjMatrix(kIdentity)
            , PrevViewProjMatrix(kIdentity)
            , ViewerPos(kZero)
            , PrevViewerPos(kZero)
            , HZBSizeAndInv(kZero)
            , Projection(ProjectionType::Perspective)
            , ViewportWidth(0)
            , ViewportHeight(0)
            , InvViewportWidth(0.0f)
            , InvViewportHeight(0.0f)
            , LodScale(0.0f)
        {
        }

        Matrix4 ViewMatrix;
        Matrix4 ProjMatrix;
        Matrix4 ViewProjMatrix;
        Matrix4 InverseViewProjMatrix;

        Matrix4 PrevViewMatrix;
        Matrix4 PrevProjMatrix;
        Matrix4 PrevViewProjMatrix;

        Vector3 ViewerPos;
        Vector3 PrevViewerPos;
        Vector4 HZBSizeAndInv;

        ProjectionType Projection;
        uint32_t ViewportWidth;
        uint32_t ViewportHeight;
        float InvViewportWidth;
        float InvViewportHeight;
        float LodScale;
    };

    struct FrameConstants
    {
        FrameConstants()
            : SunShadowMatrix(kIdentity)
            , SunDirection(kZero)
            , SunIntensity(kZero)
            , ShadowTexelSize(kZero)
            , InvTileDim(kZero)
            , TileCount{}
            , FirstLightIndex{}
            , FrameIndexMod2(0)
            , IBLLutTextureSize(0)
            , IBLSpecularLDMapMipCount(0)
            , BindlessResourcesBaseIndex(0)
        {
        }

        Matrix4 SunShadowMatrix;
        Vector3 SunDirection;
        Vector3 SunIntensity;
        Vector4 ShadowTexelSize;
        Vector4 InvTileDim;

        uint32_t TileCount[4];
        uint32_t FirstLightIndex[4];
        uint32_t FrameIndexMod2;
        uint32_t IBLLutTextureSize;
        uint32_t IBLSpecularLDMapMipCount;
        uint32_t BindlessResourcesBaseIndex;
    };

    class RenderView
    {
    public:
        RenderView()
            : m_Camera(nullptr)
            , m_Viewport{}
            , m_Scissor{}
        {
        }

        void SetCamera(const BaseCamera& camera)
        {
            m_Camera = &camera;

            m_Constants.ViewMatrix = camera.GetViewMatrix();
            m_Constants.ProjMatrix = camera.GetProjMatrix();
            m_Constants.ViewProjMatrix = camera.GetViewProjMatrix();
            m_Constants.InverseViewProjMatrix = Invert(camera.GetViewProjMatrix());
            m_Constants.ViewerPos = camera.GetPosition();
            m_Constants.Projection = camera.GetProjectionType();

            m_Constants.PrevViewMatrix = m_Constants.ViewMatrix;
            m_Constants.PrevProjMatrix = m_Constants.ProjMatrix;
            m_Constants.PrevViewProjMatrix = m_Constants.ViewProjMatrix;
            m_Constants.PrevViewerPos = m_Constants.ViewerPos;

            UpdateLodScale();
        }

        void SetPreviousCamera(const BaseCamera& camera)
        {
            m_Constants.PrevViewMatrix = camera.GetViewMatrix();
            m_Constants.PrevProjMatrix = camera.GetProjMatrix();
            m_Constants.PrevViewProjMatrix = camera.GetViewProjMatrix();
            m_Constants.PrevViewerPos = camera.GetPosition();
        }

        void SetViewport(const D3D12_VIEWPORT& viewport)
        {
            m_Viewport = viewport;
            m_Constants.ViewportWidth = static_cast<uint32_t>(viewport.Width);
            m_Constants.ViewportHeight = static_cast<uint32_t>(viewport.Height);
            m_Constants.InvViewportWidth = viewport.Width > 0.0f ? 1.0f / viewport.Width : 0.0f;
            m_Constants.InvViewportHeight = viewport.Height > 0.0f ? 1.0f / viewport.Height : 0.0f;

            UpdateLodScale();
        }

        void SetScissor(const D3D12_RECT& scissor)
        {
            m_Scissor = scissor;
        }

        void SetHZBSize(uint32_t width, uint32_t height)
        {
            const float widthF = static_cast<float>(width);
            const float heightF = static_cast<float>(height);
            m_Constants.HZBSizeAndInv = Vector4(
                widthF,
                heightF,
                width > 0 ? 1.0f / widthF : 0.0f,
                height > 0 ? 1.0f / heightF : 0.0f);
        }

        void SetHZBSize(const HierarchicalDepthBuffer& hzb)
        {
            SetHZBSize(hzb.GetWidth(), hzb.GetHeight());
        }

        const ViewConstants& GetConstants() const
        {
            return m_Constants;
        }

        const BaseCamera* GetCamera() const
        {
            return m_Camera;
        }

        const D3D12_VIEWPORT& GetViewport() const
        {
            return m_Viewport;
        }

        const D3D12_RECT& GetScissor() const
        {
            return m_Scissor;
        }

    private:
        void UpdateLodScale()
        {
            const float scaleX = std::fabs(static_cast<float>(m_Constants.ProjMatrix.GetX().GetX()));
            const float scaleY = std::fabs(static_cast<float>(m_Constants.ProjMatrix.GetY().GetY()));

            if (scaleX > 0.0f && scaleY > 0.0f && m_Viewport.Width > 0.0f && m_Viewport.Height > 0.0f)
            {
                const float lodScaleX = 2.0f / (scaleX * m_Viewport.Width);
                const float lodScaleY = 2.0f / (scaleY * m_Viewport.Height);

                m_Constants.LodScale = std::min(lodScaleX, lodScaleY);
            }
            else
            {
                m_Constants.LodScale = 0.0f;
            }
        }

        ViewConstants m_Constants;
        const BaseCamera* m_Camera;
        D3D12_VIEWPORT m_Viewport;
        D3D12_RECT m_Scissor;
    };

    //extern RootSignature m_RootSig;
    extern DescriptorHeap s_TextureHeap;
    extern DescriptorHeap s_SamplerHeap;
    extern DescriptorHandle m_BindlessResources;

	extern float s_SpecularIBLRange;
	extern float s_SpecularIBLBias;

	struct DispatchMeshCommand
	{
		D3D12_DISPATCH_MESH_ARGUMENTS dispatchMeshArguments;
	};

	extern CommandSignature GPUDrivenDrawIndirectCommandSignature;

	inline UINT AlignForUavCounter(UINT bufferSize)
	{
		const UINT alignment = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
		return (bufferSize + (alignment - 1)) & ~(alignment - 1);
	}

    void Initialize(void);
    void Shutdown(void);

    std::string GetModelShaderPath(const char* shaderFileName);

    void SetIBLTextures();
    void SetIBLBias(float LODBias);
    void UpdateGlobalDescriptors(void);
    void DrawSkybox(GraphicsContext& gfxContext,
        const RenderView& view,
        const FrameConstants& frame);

    void InstanceCull(
        GraphicsContext& gfxContext,
        const RenderView& view,
        const FrameConstants& frame,
        const Program* program,
        const ComputePSO& pso,
        bool disableHZBCull = false);
    void DAGCull(
        GraphicsContext& gfxContext,
        const RenderView& view,
        const FrameConstants& frame,
        const Program* program,
        const ComputePSO& pso,
        bool disableHZBCull = false);

    uint32_t GetBindlessResourcesBaseOffset();
	void SetBindlessResourceDescriptor(uint32_t bindlessIndex, const D3D12_CPU_DESCRIPTOR_HANDLE& handle);

    void ExportDepth(
        GraphicsContext& gfxContext,
        const RenderView& view,
        const FrameConstants& frame);
    void ResolveVBufferToGBuffer(
        GraphicsContext& gfxContext,
        const RenderView& view,
        const FrameConstants& frame);

    void RenderVisibility(
        GraphicsContext& gfxContext,
        const RenderView& view,
        const FrameConstants& frame);

	HierarchicalDepthBuffer& GetPrevHZB();
	HierarchicalDepthBuffer& GetCurrentHZB();

    void RenderSceneDepth(
        GraphicsContext& gfxContext,
        const RenderView& view,
        const FrameConstants& frame,
        DepthBuffer& depthTarget);
} // namespace Renderer
