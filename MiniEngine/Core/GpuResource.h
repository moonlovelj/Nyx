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

class GpuResource
{
    friend class CommandContext;
    friend class GraphicsContext;
    friend class ComputeContext;

public:
    GpuResource() : 
        m_GpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS_NULL),
        m_UsageState(D3D12_RESOURCE_STATE_COMMON),
        m_TransitioningState((D3D12_RESOURCE_STATES)-1)
    {
    }

    GpuResource(ID3D12Resource* pResource, D3D12_RESOURCE_STATES CurrentState) :
        m_GpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS_NULL),
        m_pResource(pResource),
        m_UsageState(CurrentState),
        m_TransitioningState((D3D12_RESOURCE_STATES)-1)
    {
    }

    virtual ~GpuResource() { Destroy(); }

    virtual void Destroy()
    {
        m_pResource = nullptr;
        m_GpuVirtualAddress = D3D12_GPU_VIRTUAL_ADDRESS_NULL;
        ++m_VersionID;
    }

    ID3D12Resource* operator->() { return m_pResource.Get(); } 
    const ID3D12Resource* operator->() const { return m_pResource.Get(); }

    ID3D12Resource* GetResource() { return m_pResource.Get(); } 
    const ID3D12Resource* GetResource() const { return m_pResource.Get(); }

    ID3D12Resource** GetAddressOf() { return m_pResource.GetAddressOf(); }

    D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const { return m_GpuVirtualAddress; }

    uint32_t GetVersionID() const { return m_VersionID; }

    // RenderGraph uses these only to validate an imported resource before it
    // records any commands. CommandContext remains the sole state mutator.
    D3D12_RESOURCE_STATES GetUsageState() const { return m_UsageState; }
    bool HasPendingTransition() const
    {
        return m_TransitioningState != static_cast<D3D12_RESOURCE_STATES>(-1);
    }

protected:

    Microsoft::WRL::ComPtr<ID3D12Resource> m_pResource;
    D3D12_RESOURCE_STATES m_UsageState;
    D3D12_RESOURCE_STATES m_TransitioningState;
    D3D12_GPU_VIRTUAL_ADDRESS m_GpuVirtualAddress;

    // Used to identify when a resource changes so descriptors can be copied etc.
    uint32_t m_VersionID = 0;
};
