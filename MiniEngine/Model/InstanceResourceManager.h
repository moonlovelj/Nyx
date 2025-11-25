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
	uint32_t indirectBase;
	uint32_t indirectCount;
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
		uint32_t maxIndirectArgs,
		uint32_t meshConstantStride,
		uint32_t jointStride,
		uint32_t indirectStride);

	InstanceAllocation Allocate(uint32_t meshConstCount,
		uint32_t jointCount,
		uint32_t indirectCount);

	void Cleanup();

	void ScheduleMeshConstantsUpdate(const InstanceAllocation& a,
		UploadBuffer& srcUpload,
		uint32_t first, uint32_t count);

	void ScheduleJointsUpdate(const InstanceAllocation& a,
		UploadBuffer& srcUpload,
		uint32_t first, uint32_t count);

	void ScheduleIndirectUpdate(const InstanceAllocation& a,
		UploadBuffer& srcUpload,
		uint32_t first, uint32_t count);

	void FlushScheduledUpdates(GraphicsContext& ctx);

	const ByteAddressBuffer& GetMeshConstantsBuffer() const { return m_MeshConstantsGPU; }
	const ByteAddressBuffer& GetJointsBuffer() const { return m_JointsGPU; }
	const ByteAddressBuffer& GetIndirectArgsBuffer() const { return m_IndirectGPU; }

	D3D12_GPU_VIRTUAL_ADDRESS GetMeshConstantsBuffer(const InstanceAllocation& a) const;
	D3D12_GPU_VIRTUAL_ADDRESS GetJointsBuffer(const InstanceAllocation& a) const;
	D3D12_GPU_VIRTUAL_ADDRESS GetIndirectArgsBuffer(const InstanceAllocation& a) const;

private:
	struct CopyJob
	{
		GpuResource* src;
		GpuResource* dstBuffer;
		size_t srcOffset;
		size_t dstOffset;
		size_t size;
	};

	std::mutex m_Mutex;
	std::vector<CopyJob> m_CopyJobs;

	uint32_t m_MaxMeshConstants = 0;
	uint32_t m_MaxJoints = 0;
	uint32_t m_MaxIndirect = 0;

	uint32_t m_MeshConstantStride = 0;
	uint32_t m_JointStride = 0;
	uint32_t m_IndirectStride = 0;

	uint32_t m_NextMeshConstant = 0;
	uint32_t m_NextJoint = 0;
	uint32_t m_NextIndirect = 0;

	uint32_t m_InstanceCount = 0;

	ByteAddressBuffer m_MeshConstantsGPU;
	ByteAddressBuffer m_JointsGPU;
	ByteAddressBuffer m_IndirectGPU;
};