#include "ExecuteIndirect.h"

#include "../Core/CommandContext.h"

namespace GPUDriven
{
	BoolVar Enable("GPUDriven/Enabled", true);

	CommandSignature GPUDrivenDrawIndirectCommandSignature;

	void Initialize(const RootSignature* RootSignature)
	{
		GPUDrivenDrawIndirectCommandSignature.Reset(6);
		GPUDrivenDrawIndirectCommandSignature[0].ConstantBufferView(0);
		GPUDrivenDrawIndirectCommandSignature[1].ConstantBufferView(1);
		GPUDrivenDrawIndirectCommandSignature[2].ShaderResourceView(4);
		GPUDrivenDrawIndirectCommandSignature[3].VertexBufferView(0);
		GPUDrivenDrawIndirectCommandSignature[4].IndexBufferView();
		GPUDrivenDrawIndirectCommandSignature[5].DrawIndexed();
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