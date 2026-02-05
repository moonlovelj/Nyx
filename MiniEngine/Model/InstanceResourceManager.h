#pragma once
#include "../Core/GpuBuffer.h"
#include "../Core/UploadBuffer.h"
#include "../Core/CommandContext.h"
#include <vector>
#include <mutex>
#include <cstdint>

struct InstanceAllocation
{
	uint32_t instanceID;
	uint32_t meshConstantBase;
	uint32_t meshConstantCount;
	uint32_t jointBase;
	uint32_t jointCount;
};

namespace InstanceResourceManager
{
	void Initialize(uint32_t maxMeshConstants,
		uint32_t maxJoints,
		uint32_t meshConstantStride,
		uint32_t jointStride);

	InstanceAllocation Allocate(uint32_t meshConstCount,
		uint32_t jointCount);

	void Cleanup();

	void FlushBufferUpdate(GraphicsContext& ctx);

	const ByteAddressBuffer& GetMeshConstantsBuffer();
	const ByteAddressBuffer& GetJointsBuffer();

	D3D12_GPU_VIRTUAL_ADDRESS GetMeshConstantsBuffer(const InstanceAllocation& a);
	D3D12_GPU_VIRTUAL_ADDRESS GetJointsBuffer(const InstanceAllocation& a);

	UploadBuffer& GetMeshConstantsCPU();
	UploadBuffer& GetJointsCPU();
};
