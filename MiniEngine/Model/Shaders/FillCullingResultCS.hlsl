#ifndef __FILL_CULLING_RESULT_HLSLI__
#define __FILL_CULLING_RESULT_HLSLI__

#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "CullingCommon.hlsli"

[numthreads(CULLING_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint index = (groupId.x * CULLING_THREAD_GROUP_SIZE) + groupIndex;
    if (index < MaxCommands)
    {
        ByteAddressBuffer visibleFlags = GetIndirectVisibleFlagsBufferSRV(PsoIdx);
        AppendStructuredBuffer<IndirectCommand> OutputCommands = GetIndirectCullingResultsBufferUAV(PsoIdx);

        if (CullingStage == CULLING_RESULT_NO_CULLED)
        {
            OutputCommands.Append(GetIndirectCommandsBufferSRV(PsoIdx, index));
        }
        else if (CullingStage == CULLING_RESULT_OF_FRUSTUM)
        {
            uint visible = visibleFlags.Load(index * 4);
            if (visible & CULLING_FRUSTUM_VISIBLE)
            {
                OutputCommands.Append(GetIndirectCommandsBufferSRV(PsoIdx, index));
            }
        }
        else if (CullingStage == CULLING_RESULT_OF_OCCLUSION_PASS1)
        {
            uint visible = visibleFlags.Load(index * 4);
            if ((visible & CULLING_FRUSTUM_VISIBLE) > 0 &&
                (visible & CULLING_OCCLUSION_PASS1_VISIBLE) > 0)
            {
                OutputCommands.Append(GetIndirectCommandsBufferSRV(PsoIdx, index));
            }
        }
        else if (CullingStage == CULLING_RESULT_OF_OCCLUSION_PASS2)
        {
            uint visible = visibleFlags.Load(index * 4);
            if ((visible & CULLING_FRUSTUM_VISIBLE) > 0 &&
                (visible & CULLING_OCCLUSION_PASS1_VISIBLE) == 0 &&
                (visible & CULLING_OCCLUSION_PASS2_VISIBLE) > 0)
            {
                OutputCommands.Append(GetIndirectCommandsBufferSRV(PsoIdx, index));
            }
        }
        else if (CullingStage == CULLING_RESULT_OF_FREEZE_CULL)
        {
            uint visible = visibleFlags.Load(index * 4);
            if (visible & (CULLING_OCCLUSION_PASS1_VISIBLE | CULLING_OCCLUSION_PASS2_VISIBLE))
            {
                OutputCommands.Append(GetIndirectCommandsBufferSRV(PsoIdx, index));
            }
        }
    }
}
#endif