#include "InstanceResourceManager.h"
#include "ConstantBuffers.h"

namespace InstanceResourceManager
{
    uint32_t s_MaxMeshConstants = 0;
    uint32_t s_MaxJoints = 0;

    uint32_t s_MeshConstantStride = 0;
    uint32_t s_JointStride = 0;

    uint32_t s_NextMeshConstant = 0;
    uint32_t s_NextJoint = 0;

    uint32_t s_InstanceCount = 0;

    UploadBuffer s_MeshConstantsCPU;
    UploadBuffer s_JointsCPU;

    ByteAddressBuffer s_MeshConstantsGPU;
    ByteAddressBuffer s_JointsGPU;

	void Initialize(uint32_t maxMeshConstants,
		uint32_t maxJoints,
		uint32_t meshConstantStride,
		uint32_t jointStride)
	{
		s_MaxMeshConstants = maxMeshConstants;
		s_MaxJoints = maxJoints;
		s_MeshConstantStride = meshConstantStride;
		s_JointStride = jointStride;

		s_MeshConstantsCPU.Create(L"MeshConstantsAllCPU",
			maxMeshConstants * meshConstantStride);

		s_JointsCPU.Create(L"JointsAllCPU",
			maxJoints * jointStride);

		s_MeshConstantsGPU.Create(L"MeshConstantsAll",
			maxMeshConstants, meshConstantStride);
		s_JointsGPU.Create(L"JointsAll",
			maxJoints, jointStride);

		s_NextMeshConstant = 0;
		s_NextJoint = 0;

		s_InstanceCount = 0;
	}

	InstanceAllocation Allocate(uint32_t meshConstCount,
		uint32_t jointCount)
	{
		InstanceAllocation alloc{};
		alloc.instanceID = s_InstanceCount++;
		alloc.meshConstantBase = s_NextMeshConstant;
		alloc.meshConstantCount = meshConstCount;
		alloc.jointBase = s_NextJoint;
		alloc.jointCount = jointCount;

		s_NextMeshConstant += meshConstCount;
		s_NextJoint += jointCount;

		return alloc;
	}

	void Cleanup()
	{
		s_MeshConstantsCPU.Destroy();
		s_JointsCPU.Destroy();
		s_MeshConstantsGPU.Destroy();
		s_JointsGPU.Destroy();

		s_MaxMeshConstants = 0;
		s_MaxJoints = 0;

		s_MeshConstantStride = 0;
		s_JointStride = 0;

		s_NextMeshConstant = 0;
		s_NextJoint = 0;

		s_InstanceCount = 0;
	}

	void FlushBufferUpdate(GraphicsContext& ctx)
	{
		ctx.TransitionResource(s_MeshConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
		ctx.GetCommandList()->CopyBufferRegion(s_MeshConstantsGPU.GetResource(), 0,
			s_MeshConstantsCPU.GetResource(), 0, s_MaxMeshConstants * s_MeshConstantStride);
		ctx.TransitionResource(s_MeshConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);

		ctx.TransitionResource(s_JointsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
		ctx.GetCommandList()->CopyBufferRegion(s_JointsGPU.GetResource(), 0,
			s_JointsCPU.GetResource(), 0, s_MaxJoints * s_JointStride);
		ctx.TransitionResource(s_JointsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);

		ctx.FlushResourceBarriers();
	}

	const ByteAddressBuffer& GetMeshConstantsBuffer()
	{
		return s_MeshConstantsGPU;
	}

	const ByteAddressBuffer& GetJointsBuffer()
	{
		return s_JointsGPU;
	}

	D3D12_GPU_VIRTUAL_ADDRESS GetMeshConstantsBuffer(const InstanceAllocation& a)
	{
		return s_MeshConstantsGPU.GetGpuVirtualAddress() + s_MeshConstantStride * a.meshConstantBase;
	}

	D3D12_GPU_VIRTUAL_ADDRESS GetJointsBuffer(const InstanceAllocation& a)
	{
		return s_JointsGPU.GetGpuVirtualAddress() + s_JointStride * a.jointBase;
	}

	UploadBuffer& GetMeshConstantsCPU()
	{
		return s_MeshConstantsCPU;
	}

	UploadBuffer& GetJointsCPU()
	{
		return s_JointsCPU;
	}
}
