#pragma once

#include "RenderGraphD3D12.h"
#include "RenderGraphInternal.h"

#include <memory>
#include <string>

class GpuResource;

namespace RenderGraph::Detail
{
    // The sole owner of transient D3D12 memory. It caches complete physical
    // layouts, keeps in-flight layouts alive by fence, and resolves ResourceId
    // directly to distinct placed resources.
    class D3D12TransientAllocator
    {
      public:
        D3D12TransientAllocator();
        ~D3D12TransientAllocator();

        D3D12TransientAllocator(const D3D12TransientAllocator&) = delete;
        D3D12TransientAllocator& operator=(const D3D12TransientAllocator&) = delete;

        bool Prepare(
            const GraphDefinition& definition,
            const CompiledPlan& plan,
            std::string& message);

        GpuResource* GetResource(ResourceId resource) const;

        // The next direct-queue fence owns the current layout after commands have
        // been recorded. A later Prepare recycles it only after that fence passes.
        void Retire();
        void Trim();
        void Shutdown();

        TransientMemoryStats GetStats() const;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    D3D12TransientAllocator& GetD3D12TransientAllocator();
    void ShutdownD3D12TransientAllocator();
} // namespace RenderGraph::Detail
