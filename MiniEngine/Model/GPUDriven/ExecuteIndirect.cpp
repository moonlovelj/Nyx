#include "ExecuteIndirect.h"

#include "../Core/CommandContext.h"

namespace GPUDriven
{
	CommandSignature GPUDrivenDrawIndirectCommandSignature;

	void Initialize(const RootSignature* RootSignature)
	{
		GPUDrivenDrawIndirectCommandSignature.Reset(2);
		GPUDrivenDrawIndirectCommandSignature[0].Constant(4, 0, 2);
		GPUDrivenDrawIndirectCommandSignature[1].DrawIndexed();
		GPUDrivenDrawIndirectCommandSignature.Finalize(RootSignature, sizeof(IndirectCommand));
	}

	void Shutdown()
	{
		GPUDrivenDrawIndirectCommandSignature.Destroy();
	}

	void DrawIndirect(GraphicsContext& context, GpuBuffer& ArgumentBuffer, uint32_t MaxCommands, uint64_t ArgumentStartOffset
		, GpuBuffer* CommandCounterBuffer, uint64_t CounterOffset)
	{
		context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, 
			ArgumentBuffer,
			ArgumentStartOffset,
			MaxCommands, CommandCounterBuffer, CounterOffset);
	}
}