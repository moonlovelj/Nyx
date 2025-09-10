#include "pch.h"
#include "IBL.h"
#include "BufferManager.h"
#include "CompiledShaders/IBLGenerateCubeMapCS.h"
#include "CompiledShaders/IBLDiffuseLDMapCS.h"
#include "CompiledShaders/IBLSpecularLDMapCS.h"
#include "CompiledShaders/IBLLutCS.h"

namespace IBL
{
    const uint32_t g_IBLCubeMapSize = 1024;
    const uint32_t g_IBLDiffuseLDMapSize = 128;
    const uint32_t g_IBLSpecularLDMapSize = 512;
    const uint32_t g_IBLLutSize = 1024;

    ComputePSO m_IBLGenerateCubeMapPSO(L"IBL Generate Cube Map CS");
    ComputePSO m_IBLDiffuseLDMapPSO(L"IBL Diffuse LD Map CS");
    ComputePSO m_IBLSpecularLDMapPSO(L"IBL Specular LD Map CS");
    ComputePSO m_IBLLutPSO(L"IBL Lut CS");

    TextureRef m_IBLHDRI;

    bool m_bIsPrecomputed = false;
    bool m_bInited = false;
}

void IBL::InitializeResources(TextureRef IBLHDRI)
{
    m_bInited = true;

#define CreatePSO( ObjName, ShaderByteCode ) \
    ObjName.SetRootSignature(Graphics::g_CommonRS); \
    ObjName.SetComputeShader(ShaderByteCode, sizeof(ShaderByteCode) ); \
    ObjName.Finalize();

    CreatePSO(m_IBLGenerateCubeMapPSO, g_pIBLGenerateCubeMapCS);
    CreatePSO(m_IBLDiffuseLDMapPSO, g_pIBLDiffuseLDMapCS);
    CreatePSO(m_IBLSpecularLDMapPSO, g_pIBLSpecularLDMapCS);
    CreatePSO(m_IBLLutPSO, g_pIBLLutCS);
#undef CreatePSO

    ChangeIBL(IBLHDRI);
}

bool IBL::IsValid()
{
    return m_bInited && m_IBLHDRI.IsValid();
}

void IBL::ChangeIBL(TextureRef IBLHDRI)
{
    if (!m_bInited)
    {
        return;
    }

    m_IBLHDRI = IBLHDRI;
    m_bIsPrecomputed = false;
}

void IBL::Shutdown( void )
{
    if (m_IBLHDRI.IsValid())
    {
        m_IBLHDRI = nullptr;
    }
}

void IBL::Precompute(GraphicsContext& gfxContext)
{
    if (m_bIsPrecomputed || !m_IBLHDRI.IsValid())
    {
        return;
    }

    m_bIsPrecomputed = true;

    {
        ScopedTimer _prof(L"IBL Generate Cube Map", gfxContext);
		ComputeContext& Context = gfxContext.GetComputeContext();
		Context.SetRootSignature(Graphics::g_CommonRS);
		Context.SetPipelineState(m_IBLGenerateCubeMapPSO);

		Context.TransitionResource(Graphics::g_IBLCubeMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		Context.SetConstants(0, g_IBLCubeMapSize);
		Context.SetDynamicDescriptor(1, 0, m_IBLHDRI.GetSRV());
		Context.SetDynamicDescriptor(2, 0, Graphics::g_IBLCubeMap.GetUAV());

		uint32_t groupCountX = Math::DivideByMultiple(g_IBLCubeMapSize, 8);
		uint32_t groupCountY = Math::DivideByMultiple(g_IBLCubeMapSize, 8);

		Context.Dispatch(groupCountX, groupCountY, 6);
    }

	{
		ScopedTimer _prof(L"IBL Cube Map GenerateMipMaps", gfxContext);
        ComputeContext& Context = gfxContext.GetComputeContext();
        Graphics::g_IBLCubeMap.GenerateMipMaps(Context);
        Context.TransitionResource(Graphics::g_IBLCubeMap, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }

    const uint32_t HDRITextureSize = Graphics::g_IBLCubeMap.GetWidth();
    const uint32_t HDRIMipCount = Math::Log2(HDRITextureSize) + 1;
    {
        ScopedTimer _prof(L"Precompute Diffuse LD Map", gfxContext);

        ComputeContext& Context = gfxContext.GetComputeContext();
        Context.SetRootSignature(Graphics::g_CommonRS);
        Context.SetPipelineState(m_IBLDiffuseLDMapPSO);

        Context.TransitionResource(Graphics::g_IBLDiffuseLDMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        Context.SetConstants(0, HDRITextureSize, HDRIMipCount, g_IBLDiffuseLDMapSize);
        Context.SetDynamicDescriptor(1, 0, Graphics::g_IBLCubeMap.GetSRV());
        Context.SetDynamicDescriptor(2, 0, Graphics::g_IBLDiffuseLDMap.GetUAV());

        uint32_t groupCountX = Math::DivideByMultiple(g_IBLDiffuseLDMapSize, 8);
        uint32_t groupCountY = Math::DivideByMultiple(g_IBLDiffuseLDMapSize, 8);

        Context.Dispatch(groupCountX, groupCountY, 6);
    }

    {
        ScopedTimer _prof(L"Precompute Specular LD Map", gfxContext);

        ComputeContext& Context = gfxContext.GetComputeContext();
        Context.SetRootSignature(Graphics::g_CommonRS);
        Context.SetPipelineState(m_IBLSpecularLDMapPSO);

        Context.TransitionResource(Graphics::g_IBLSpecularLDMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const uint32_t SpecularLDMapMipLevel = Math::Log2(g_IBLSpecularLDMapSize) + 1;
        uint32_t BaseDivisor = 1;
        for (uint32_t i = 0; i < SpecularLDMapMipLevel; ++i)
        {
            uint32_t SpecularLDMapMipSize = g_IBLSpecularLDMapSize / BaseDivisor;
            float Roughness = (float)i / (SpecularLDMapMipLevel - 1);
            Roughness *= Roughness;
            //Context.SetConstants(0, HDRITextureSize, HDRIMipCount, SpecularLDMapMipSize, Roughness);
            Context.SetConstants(0, HDRITextureSize, HDRIMipCount, SpecularLDMapMipLevel, i);
            Context.SetDynamicDescriptor(1, 0, Graphics::g_IBLCubeMap.GetSRV());
            Context.SetDynamicDescriptor(2, 0, Graphics::g_IBLSpecularLDMap.GetUAV(i));

            uint32_t groupCountX = Math::DivideByMultiple(SpecularLDMapMipSize, 8);
            uint32_t groupCountY = Math::DivideByMultiple(SpecularLDMapMipSize, 8);

            Context.Dispatch(groupCountX, groupCountY, 6);

            BaseDivisor *= 2;
        }
    }

    {
        ScopedTimer _prof(L"Precompute IBL Lut", gfxContext);

        ComputeContext& Context = gfxContext.GetComputeContext();
        Context.SetRootSignature(Graphics::g_CommonRS);
        Context.SetPipelineState(m_IBLLutPSO);

        Context.TransitionResource(Graphics::g_IBLLut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        Context.SetConstants(0, g_IBLLutSize);
        Context.SetDynamicDescriptor(2, 0, Graphics::g_IBLLut.GetUAV());

        uint32_t groupCountX = Math::DivideByMultiple(IBL::g_IBLLutSize, 8);
        uint32_t groupCountY = Math::DivideByMultiple(IBL::g_IBLLutSize, 8);

        Context.Dispatch(groupCountX, groupCountY, 1);
    }

    //gfxContext.FlushResourceBarriers();
}
