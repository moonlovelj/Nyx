#include "ExecuteIndirect.h"

#include "../Core/CommandContext.h"

namespace GPUDriven
{
	BoolVar Enable("GPUDriven/Enabled", true);

	CommandSignature GPUDrivenDrawIndirectCommandSignature;

	void Initialize(const RootSignature* RootSignature)
	{
		GPUDrivenDrawIndirectCommandSignature.Reset(1);
		GPUDrivenDrawIndirectCommandSignature[0].DrawIndexed();
		GPUDrivenDrawIndirectCommandSignature.Finalize(RootSignature);
	}

	void Shutdown()
	{
		GPUDrivenDrawIndirectCommandSignature.Destroy();
	}

	void DrawIndirect(GraphicsContext& context, const IndirectArgsBufferWarp& IndirectArgsBufferWarp)
	{
		context.ExecuteIndirect(GPUDrivenDrawIndirectCommandSignature, *IndirectArgsBufferWarp.indirectArgsBuffer, 0, IndirectArgsBufferWarp.numCommands);
	}
}