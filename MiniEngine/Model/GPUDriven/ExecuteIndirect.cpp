#include "ExecuteIndirect.h"

#include "../Core/CommandContext.h"

namespace GPUDriven
{
	BoolVar Enable("GPUDriven/Enabled", true);

	CommandSignature GPUDrivenDrawIndirectCommandSignature;

	void Initialize(const RootSignature* RootSignature)
	{
		GPUDrivenDrawIndirectCommandSignature.Reset(3);
		GPUDrivenDrawIndirectCommandSignature[0].ConstantBufferView(4);
		GPUDrivenDrawIndirectCommandSignature[1].IndexBufferView();
		GPUDrivenDrawIndirectCommandSignature[2].DrawIndexed();
		GPUDrivenDrawIndirectCommandSignature.Finalize(RootSignature, sizeof(IndirectCommand));
	}

	void Shutdown()
	{
		GPUDrivenDrawIndirectCommandSignature.Destroy();
	}

	void DrawIndirect(GraphicsContext& context, GpuBuffer& ArgumentBuffer, uint32_t MaxCommands, uint64_t ArgumentStartOffset)
	{
		context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, 
			ArgumentBuffer,
			ArgumentStartOffset,
			MaxCommands);
	}
}