#include "InstanceResourceManager.h"
#include "ConstantBuffers.h"

void InstanceResourceManager::Initialize(uint32_t maxMeshConstants,
	uint32_t maxJoints,
	uint32_t meshConstantStride,
	uint32_t jointStride)
{
	m_MaxMeshConstants = maxMeshConstants;
	m_MaxJoints = maxJoints;
	m_MeshConstantStride = meshConstantStride;
	m_JointStride = jointStride;

	m_MeshConstantsCPU.Create(L"MeshConstantsAllCPU",
		maxMeshConstants * meshConstantStride);

	m_JointsCPU.Create(L"JointsAllCPU",
		maxJoints * jointStride);

	m_MeshConstantsGPU.Create(L"MeshConstantsAll",
		maxMeshConstants, meshConstantStride);
	m_JointsGPU.Create(L"JointsAll",
		maxJoints, jointStride);

	m_NextMeshConstant = 0;
	m_NextJoint = 0;

	m_InstanceCount = 0;
}

InstanceAllocation InstanceResourceManager::Allocate(uint32_t meshConstCount,
	uint32_t jointCount)
{
	InstanceAllocation alloc{};
	alloc.instanceID = m_InstanceCount++;
	alloc.meshConstantBase = m_NextMeshConstant;
	alloc.meshConstantCount = meshConstCount;
	alloc.jointBase = m_NextJoint;
	alloc.jointCount = jointCount;

	m_NextMeshConstant += meshConstCount;
	m_NextJoint += jointCount;

	return alloc;
}

void InstanceResourceManager::Cleanup()
{
	m_MeshConstantsCPU.Destroy();
	m_JointsCPU.Destroy();
	m_MeshConstantsGPU.Destroy();
	m_JointsGPU.Destroy();

	m_MaxMeshConstants = 0;
	m_MaxJoints = 0;

	m_MeshConstantStride = 0;
	m_JointStride = 0;

	m_NextMeshConstant = 0;
	m_NextJoint = 0;

	m_InstanceCount = 0;
}


void InstanceResourceManager::FlushBufferUpdate(GraphicsContext& ctx)
{
	ctx.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
	ctx.GetCommandList()->CopyBufferRegion(m_MeshConstantsGPU.GetResource(), 0,
		m_MeshConstantsCPU.GetResource(), 0, m_MaxMeshConstants * m_MeshConstantStride);
	ctx.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);

	ctx.TransitionResource(m_JointsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
	ctx.GetCommandList()->CopyBufferRegion(m_JointsGPU.GetResource(), 0,
		m_JointsCPU.GetResource(), 0, m_MaxJoints * m_JointStride);
	ctx.TransitionResource(m_JointsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);

	ctx.FlushResourceBarriers();
}

D3D12_GPU_VIRTUAL_ADDRESS InstanceResourceManager::GetMeshConstantsBuffer(const InstanceAllocation& a) const
{
	return m_MeshConstantsGPU.GetGpuVirtualAddress() + m_MeshConstantStride * a.meshConstantBase;
}

D3D12_GPU_VIRTUAL_ADDRESS InstanceResourceManager::GetJointsBuffer(const InstanceAllocation& a) const
{
	return m_JointsGPU.GetGpuVirtualAddress() + m_JointStride * a.jointBase;
}