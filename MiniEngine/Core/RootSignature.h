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

#include "pch.h"

class DescriptorCache;

class RootParameter
{
    friend class RootSignature;
public:

    RootParameter() 
    {
        m_RootParam.ParameterType = (D3D12_ROOT_PARAMETER_TYPE)0xFFFFFFFF;
    }

    ~RootParameter()
    {
        Clear();
    }

    void Clear()
    {
        if (m_RootParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            delete [] m_RootParam.DescriptorTable.pDescriptorRanges;

        m_RootParam.ParameterType = (D3D12_ROOT_PARAMETER_TYPE)0xFFFFFFFF;
    }

    void InitAsConstants( UINT Register, UINT NumDwords, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0 )
    {
        m_RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        m_RootParam.ShaderVisibility = Visibility;
        m_RootParam.Constants.Num32BitValues = NumDwords;
        m_RootParam.Constants.ShaderRegister = Register;
        m_RootParam.Constants.RegisterSpace = Space;
    }

    void InitAsConstantBuffer( UINT Register, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0 )
    {
        m_RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        m_RootParam.ShaderVisibility = Visibility;
        m_RootParam.Descriptor.ShaderRegister = Register;
        m_RootParam.Descriptor.RegisterSpace = Space;
        m_RootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    }

    void InitAsBufferSRV( UINT Register, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0 )
    {
        m_RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        m_RootParam.ShaderVisibility = Visibility;
        m_RootParam.Descriptor.ShaderRegister = Register;
        m_RootParam.Descriptor.RegisterSpace = Space;
        m_RootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    }

    void InitAsBufferUAV( UINT Register, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0 )
    {
        m_RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        m_RootParam.ShaderVisibility = Visibility;
        m_RootParam.Descriptor.ShaderRegister = Register;
        m_RootParam.Descriptor.RegisterSpace = Space;
        m_RootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    }

    void InitAsDescriptorRange( D3D12_DESCRIPTOR_RANGE_TYPE Type, UINT Register, UINT Count, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0 )
    {
        InitAsDescriptorTable(1, Visibility);
        SetTableRange(0, Type, Register, Count, Space);
    }

    void InitAsDescriptorTable( UINT RangeCount, D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL )
    {
        m_RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        m_RootParam.ShaderVisibility = Visibility;
        m_RootParam.DescriptorTable.NumDescriptorRanges = RangeCount;
        m_RootParam.DescriptorTable.pDescriptorRanges = new D3D12_DESCRIPTOR_RANGE1[RangeCount];
    }

    void SetTableRange( UINT RangeIndex, D3D12_DESCRIPTOR_RANGE_TYPE Type, UINT Register, UINT Count, UINT Space = 0 )
    {
        D3D12_DESCRIPTOR_RANGE1* range = const_cast<D3D12_DESCRIPTOR_RANGE1*>(m_RootParam.DescriptorTable.pDescriptorRanges + RangeIndex);
        range->RangeType = Type;
        range->NumDescriptors = Count;
        range->BaseShaderRegister = Register;
        range->RegisterSpace = Space;
        range->OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE_FLAGS Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
		switch (Type)
		{
		case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
		case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
		case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
            Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
			break;
		default:
			break;
		}
		range->Flags = Flags;
    }

    const D3D12_ROOT_PARAMETER1& operator() ( void ) const { return m_RootParam; }
        

protected:

    D3D12_ROOT_PARAMETER1 m_RootParam;
};

// Maximum 64 DWORDS divied up amongst all root parameters.
// Root constants = 1 DWORD * NumConstants
// Root descriptor (CBV, SRV, or UAV) = 2 DWORDs each
// Descriptor table pointer = 1 DWORD
// Static samplers = 0 DWORDS (compiled into shader)
class RootSignature
{
    friend class DynamicDescriptorHeap;

public:

    RootSignature( UINT NumRootParams = 0, UINT NumStaticSamplers = 0 ) : m_Finalized(FALSE), m_NumParameters(NumRootParams)
    {
        Reset(NumRootParams, NumStaticSamplers);
    }

    ~RootSignature()
    {
    }

    static void DestroyAll(void);

    void Reset( UINT NumRootParams, UINT NumStaticSamplers = 0 )
    {
        if (NumRootParams > 0)
            m_ParamArray.reset(new RootParameter[NumRootParams]);
        else
            m_ParamArray = nullptr;
        m_NumParameters = NumRootParams;

        if (NumStaticSamplers > 0)
            m_SamplerArray.reset(new D3D12_STATIC_SAMPLER_DESC[NumStaticSamplers]);
        else
            m_SamplerArray = nullptr;
        m_NumSamplers = NumStaticSamplers;
        m_NumInitializedStaticSamplers = 0;
    }

    RootParameter& operator[] ( size_t EntryIndex )
    {
        ASSERT(EntryIndex < m_NumParameters);
        return m_ParamArray.get()[EntryIndex];
    }

    const RootParameter& operator[] ( size_t EntryIndex ) const
    {
        ASSERT(EntryIndex < m_NumParameters);
        return m_ParamArray.get()[EntryIndex];
    }

    void InitStaticSampler( UINT Register, const D3D12_SAMPLER_DESC& NonStaticSamplerDesc,
        D3D12_SHADER_VISIBILITY Visibility = D3D12_SHADER_VISIBILITY_ALL, UINT Space = 0 );

    void Finalize(const std::wstring& name, D3D12_ROOT_SIGNATURE_FLAGS Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ID3D12RootSignature* GetSignature() const { return m_Signature; }

protected:

    BOOL m_Finalized;
    UINT m_NumParameters;
    UINT m_NumSamplers;
    UINT m_NumInitializedStaticSamplers;
    uint32_t m_DescriptorTableBitMap;		// One bit is set for root parameters that are non-sampler descriptor tables
    uint32_t m_SamplerTableBitMap;			// One bit is set for root parameters that are sampler descriptor tables
    uint32_t m_DescriptorTableSize[16];		// Non-sampler descriptor tables need to know their descriptor count
    std::unique_ptr<RootParameter[]> m_ParamArray;
    std::unique_ptr<D3D12_STATIC_SAMPLER_DESC[]> m_SamplerArray;
    ID3D12RootSignature* m_Signature;
};
