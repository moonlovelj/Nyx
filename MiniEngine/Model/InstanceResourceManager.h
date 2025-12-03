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

class InstanceResourceManager
{
public:
	static InstanceResourceManager& Get()
	{
		static InstanceResourceManager g;
		return g;
	}

	void Initialize(uint32_t maxMeshConstants,
		uint32_t maxJoints,
		uint32_t meshConstantStride,
		uint32_t jointStride);

	InstanceAllocation Allocate(uint32_t meshConstCount,
		uint32_t jointCount);

	void Cleanup();

	void FlushBufferUpdate(GraphicsContext& ctx);

	const ByteAddressBuffer& GetMeshConstantsBuffer() const { return m_MeshConstantsGPU; }
	const ByteAddressBuffer& GetJointsBuffer() const { return m_JointsGPU; }

	D3D12_GPU_VIRTUAL_ADDRESS GetMeshConstantsBuffer(const InstanceAllocation& a) const;
	D3D12_GPU_VIRTUAL_ADDRESS GetJointsBuffer(const InstanceAllocation& a) const;

	UploadBuffer& GetMeshConstantsCPU() { return m_MeshConstantsCPU; }
	UploadBuffer& GetJointsCPU() { return m_JointsCPU; }

private:

	uint32_t m_MaxMeshConstants = 0;
	uint32_t m_MaxJoints = 0;

	uint32_t m_MeshConstantStride = 0;
	uint32_t m_JointStride = 0;

	uint32_t m_NextMeshConstant = 0;
	uint32_t m_NextJoint = 0;

	uint32_t m_InstanceCount = 0;

	UploadBuffer m_MeshConstantsCPU;
	UploadBuffer m_JointsCPU;

	ByteAddressBuffer m_MeshConstantsGPU;
	ByteAddressBuffer m_JointsGPU;
};