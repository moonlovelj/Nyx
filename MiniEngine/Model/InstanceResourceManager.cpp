#include "InstanceResourceManager.h"
#include "ConstantBuffers.h"

void InstanceResourceManager::Initialize(uint32_t maxMeshConstants,
	uint32_t maxJoints,
	uint32_t maxIndirectArgs,
	uint32_t meshConstantStride,
	uint32_t jointStride,
	uint32_t indirectStride)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_MaxMeshConstants = maxMeshConstants;
	m_MaxJoints = maxJoints;
	m_MaxIndirect = maxIndirectArgs;
	m_MeshConstantStride = meshConstantStride;
	m_JointStride = jointStride;
	m_IndirectStride = indirectStride;

	m_MeshConstantsGPU.Create(L"MeshConstantsAll",
		maxMeshConstants, meshConstantStride);
	m_JointsGPU.Create(L"JointsAll",
		maxJoints, jointStride);
	m_IndirectGPU.Create(L"IndirectArgsAll",
		maxIndirectArgs, indirectStride);

	m_NextMeshConstant = 0;
	m_NextJoint = 0;
	m_NextIndirect = 0;

	m_InstanceCount = 0;
}

InstanceAllocation InstanceResourceManager::Allocate(uint32_t meshConstCount,
	uint32_t jointCount,
	uint32_t indirectCount)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	InstanceAllocation alloc{};
	alloc.instanceID = m_InstanceCount++;
	alloc.meshConstantBase = m_NextMeshConstant;
	alloc.meshConstantCount = meshConstCount;
	alloc.jointBase = m_NextJoint;
	alloc.jointCount = jointCount;
	alloc.indirectBase = m_NextIndirect;
	alloc.indirectCount = indirectCount;

	m_NextMeshConstant += meshConstCount;
	m_NextJoint += jointCount;
	m_NextIndirect += indirectCount;

	return alloc;
}

void InstanceResourceManager::Cleanup()
{
	m_MeshConstantsGPU.Destroy();
	m_JointsGPU.Destroy();
	m_IndirectGPU.Destroy();

	m_MaxMeshConstants = 0;
	m_MaxJoints = 0;
	m_MaxIndirect = 0;

	m_MeshConstantStride = 0;
	m_JointStride = 0;
	m_IndirectStride = 0;

	m_NextMeshConstant = 0;
	m_NextJoint = 0;
	m_NextIndirect = 0;

	m_InstanceCount = 0;
}

void InstanceResourceManager::ScheduleMeshConstantsUpdate(const InstanceAllocation& a,
	UploadBuffer& srcUpload,
	uint32_t first, uint32_t count)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (count == 0) return;
	CopyJob job;
	job.src = &srcUpload;
	job.dstBuffer = &m_MeshConstantsGPU;
	job.srcOffset = size_t(first) * m_MeshConstantStride;
	job.dstOffset = size_t(a.meshConstantBase + first) * m_MeshConstantStride;
	job.size = size_t(count) * m_MeshConstantStride;
	m_CopyJobs.push_back(job);
}

void InstanceResourceManager::ScheduleJointsUpdate(const InstanceAllocation& a,
	UploadBuffer& srcUpload,
	uint32_t first, uint32_t count)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (count == 0) return;
	CopyJob job;
	job.src = &srcUpload;
	job.dstBuffer = &m_JointsGPU;
	job.srcOffset = size_t(first) * m_JointStride;
	job.dstOffset = size_t(a.jointBase + first) * m_JointStride;
	job.size = size_t(count) * m_JointStride;
	m_CopyJobs.push_back(job);
}

void InstanceResourceManager::ScheduleIndirectUpdate(const InstanceAllocation& a,
	UploadBuffer& srcUpload,
	uint32_t first, uint32_t count)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	for (uint32_t i = 0; i < count; ++i)
	{
		CopyJob job;
		job.src = &srcUpload;
		job.dstBuffer = &m_IndirectGPU;
		job.srcOffset = size_t(first + i) * m_IndirectStride;
		job.dstOffset = size_t(a.indirectBase + first + i) * m_IndirectStride;
		job.size = m_IndirectStride;
		m_CopyJobs.push_back(job);
	}
}

void InstanceResourceManager::FlushScheduledUpdates(GraphicsContext& ctx)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (m_CopyJobs.empty()) return;

	for (auto& job : m_CopyJobs)
	{
		ctx.TransitionResource(*job.dstBuffer, D3D12_RESOURCE_STATE_COPY_DEST, true);
		ctx.GetCommandList()->CopyBufferRegion(job.dstBuffer->GetResource(), job.dstOffset,
			job.src->GetResource(), job.srcOffset, job.size);
		ctx.TransitionResource(*job.dstBuffer, D3D12_RESOURCE_STATE_GENERIC_READ);
	}
	ctx.FlushResourceBarriers();
	m_CopyJobs.clear();
}

D3D12_GPU_VIRTUAL_ADDRESS InstanceResourceManager::GetMeshConstantsBuffer(const InstanceAllocation& a) const
{
	return m_MeshConstantsGPU.GetGpuVirtualAddress() + m_MeshConstantStride * a.meshConstantBase;
}

D3D12_GPU_VIRTUAL_ADDRESS InstanceResourceManager::GetJointsBuffer(const InstanceAllocation& a) const
{
	return m_JointsGPU.GetGpuVirtualAddress() + m_JointStride * a.jointBase;
}

D3D12_GPU_VIRTUAL_ADDRESS InstanceResourceManager::GetIndirectArgsBuffer(const InstanceAllocation& a) const
{
	return m_IndirectGPU.GetGpuVirtualAddress() + m_IndirectStride * a.indirectBase;
}