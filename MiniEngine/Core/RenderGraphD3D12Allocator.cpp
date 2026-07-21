#include "pch.h"
#include "RenderGraphD3D12Allocator.h"

#include "ColorBuffer.h"
#include "CommandListManager.h"
#include "DepthBuffer.h"
#include "D3D12MemAlloc.h"
#include "GpuBuffer.h"
#include "GraphicsCore.h"
#include "PixelBuffer.h"

#include <deque>
#include <limits>
#include <sstream>

namespace RenderGraph::Detail
{
    namespace
    {
        constexpr uint64_t kFnvOffset = 14695981039346656037ull;
        constexpr uint64_t kFnvPrime = 1099511628211ull;
        constexpr size_t kCachedLayoutLimit = 8;

        template <typename T>
        void HashValue(uint64_t& hash, const T& value)
        {
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            for (size_t i = 0; i < sizeof(T); ++i)
            {
                hash ^= bytes[i];
                hash *= kFnvPrime;
            }
        }

        uint64_t PhysicalPlanHash(
            const GraphDefinition& definition,
            const CompiledPlan& plan)
        {
            uint64_t hash = kFnvOffset;
            HashValue(hash, definition.Resources.size());
            HashValue(hash, plan.MemorySlots.size());

            for (ResourceId id = 0; id < definition.Resources.size(); ++id)
            {
                const ResourceDef& resource = definition.Resources[id];
                if (resource.Imported || plan.Resources[id].LifetimeRange.IsEmpty())
                    continue;

                HashValue(hash, id);
                HashValue(hash, plan.Resources[id].MemorySlot);
                HashValue(hash, resource.Desc.Kind);
                if (resource.Desc.Kind == ResourceKind::Texture)
                {
                    const TextureDesc& desc = resource.Desc.Texture;
                    HashValue(hash, desc.Width);
                    HashValue(hash, desc.Height);
                    HashValue(hash, desc.DepthOrArraySize);
                    HashValue(hash, desc.MipLevels);
                    HashValue(hash, desc.SampleCount);
                    HashValue(hash, desc.Format);
                    HashValue(hash, desc.Flags);
                }
                else
                {
                    const BufferDesc& desc = resource.Desc.Buffer;
                    HashValue(hash, desc.SizeInBytes);
                    HashValue(hash, desc.StrideInBytes);
                    HashValue(hash, desc.Flags);
                }
            }
            return hash;
        }

        D3D12_RESOURCE_FLAGS GetNativeFlags(ResourceFlags flags)
        {
            D3D12_RESOURCE_FLAGS result = D3D12_RESOURCE_FLAG_NONE;
            if (HasAny(flags, ResourceFlags::AllowRenderTarget))
                result |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (HasAny(flags, ResourceFlags::AllowDepthStencil))
                result |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (HasAny(flags, ResourceFlags::AllowUnorderedAccess))
                result |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return result;
        }

        bool MakeNativeDesc(
            const ResourceDescriptor& graphDesc,
            D3D12_RESOURCE_DESC& nativeDesc,
            std::string& message)
        {
            nativeDesc = {};
            if (graphDesc.Kind == ResourceKind::Texture)
            {
                const TextureDesc& texture = graphDesc.Texture;
                const ResourceFlags flags = texture.Flags;
                if (texture.Format == DXGI_FORMAT_UNKNOWN)
                {
                    message = "Transient textures require a concrete DXGI format.";
                    return false;
                }
                if (HasAny(flags, ResourceFlags::AllowRenderTarget) &&
                    HasAny(flags, ResourceFlags::AllowDepthStencil))
                {
                    message = "A transient texture cannot be both a render target and a depth stencil.";
                    return false;
                }
                if (HasAny(flags, ResourceFlags::AllowDepthStencil) &&
                    (texture.DepthOrArraySize != 1 || texture.MipLevels != 1))
                {
                    message = "The current DepthBuffer wrapper supports one 2D slice and one mip.";
                    return false;
                }

                nativeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                nativeDesc.Width = texture.Width;
                nativeDesc.Height = texture.Height;
                nativeDesc.DepthOrArraySize = texture.DepthOrArraySize;
                nativeDesc.MipLevels = texture.MipLevels;
                nativeDesc.Format = PixelBuffer::GetBaseFormat(
                    static_cast<DXGI_FORMAT>(texture.Format));
                nativeDesc.SampleDesc.Count = texture.SampleCount;
                nativeDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                nativeDesc.Flags = GetNativeFlags(flags);
                return true;
            }

            const BufferDesc& buffer = graphDesc.Buffer;
            if (buffer.StrideInBytes != 0 &&
                buffer.SizeInBytes % buffer.StrideInBytes != 0)
            {
                message = "A structured transient buffer size must be divisible by its stride.";
                return false;
            }

            const uint64_t width = (buffer.SizeInBytes + 3u) & ~uint64_t{3u};
            if (width / (buffer.StrideInBytes == 0 ? 4u : buffer.StrideInBytes) >
                (std::numeric_limits<uint32_t>::max)())
            {
                message = "The transient buffer exceeds the current GpuBuffer element limit.";
                return false;
            }

            nativeDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            nativeDesc.Width = width;
            nativeDesc.Height = 1;
            nativeDesc.DepthOrArraySize = 1;
            nativeDesc.MipLevels = 1;
            nativeDesc.Format = DXGI_FORMAT_UNKNOWN;
            nativeDesc.SampleDesc.Count = 1;
            nativeDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            // MiniEngine's buffer wrappers always materialize SRV and UAV views.
            nativeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return true;
        }

        D3D12_HEAP_FLAGS GetHeapFlags(const D3D12_RESOURCE_DESC& desc)
        {
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                               D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0)
            {
                return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
            }
            return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        }

        std::wstring ToWide(std::string_view text)
        {
            if (text.empty())
                return {};
            const int size = MultiByteToWideChar(
                CP_UTF8,
                0,
                text.data(),
                static_cast<int>(text.size()),
                nullptr,
                0);
            if (size <= 0)
                return std::wstring(text.begin(), text.end());
            std::wstring result(size, L'\0');
            MultiByteToWideChar(
                CP_UTF8,
                0,
                text.data(),
                static_cast<int>(text.size()),
                result.data(),
                size);
            return result;
        }

        std::string HResultMessage(const char* operation, HRESULT result)
        {
            std::ostringstream stream;
            stream << operation << " failed with HRESULT 0x" << std::hex
                   << static_cast<uint32_t>(result) << ".";
            return stream.str();
        }

        using AllocationPtr = Microsoft::WRL::ComPtr<D3D12MA::Allocation>;
        using AllocatorPtr = Microsoft::WRL::ComPtr<D3D12MA::Allocator>;

        struct PreparedLayout
        {
            uint64_t Hash = 0;
            uint64_t Bytes = 0;
            std::vector<ResourceDescriptor> Descriptors;
            std::vector<MemorySlotId> MemorySlotByResource;
            // Blocks must outlive the resources placed in them.
            std::vector<AllocationPtr> Blocks;
            std::vector<std::unique_ptr<GpuResource>> Resources;
        };

        struct RetiredLayout
        {
            uint64_t Fence = 0;
            std::unique_ptr<PreparedLayout> Layout;
        };

        std::unique_ptr<GpuResource> AttachResource(
            const ResourceDef& definition,
            Microsoft::WRL::ComPtr<ID3D12Resource>& nativeResource)
        {
            const std::wstring name = ToWide(definition.Name);
            if (definition.Desc.Kind == ResourceKind::Texture)
            {
                const TextureDesc& desc = definition.Desc.Texture;
                const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(desc.Format);
                if (HasAny(desc.Flags, ResourceFlags::AllowDepthStencil))
                {
                    auto resource = std::make_unique<DepthBuffer>();
                    resource->CreateFromResource(name, nativeResource.Detach(), format);
                    return resource;
                }

                auto resource = std::make_unique<ColorBuffer>();
                resource->CreateFromResource(name, nativeResource.Detach(), format);
                return resource;
            }

            const BufferDesc& desc = definition.Desc.Buffer;
            const uint32_t elementSize = desc.StrideInBytes == 0 ? 4u : desc.StrideInBytes;
            const uint64_t alignedSize =
                (desc.SizeInBytes + elementSize - 1) / elementSize * elementSize;
            std::unique_ptr<GpuBuffer> resource;
            if (desc.StrideInBytes != 0)
                resource = std::make_unique<StructuredBuffer>();
            else
                resource = std::make_unique<ByteAddressBuffer>();

            resource->CreateFromResource(
                name,
                nativeResource.Detach(),
                static_cast<uint32_t>(alignedSize / elementSize),
                elementSize);
            return resource;
        }

        bool MatchesPhysicalPlan(
            const GraphDefinition& definition,
            const CompiledPlan& plan,
            const PreparedLayout& layout)
        {
            if (layout.Resources.size() != definition.Resources.size() ||
                layout.Descriptors.size() != definition.Resources.size() ||
                layout.MemorySlotByResource.size() != definition.Resources.size() ||
                layout.Blocks.size() != plan.MemorySlots.size())
            {
                return false;
            }

            for (ResourceId id = 0; id < definition.Resources.size(); ++id)
            {
                const bool required = !definition.Resources[id].Imported &&
                                      !plan.Resources[id].LifetimeRange.IsEmpty();
                if (!required)
                    continue;
                if (!layout.Resources[id] ||
                    !(layout.Descriptors[id] == definition.Resources[id].Desc) ||
                    layout.MemorySlotByResource[id] != plan.Resources[id].MemorySlot)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    struct D3D12TransientAllocator::Impl
    {
        bool EnsureAllocator(std::string& message)
        {
            if (Allocator)
                return true;
            if (Graphics::g_Device == nullptr)
            {
                message = "The D3D12 device is not initialized.";
                return false;
            }

            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (FAILED(result))
            {
                message = HResultMessage("CreateDXGIFactory1", result);
                return false;
            }
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            result = factory->EnumAdapterByLuid(
                Graphics::g_Device->GetAdapterLuid(),
                IID_PPV_ARGS(&adapter));
            if (FAILED(result))
            {
                message = HResultMessage("EnumAdapterByLuid", result);
                return false;
            }

            D3D12MA::ALLOCATOR_DESC desc = {};
            desc.pDevice = Graphics::g_Device;
            desc.pAdapter = adapter.Get();
            result = D3D12MA::CreateAllocator(&desc, Allocator.GetAddressOf());
            if (FAILED(result))
            {
                message = HResultMessage("D3D12MA::CreateAllocator", result);
                return false;
            }
            return true;
        }

        void DestroyLayout(std::unique_ptr<PreparedLayout>& layout)
        {
            if (layout)
            {
                Stats.AllocatedBytes -= layout->Bytes;
                layout.reset();
            }
        }

        void Collect()
        {
            while (!Pending.empty() &&
                   Graphics::g_CommandManager.IsFenceComplete(Pending.front().Fence))
            {
                Available.push_back(std::move(Pending.front().Layout));
                Pending.pop_front();
            }

            while (Available.size() > kCachedLayoutLimit)
            {
                DestroyLayout(Available.front());
                Available.erase(Available.begin());
            }
        }

        bool BuildLayout(
            const GraphDefinition& definition,
            const CompiledPlan& plan,
            uint64_t hash,
            std::unique_ptr<PreparedLayout>& layout,
            std::string& message)
        {
            auto candidate = std::make_unique<PreparedLayout>();
            candidate->Hash = hash;
            candidate->Descriptors.resize(definition.Resources.size());
            candidate->MemorySlotByResource.resize(
                definition.Resources.size(),
                kInvalidIndex);
            candidate->Blocks.reserve(plan.MemorySlots.size());
            candidate->Resources.resize(definition.Resources.size());

            std::vector<D3D12_RESOURCE_DESC> nativeDescs(definition.Resources.size());
            for (ResourceId id = 0; id < definition.Resources.size(); ++id)
            {
                if (definition.Resources[id].Imported ||
                    plan.Resources[id].LifetimeRange.IsEmpty())
                {
                    continue;
                }
                if (!MakeNativeDesc(definition.Resources[id].Desc, nativeDescs[id], message))
                {
                    message = "Resource '" + definition.Resources[id].Name + "': " + message;
                    return false;
                }
                candidate->Descriptors[id] = definition.Resources[id].Desc;
                candidate->MemorySlotByResource[id] = plan.Resources[id].MemorySlot;
            }

            for (MemorySlotId slotId = 0; slotId < plan.MemorySlots.size(); ++slotId)
            {
                const MemorySlotPlan& slot = plan.MemorySlots[slotId];
                D3D12_RESOURCE_ALLOCATION_INFO slotInfo = {};
                D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
                for (const MemoryUse& use : slot.Uses)
                {
                    const D3D12_RESOURCE_DESC& desc = nativeDescs[use.Resource];
                    const D3D12_RESOURCE_ALLOCATION_INFO info =
                        Graphics::g_Device->GetResourceAllocationInfo(0, 1, &desc);
                    if (info.SizeInBytes == UINT64_MAX)
                    {
                        message = "GetResourceAllocationInfo rejected resource '" +
                                  definition.Resources[use.Resource].Name + "'.";
                        return false;
                    }

                    const D3D12_HEAP_FLAGS resourceHeapFlags = GetHeapFlags(desc);
                    if (heapFlags == D3D12_HEAP_FLAG_NONE)
                        heapFlags = resourceHeapFlags;
                    else if (heapFlags != resourceHeapFlags)
                    {
                        message = "Memory slot " + std::to_string(slotId) +
                                  " mixes incompatible D3D12 heap categories.";
                        return false;
                    }

                    slotInfo.Alignment = (std::max)(slotInfo.Alignment, info.Alignment);
                    slotInfo.SizeInBytes = (std::max)(slotInfo.SizeInBytes, info.SizeInBytes);
                }
                slotInfo.SizeInBytes =
                    (slotInfo.SizeInBytes + slotInfo.Alignment - 1) & ~(slotInfo.Alignment - 1);

                D3D12MA::ALLOCATION_DESC allocationDesc = {};
                allocationDesc.Flags = D3D12MA::ALLOCATION_FLAG_CAN_ALIAS;
                allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
                allocationDesc.ExtraHeapFlags = heapFlags;
                AllocationPtr allocation;
                const HRESULT allocationResult = Allocator->AllocateMemory(
                    &allocationDesc,
                    &slotInfo,
                    allocation.GetAddressOf());
                if (FAILED(allocationResult))
                {
                    message = HResultMessage("D3D12MA::AllocateMemory", allocationResult);
                    return false;
                }
                for (const MemoryUse& use : slot.Uses)
                {
                    const ResourceDef& resourceDef = definition.Resources[use.Resource];
                    const D3D12_RESOURCE_DESC& desc = nativeDescs[use.Resource];
                    D3D12_CLEAR_VALUE clearValue = {};
                    const D3D12_CLEAR_VALUE* optimizedClear = nullptr;
                    if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0)
                    {
                        clearValue.Format = static_cast<DXGI_FORMAT>(resourceDef.Desc.Texture.Format);
                        optimizedClear = &clearValue;
                    }

                    Microsoft::WRL::ComPtr<ID3D12Resource> nativeResource;
                    const HRESULT resourceResult = Allocator->CreateAliasingResource(
                        allocation.Get(),
                        0,
                        &desc,
                        D3D12_RESOURCE_STATE_COMMON,
                        optimizedClear,
                        IID_PPV_ARGS(&nativeResource));
                    if (FAILED(resourceResult))
                    {
                        message = "Resource '" + resourceDef.Name + "': " +
                                  HResultMessage("D3D12MA::CreateAliasingResource", resourceResult);
                        return false;
                    }

                    candidate->Resources[use.Resource] =
                        AttachResource(resourceDef, nativeResource);
                }

                candidate->Bytes += allocation->GetSize();
                candidate->Blocks.push_back(std::move(allocation));
            }

            layout = std::move(candidate);
            return true;
        }

        AllocatorPtr Allocator;
        std::unique_ptr<PreparedLayout> Active;
        std::vector<std::unique_ptr<PreparedLayout>> Available;
        std::deque<RetiredLayout> Pending;
        TransientMemoryStats Stats;
    };

    D3D12TransientAllocator::D3D12TransientAllocator()
        : m_Impl(std::make_unique<Impl>())
    {
    }

    D3D12TransientAllocator::~D3D12TransientAllocator()
    {
        Shutdown();
    }

    bool D3D12TransientAllocator::Prepare(
        const GraphDefinition& definition,
        const CompiledPlan& plan,
        std::string& message)
    {
        if (m_Impl->Active)
        {
            message = "Transient allocation is already active; nested GPU graph execution is not supported.";
            return false;
        }
        if (plan.MemorySlots.empty())
            return true;
        if (!m_Impl->EnsureAllocator(message))
            return false;

        m_Impl->Collect();
        const uint64_t hash = PhysicalPlanHash(definition, plan);
        const auto reusable = std::find_if(
            m_Impl->Available.rbegin(),
            m_Impl->Available.rend(),
            [&](const std::unique_ptr<PreparedLayout>& layout)
            {
                return layout->Hash == hash &&
                       MatchesPhysicalPlan(definition, plan, *layout);
            });
        if (reusable != m_Impl->Available.rend())
        {
            const size_t index = static_cast<size_t>(
                std::distance(reusable, m_Impl->Available.rend()) - 1);
            m_Impl->Active = std::move(m_Impl->Available[index]);
            m_Impl->Available.erase(m_Impl->Available.begin() + index);
            ++m_Impl->Stats.LayoutReuseCount;
            return true;
        }

        if (!m_Impl->BuildLayout(definition, plan, hash, m_Impl->Active, message))
            return false;
        m_Impl->Stats.AllocatedBytes += m_Impl->Active->Bytes;
        m_Impl->Stats.PeakAllocatedBytes = (std::max)(
            m_Impl->Stats.PeakAllocatedBytes,
            m_Impl->Stats.AllocatedBytes);
        ++m_Impl->Stats.LayoutCreationCount;
        return true;
    }

    GpuResource* D3D12TransientAllocator::GetResource(ResourceId resource) const
    {
        return m_Impl->Active && resource < m_Impl->Active->Resources.size()
                   ? m_Impl->Active->Resources[resource].get()
                   : nullptr;
    }

    void D3D12TransientAllocator::Retire()
    {
        if (!m_Impl->Active)
            return;
        const uint64_t fence =
            Graphics::g_CommandManager.GetGraphicsQueue().GetNextFenceValue();
        m_Impl->Pending.push_back({fence, std::move(m_Impl->Active)});
    }

    void D3D12TransientAllocator::Trim()
    {
        m_Impl->Collect();
        for (auto& layout : m_Impl->Available)
            m_Impl->DestroyLayout(layout);
        m_Impl->Available.clear();
    }

    void D3D12TransientAllocator::Shutdown()
    {
        m_Impl->Active.reset();
        m_Impl->Available.clear();
        m_Impl->Pending.clear();
        m_Impl->Stats = {};
        m_Impl->Allocator.Reset();
    }

    TransientMemoryStats D3D12TransientAllocator::GetStats() const
    {
        TransientMemoryStats stats = m_Impl->Stats;
        stats.CachedLayoutCount = static_cast<uint32_t>(m_Impl->Available.size());
        stats.PendingLayoutCount = static_cast<uint32_t>(m_Impl->Pending.size());
        return stats;
    }

    D3D12TransientAllocator& GetD3D12TransientAllocator()
    {
        static D3D12TransientAllocator allocator;
        return allocator;
    }

    void ShutdownD3D12TransientAllocator()
    {
        GetD3D12TransientAllocator().Shutdown();
    }
} // namespace RenderGraph::Detail

namespace RenderGraph
{
    TransientMemoryStats GetTransientMemoryStats()
    {
        return Detail::GetD3D12TransientAllocator().GetStats();
    }

    void TrimTransientMemory()
    {
        Detail::GetD3D12TransientAllocator().Trim();
    }
} // namespace RenderGraph
