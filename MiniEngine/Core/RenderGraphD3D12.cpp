#include "pch.h"
#include "RenderGraphD3D12.h"

namespace RenderGraph
{
    std::optional<D3D12_RESOURCE_STATES> TryGetD3D12ResourceState(ResourceState state)
    {
        const uint32_t stageBits = static_cast<uint32_t>(state.Stages);
        const uint32_t validStageBits = static_cast<uint32_t>(ShaderStage::All);
        if ((stageBits & ~validStageBits) != 0)
            return std::nullopt;

        if (state.UsageType == Usage::ShaderResource)
        {
            const ShaderStage stages = state.Stages == ShaderStage::None
                                           ? ShaderStage::All
                                           : state.Stages;
            uint32_t nativeState = 0;
            if (HasAny(stages, ShaderStage::Pixel))
                nativeState |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (HasAny(stages, ShaderStage::Vertex | ShaderStage::Compute))
                nativeState |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            return static_cast<D3D12_RESOURCE_STATES>(nativeState);
        }

        if (state.UsageType != Usage::UnorderedAccess && state.Stages != ShaderStage::None)
            return std::nullopt;

        switch (state.UsageType)
        {
        case Usage::Undefined: return std::nullopt;
        case Usage::Common: return D3D12_RESOURCE_STATE_COMMON;
        case Usage::ShaderResource: break;
        case Usage::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case Usage::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case Usage::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
        case Usage::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case Usage::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case Usage::CopyDestination: return D3D12_RESOURCE_STATE_COPY_DEST;
        case Usage::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case Usage::Present: return D3D12_RESOURCE_STATE_PRESENT;
        }
        return std::nullopt;
    }

    bool IsD3D12ResourceStateCompatible(
        D3D12_RESOURCE_STATES actual,
        D3D12_RESOURCE_STATES declared)
    {
        if (actual == declared)
            return true;

        const uint32_t actualBits = static_cast<uint32_t>(actual);
        const uint32_t declaredBits = static_cast<uint32_t>(declared);
        const uint32_t readOnlyBits =
            static_cast<uint32_t>(D3D12_RESOURCE_STATE_GENERIC_READ) |
            static_cast<uint32_t>(D3D12_RESOURCE_STATE_DEPTH_READ);
        return declaredBits != 0 &&
               (declaredBits & ~readOnlyBits) == 0 &&
               (actualBits & ~readOnlyBits) == 0 &&
               (actualBits & declaredBits) == declaredBits;
    }
} // namespace RenderGraph
