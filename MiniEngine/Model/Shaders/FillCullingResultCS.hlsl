#ifndef __FILL_CULLING_RESULT_HLSLI__
#define __FILL_CULLING_RESULT_HLSLI__

#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "CullingCommon.hlsli"

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint index = (groupId.x * THREAD_GROUP_SIZE) + groupIndex;
    if (index < maxCommands)
    {
        ByteAddressBuffer visibleFlags = ResourceDescriptorHeap[GetVisibleFlagSRVIndexInDescriptorHeap()];
        AppendStructuredBuffer<IndirectCommand> OutputCommands = ResourceDescriptorHeap[GetCullingResultUAVIndexInDescriptorHeap()];
        if (cullingStage == CULLING_RESULT_NO_CULLED)
        {
            OutputCommands.Append(inputCommands[index + startCommand]);
        }
        else if (cullingStage == CULLING_RESULT_OF_FRUSTUM)
        {
            uint visible = visibleFlags.Load(index * 4);
            if (visible & CULLING_FRUSTUM_VISIBLE)
            {
                OutputCommands.Append(inputCommands[index + startCommand]);
            }
        }
        else if (cullingStage == CULLING_RESULT_OF_OCCLUSION_PASS1)
        {
            uint visible = visibleFlags.Load(index * 4);
            if ((visible & CULLING_FRUSTUM_VISIBLE) > 0 && 
                (visible & CULLING_OCCLUSION_PASS1_VISIBLE) > 0)
            {
                OutputCommands.Append(inputCommands[index + startCommand]);
            }
        }
        else if (cullingStage == CULLING_RESULT_OF_OCCLUSION_PASS2)
        {
            uint visible = visibleFlags.Load(index * 4);
            if ((visible & CULLING_FRUSTUM_VISIBLE) > 0 &&
                (visible & CULLING_OCCLUSION_PASS1_VISIBLE) == 0 &&
                (visible & CULLING_OCCLUSION_PASS2_VISIBLE) > 0)
            {
                OutputCommands.Append(inputCommands[index + startCommand]);
            }
        }
    }
}
#endif