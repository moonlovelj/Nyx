#ifndef __DAG_CULL_HLSL__
#define __DAG_CULL_HLSL__
#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "BindlessIndices.hlsli"
#include "CullCommon.hlsli"
#include "WaveUtil.hlsli"
#include "../MeshletStructs.h"

groupshared uint GroupNumCandidateNodes;
groupshared uint GroupCandidateNodesOffset;

groupshared uint GroupNodeBatchStartIndex;

groupshared uint GroupNodeCount;
groupshared uint GroupNodeMask;
groupshared uint2 GroupNodeData[MAX_BVH_NODES_PER_GROUP];

groupshared uint GroupMeshletBatchStartIndex;
groupshared uint GroupMeshletBatchReadySize;

groupshared uint GroupLeafCount;
groupshared uint GroupLeafPrefix[DAG_CULL_GROUP_SIZE + 1];
groupshared uint2 GroupLeafKey[DAG_CULL_GROUP_SIZE];
groupshared uint GroupLeafMeshletCount[DAG_CULL_GROUP_SIZE];
groupshared uint GroupCandidateMeshletBase;
groupshared uint GroupCandidateMeshletCount;

uint FindLeafIndexForFlatMeshlet(uint flatIndex, uint leafCount)
{
    uint lo = 0u;
    uint hi = leafCount;
    while (lo + 1u < hi)
    {
        const uint mid = (lo + hi) >> 1u;
        if (GroupLeafPrefix[mid] <= flatIndex)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

void ProcessNodeBatch(uint batchSize, uint groupIndex, uint passIndex)
{
    globallycoherent RWStructuredBuffer<QueueState> taskStateUAV = GetTaskQueueStateBufferUAV();
    globallycoherent RWByteAddressBuffer taskQueueUAV = GetTaskQueueBufferUAV();
    
    // Each thread maps to (node slot in batch, child slot within that node).
    const uint nodeSlotInBatch = (groupIndex >> MAX_BVH_NODE_CHILDREN_BITS_NUM);
    const uint childSlotInNode = groupIndex & (MAX_BVH_NODE_CHILDREN_NUM - 1u);
    const uint safeNodeSlot = min(nodeSlotInBatch, batchSize - 1u);
    
    uint2 nodeData = GroupNodeData[safeNodeSlot];
    uint instanceIndex = nodeData.x;
    uint parentNodeIndex = nodeData.y;

    StructuredBuffer<HierarchyNode> hierarchyNodeSRV = GetHierarchyNodesBufferSRV();
    HierarchyNode parentNode = hierarchyNodeSRV[parentNodeIndex];

    uint childNodeCount = parentNode.GetChildNodeCount();
    uint childNodeStartIndex = parentNode.GetChildNodeStartIndex();
    uint childNodeIndex = childSlotInNode + childNodeStartIndex;
    HierarchyNode childNode;
    childNode.Init();

    if (childSlotInNode < childNodeCount)
    {
        childNode = hierarchyNodeSRV[childNodeIndex];
    }

    bool bVisible = (nodeSlotInBatch < batchSize);
    
    InstanceConstant inst = GetInstanceConstantSRV(instanceIndex);
    MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshBufferIdx);

#ifdef DAG_CULL_PASS0
    bVisible = bVisible && EvaluateBoundsVisibility(
        childNode.BBoxMin, childNode.BBoxMax,
        meshInstance.WorldMatrix,
        PrevViewProjMatrix,
        GetPrevSceneHZBSRV(FrameIndexMod2));
#endif
#ifdef DAG_CULL_PASS1
    bVisible = bVisible && EvaluateBoundsVisibility(
        childNode.BBoxMin, childNode.BBoxMax,
        meshInstance.WorldMatrix,
        ViewProjMatrix,
        GetCurrentSceneHZBSRV(FrameIndexMod2));
#endif

    bVisible = bVisible &&
        !TestForLod(meshInstance.WorldMatrix, ViewerPos, childNode.MaxParrentError, childNode.BoundSphere);
    
    bool bIsLeaf = childNode.IsGroup();

    bool bPushNode = bVisible && !bIsLeaf && (childSlotInNode < childNodeCount);
    
    uint nodeWriteOffsetInGroup = 0;
    if (bPushNode)
    {
        // Reserve one slot per visible internal child node.
        WaveInterlockedAddScalar(GroupNumCandidateNodes, bPushNode, 1u, nodeWriteOffsetInGroup);
    }

    GroupMemoryBarrierWithGroupSync();

    // Group leader reserves a contiguous range in the global node queue.
    if (groupIndex == 0)
    {
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeWriteOffset, GroupNumCandidateNodes, GroupCandidateNodesOffset);
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeCount, GroupNumCandidateNodes);
    }
    AllMemoryBarrierWithGroupSync();

    // Write new tasks to the queue
    if (bPushNode)
    {
        uint globalIdx = (GroupCandidateNodesOffset + nodeWriteOffsetInGroup);
        taskQueueUAV.Store2(globalIdx * NODE_BYTE_STRIDE, uint2(instanceIndex, childNodeIndex));
    }

    DeviceMemoryBarrierWithGroupSync();

    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0)
    {
        GroupLeafCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Collect visible leaf nodes for group-wide parallel candidate emission.
    if (bVisible && bIsLeaf)
    {
        const uint groupID = childNode.GetGroupIndex();

        globallycoherent RWByteAddressBuffer requestMaskUAV = GetGeometryStreamingRequestMaskBufferUAV();
        const uint requestMaskAddress = (groupID >> 5) << 2;
        const uint requestBitMask = 1u << (groupID & 31);
        uint originalTemp;
        requestMaskUAV.InterlockedOr(requestMaskAddress, requestBitMask, originalTemp);

        StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
        GroupDataLocation groupDataLocation = groupDataLocationSRV[groupID];
        bool bResident = groupDataLocation.ChunkIndex != INVALID_ID;

        uint leafOffsetInGroup = 0;
        WaveInterlockedAddScalar(GroupLeafCount, bResident, 1u, leafOffsetInGroup);
        if (bResident)
        {
            GroupLeafKey[leafOffsetInGroup] = uint2(instanceIndex, groupID);
            GroupLeafMeshletCount[leafOffsetInGroup] = childNode.GetMeshletCount();
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // Build leaf prefix and reserve candidate range once per group.
    if (groupIndex == 0)
    {
        GroupLeafPrefix[0] = 0;
        [loop]
        for (uint leafIndex = 0; leafIndex < GroupLeafCount; ++leafIndex)
        {
            GroupLeafPrefix[leafIndex + 1] = GroupLeafPrefix[leafIndex] + GroupLeafMeshletCount[leafIndex];
        }

        const uint totalMeshletsInGroup = GroupLeafPrefix[GroupLeafCount];

        uint meshletStart = 0;
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].TotalMeshlets, totalMeshletsInGroup, meshletStart);

        uint candidateWriteOffset = 0;
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].CandidateMeshletWriteOffset, totalMeshletsInGroup, candidateWriteOffset);

        const uint candidateEnd = min(candidateWriteOffset + totalMeshletsInGroup, MAX_CANDIDATE_MESHLETS);
        GroupCandidateMeshletBase = candidateWriteOffset;
        GroupCandidateMeshletCount = (uint)max((int)candidateEnd - (int)candidateWriteOffset, 0);
    }

    GroupMemoryBarrierWithGroupSync();

    globallycoherent RWByteAddressBuffer candidateMeshletBufferUAV = GetCandidateMeshletBufferUAV();
    for (uint flatMeshletIndex = groupIndex; flatMeshletIndex < GroupCandidateMeshletCount; flatMeshletIndex += DAG_CULL_GROUP_SIZE)
    {
        const uint leafIndex = FindLeafIndexForFlatMeshlet(flatMeshletIndex, GroupLeafCount);
        const uint localMeshletIndex = flatMeshletIndex - GroupLeafPrefix[leafIndex];
        const uint2 leafKey = GroupLeafKey[leafIndex];

        VisibleMeshletPayload meshletPayload;
        meshletPayload.InstanceIndex = leafKey.x;
        meshletPayload.PackedData = meshletPayload.PackData(leafKey.y, localMeshletIndex);

        const uint outIndex = GroupCandidateMeshletBase + flatMeshletIndex;
        candidateMeshletBufferUAV.Store2(outIndex * MESHLET_BYTE_STRIDE, uint2(meshletPayload.InstanceIndex, meshletPayload.PackedData));
    }

    DeviceMemoryBarrierWithGroupSync();

    if (groupIndex == 0)
    {
        const uint startIndex = GroupCandidateMeshletBase;
        const uint endIndex = GroupCandidateMeshletBase + GroupCandidateMeshletCount;
        globallycoherent RWByteAddressBuffer meshletBatchBufferUAV = GetMeshletBatchBufferUAV();

        for (uint index = startIndex; index < endIndex;)
        {
            const uint batchIndex = index / DAG_CULL_GROUP_SIZE;
            const uint nextIndex = (index & ~(DAG_CULL_GROUP_SIZE - 1u)) + DAG_CULL_GROUP_SIZE;
            const uint maxIndex = min(nextIndex, endIndex);
            const uint num = maxIndex - index;
            uint temp;
            meshletBatchBufferUAV.InterlockedAdd(batchIndex * 4, num, temp);
            index = nextIndex;
        }
    }

    DeviceMemoryBarrierWithGroupSync();
    
    if (groupIndex == 0)
    {
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeCount, -batchSize);
    }
}

void ProcessMeshletBatch(uint meshletBatchStartIndex, uint meshletBatchReadySize, uint groupIndex, uint passIndex)
{
    if (groupIndex < meshletBatchReadySize)
    {
        const uint candidateIndex = meshletBatchStartIndex * DAG_CULL_GROUP_SIZE + groupIndex;
        globallycoherent RWByteAddressBuffer candidateMeshletBufferUAV = GetCandidateMeshletBufferUAV();
        VisibleMeshletPayload meshletPayload;
        const uint2 loadedMeshlet = candidateMeshletBufferUAV.Load2(candidateIndex * MESHLET_BYTE_STRIDE);
        meshletPayload.InstanceIndex = loadedMeshlet.x;
        meshletPayload.PackedData = loadedMeshlet.y;

        const uint meshletGroupIndex = meshletPayload.GetGroupIndex();
        StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
        GroupDataLocation groupDataLocation = groupDataLocationSRV[meshletGroupIndex];

        ByteAddressBuffer geometryChunksBuffer = GetGeometryChunksBufferSRV(groupDataLocation.ChunkIndex);
        const uint meshletHeaderStart = groupDataLocation.ByteOffset + sizeof(GroupHeader);
        const uint meshletIndexInGroup = meshletPayload.GetMeshletIndex();
        
        MeshletHeader meshletHeader = geometryChunksBuffer.Load<MeshletHeader>(meshletHeaderStart + meshletIndexInGroup * sizeof(MeshletHeader));
        InstanceConstant inst = GetInstanceConstantSRV(meshletPayload.InstanceIndex);
        MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshBufferIdx);
        bool bAlphaBlend = (meshletHeader.GetPSOFlags() & PSO_ALPHA_BLEND) != 0u;

        bool bMeshletVisible = EvaluateBoundsVisibility(
            meshletHeader.BBoxMin,
            meshletHeader.BBoxMax,
            meshInstance.WorldMatrix,
            PrevViewProjMatrix,
            GetPrevSceneHZBSRV(FrameIndexMod2));
        
#ifdef DAG_CULL_PASS1
            bMeshletVisible = !bMeshletVisible && EvaluateBoundsVisibility(
                meshletHeader.BBoxMin,
                meshletHeader.BBoxMax,
                meshInstance.WorldMatrix,
                ViewProjMatrix,
                GetCurrentSceneHZBSRV(FrameIndexMod2));
#endif
        
        bMeshletVisible = bMeshletVisible && !bAlphaBlend;

        if (bMeshletVisible && meshletHeader.RefineGroupIndex != INVALID_ID)
        {
            GroupDataLocation refineGroupDataLocation = groupDataLocationSRV[meshletHeader.RefineGroupIndex];
            if (refineGroupDataLocation.ChunkIndex != INVALID_ID)
            {
                ByteAddressBuffer refineGeometryChunksBuffer = GetGeometryChunksBufferSRV(refineGroupDataLocation.ChunkIndex);
                GroupHeader refineGroupHeader = refineGeometryChunksBuffer.Load < GroupHeader > (refineGroupDataLocation.ByteOffset);
                bMeshletVisible = TestForLod(meshInstance.WorldMatrix, ViewerPos, refineGroupHeader.ParrentError, refineGroupHeader.BoundSphere);
            }
        }

        if (bMeshletVisible)
        {
            globallycoherent RWStructuredBuffer<QueueState> taskStateUAV = GetTaskQueueStateBufferUAV();
            uint writeOffset;
            WaveInterlockedAddScalar(taskStateUAV[0].PassState[passIndex].VisibleMeshletCount, true, 1, writeOffset);
            RWStructuredBuffer<VisibleMeshletPayload> payloadBufferUAV = GetVisibleMeshletBufferUAV();
#ifdef DAG_CULL_PASS1
            // Write after pass0
            if (writeOffset + taskStateUAV[0].PassState[0].VisibleMeshletCount < MAX_VISIBLE_MESHLETS)
                payloadBufferUAV[writeOffset + taskStateUAV[0].PassState[0].VisibleMeshletCount] = meshletPayload;
#else
            if (writeOffset < MAX_VISIBLE_MESHLETS)
                payloadBufferUAV[writeOffset] = meshletPayload;
#endif
        }
    }

    // Clear immediately for the next pass
    globallycoherent RWByteAddressBuffer meshletBatchBufferUAV = GetMeshletBatchBufferUAV();
    meshletBatchBufferUAV.Store(meshletBatchStartIndex * 4, 0);
}

[RootSignature(Renderer_RootSig)]
[numthreads(DAG_CULL_GROUP_SIZE, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
#ifdef DAG_CULL_PASS1
    const uint passIndex = 1;
#else
    const uint passIndex = 0;
#endif
    globallycoherent RWStructuredBuffer<QueueState> taskStateUAV = GetTaskQueueStateBufferUAV();
    globallycoherent RWByteAddressBuffer taskQueueUAV = GetTaskQueueBufferUAV();
    
    bool processNodeQueue = true;
    uint nodeBatchLocalOffset = MAX_BVH_NODES_PER_GROUP;
    uint nodeBatchBaseIndex = 0;
    uint meshletBatchStartIndex = INVALID_ID;
    
    while (true)
    {
        // Reset per-iteration group-shared counters.
        GroupMemoryBarrierWithGroupSync();
        if (groupIndex == 0)
        {
            GroupNumCandidateNodes = 0;
            GroupNodeMask = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        
        uint readyNodeMask = 0;
        if (processNodeQueue)
        {
            if (nodeBatchLocalOffset == MAX_BVH_NODES_PER_GROUP)
            {
                // Claim the next node-batch range from the global queue.
                if (groupIndex == 0)
                {
                    InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeReadOffset, MAX_BVH_NODES_PER_GROUP, GroupNodeBatchStartIndex);
                }
                
                GroupMemoryBarrierWithGroupSync();

                nodeBatchLocalOffset = 0;
                nodeBatchBaseIndex = GroupNodeBatchStartIndex;
                if (nodeBatchBaseIndex >= MAX_NODES)
                {
                    processNodeQueue = false;
                    continue;
                }
            }
            
            const uint nodeQueueIndex = nodeBatchBaseIndex + nodeBatchLocalOffset + groupIndex;
            bool laneHasNode = (nodeBatchLocalOffset + groupIndex < MAX_BVH_NODES_PER_GROUP);
            if (laneHasNode)
            {
                uint2 nodeData = taskQueueUAV.Load2(nodeQueueIndex * NODE_BYTE_STRIDE);
                laneHasNode = nodeData.x != INVALID_ID && nodeData.y != INVALID_ID;
                if (laneHasNode)
                {
                    GroupNodeData[groupIndex] = nodeData;
                }
            }

            if (laneHasNode)
            {
                InterlockedOr(GroupNodeMask, 1u << groupIndex);
            }
            AllMemoryBarrierWithGroupSync();
            
            readyNodeMask = GroupNodeMask;

            // If the first slot is valid, process the contiguous ready prefix.
            if (readyNodeMask & 1u)
            {
                uint batchSize = firstbitlow(~readyNodeMask);
                ProcessNodeBatch(batchSize, groupIndex, passIndex);
                if (groupIndex < batchSize)
                {
                    taskQueueUAV.Store2(nodeQueueIndex * NODE_BYTE_STRIDE, uint2(INVALID_ID, INVALID_ID));
                }

                nodeBatchLocalOffset += batchSize;
                continue;
            }
        }
        
        // No processable nodes this iteration; poll meshlet batches.
        if (meshletBatchStartIndex == INVALID_ID)
        {
            if (groupIndex == 0)
            {
                InterlockedAdd(taskStateUAV[0].PassState[passIndex].MeshletBatchReadOffset, 1, GroupMeshletBatchStartIndex);
            }
            GroupMemoryBarrierWithGroupSync();
            meshletBatchStartIndex = GroupMeshletBatchStartIndex;
        }

        if (!processNodeQueue && GroupMeshletBatchStartIndex >= MAX_CANDIDATE_MESHLETS_BATCH)
            break;

        if (groupIndex == 0)
        {
            GroupNodeCount = taskStateUAV[0].PassState[passIndex].NodeCount;
            globallycoherent RWByteAddressBuffer meshletBatchUAV = GetMeshletBatchBufferUAV();
            GroupMeshletBatchReadySize = meshletBatchUAV.Load(meshletBatchStartIndex * 4);
        }
        GroupMemoryBarrierWithGroupSync();

        uint meshletBatchReadySize = GroupMeshletBatchReadySize;
        if (!processNodeQueue && meshletBatchReadySize == 0)
            break;

        bool canRunMeshletPass =
            (processNodeQueue && meshletBatchReadySize == DAG_CULL_GROUP_SIZE) ||
            (!processNodeQueue && meshletBatchReadySize > 0);

        if (canRunMeshletPass)
        {
            ProcessMeshletBatch(meshletBatchStartIndex, meshletBatchReadySize, groupIndex, passIndex);
            meshletBatchStartIndex = INVALID_ID;
        }

        if (processNodeQueue && GroupNodeCount == 0)
        {
            processNodeQueue = false;
        }
    }
}

#endif
