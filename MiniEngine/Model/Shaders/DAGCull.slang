//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR NON-INFRINGEMENT.
//

//#ifndef DAG_CULL_PASS_INDEX
//#error DAG_CULL_PASS_INDEX must be supplied through ProgramDesc::AddDefine().
//#endif

#if DAG_CULL_PASS_INDEX == 1
#define DAG_CULL_PASS1
#else
#define DAG_CULL_PASS0
#endif

#include "CommonResources.slang"
#include "CullCommon.slang"
#include "WaveUtil.h.slang"

struct DAGCullConstants
{
    float4x4 ViewProjMatrix;
    float3 ViewerPos;
    float LodScale;
    float PixelErrorThreshold;
    uint ProjectionType;
    uint ViewportWidth;
    uint ViewportHeight;

#ifndef DISABLE_HZB_CULL
    float4x4 PrevViewProjMatrix;
    float4 HZBSizeAndInv;
    DescriptorHandle<Texture2D<float>> PreviousHZB;
    DescriptorHandle<Texture2D<float>> CurrentHZB;
#endif
};

ConstantBuffer<DAGCullConstants> g_DAGCull;

#ifndef DISABLE_HZB_CULL
SamplerState g_HZBSampler;
#endif

globallycoherent RWStructuredBuffer<QueueState> g_TaskQueueStateUAV;
globallycoherent RWByteAddressBuffer g_TaskQueueUAV;
globallycoherent RWByteAddressBuffer g_MeshletBatchUAV;
globallycoherent RWByteAddressBuffer g_CandidateMeshletUAV;

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

CullCommonContext GetCullCommonContext()
{
    CullCommonContext context;
    context.ViewportWidth = g_DAGCull.ViewportWidth;
    context.ViewportHeight = g_DAGCull.ViewportHeight;
#ifndef DISABLE_HZB_CULL
    context.HZBSizeAndInv = g_DAGCull.HZBSizeAndInv;
#endif
    return context;
}

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
    
    InstanceConstant inst = LoadInstanceConstant(instanceIndex);
    MeshConstant meshInstance = LoadMeshConstant(inst.MeshBufferIdx);
    CullCommonContext cullContext = GetCullCommonContext();

#ifndef DISABLE_HZB_CULL
#ifdef DAG_CULL_PASS0
    bVisible = bVisible && EvaluateBoundsVisibility(
        childNode.BBoxMin, childNode.BBoxMax,
        meshInstance.WorldMatrix,
        g_DAGCull.PrevViewProjMatrix,
        g_DAGCull.PreviousHZB,
        g_HZBSampler,
        cullContext);
#endif
#ifdef DAG_CULL_PASS1
    bVisible = bVisible && EvaluateBoundsVisibility(
        childNode.BBoxMin, childNode.BBoxMax,
        meshInstance.WorldMatrix,
        g_DAGCull.ViewProjMatrix,
        g_DAGCull.CurrentHZB,
        g_HZBSampler,
        cullContext);
#endif
#else
    bVisible = bVisible && EvaluateBoundsInFrustum(
        childNode.BBoxMin, childNode.BBoxMax,
        meshInstance.WorldMatrix,
        g_DAGCull.ViewProjMatrix,
        cullContext);
#endif

    bVisible = bVisible &&
        !TestForLod(
            meshInstance.WorldMatrix,
            g_DAGCull.ViewerPos,
            childNode.MaxParrentError,
            childNode.BoundSphere,
            g_DAGCull.LodScale,
            g_DAGCull.PixelErrorThreshold,
            g_DAGCull.ProjectionType == 0);
    
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
        InterlockedAdd(g_TaskQueueStateUAV[0].PassState[passIndex].NodeWriteOffset, GroupNumCandidateNodes, GroupCandidateNodesOffset);
        InterlockedAdd(g_TaskQueueStateUAV[0].PassState[passIndex].NodeCount, GroupNumCandidateNodes);
    }
    AllMemoryBarrierWithGroupSync();

    // Write new tasks to the queue
    if (bPushNode)
    {
        uint globalIdx = (GroupCandidateNodesOffset + nodeWriteOffsetInGroup);
        g_TaskQueueUAV.Store2(globalIdx * NODE_BYTE_STRIDE, uint2(instanceIndex, childNodeIndex));
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

        const uint requestMaskAddress = (groupID >> 5) << 2;
        const uint requestBitMask = 1u << (groupID & 31);
        uint originalTemp;
        RWByteAddressBuffer requestMaskUAV = GetGeometryStreamingRequestMaskBufferUAV();
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
        InterlockedAdd(g_TaskQueueStateUAV[0].PassState[passIndex].TotalMeshlets, totalMeshletsInGroup, meshletStart);

        uint candidateWriteOffset = 0;
        InterlockedAdd(g_TaskQueueStateUAV[0].PassState[passIndex].CandidateMeshletWriteOffset, totalMeshletsInGroup, candidateWriteOffset);

        const uint candidateEnd = min(candidateWriteOffset + totalMeshletsInGroup, MAX_CANDIDATE_MESHLETS);
        GroupCandidateMeshletBase = candidateWriteOffset;
        GroupCandidateMeshletCount = (uint)max((int)candidateEnd - (int)candidateWriteOffset, 0);
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint flatMeshletIndex = groupIndex; flatMeshletIndex < GroupCandidateMeshletCount; flatMeshletIndex += DAG_CULL_GROUP_SIZE)
    {
        const uint leafIndex = FindLeafIndexForFlatMeshlet(flatMeshletIndex, GroupLeafCount);
        const uint localMeshletIndex = flatMeshletIndex - GroupLeafPrefix[leafIndex];
        const uint2 leafKey = GroupLeafKey[leafIndex];

        VisibleMeshletPayload meshletPayload;
        meshletPayload.InstanceIndex = leafKey.x;
        meshletPayload.PackedData = meshletPayload.PackData(leafKey.y, localMeshletIndex);

        const uint outIndex = GroupCandidateMeshletBase + flatMeshletIndex;
        g_CandidateMeshletUAV.Store2(outIndex * MESHLET_BYTE_STRIDE, uint2(meshletPayload.InstanceIndex, meshletPayload.PackedData));
    }

    DeviceMemoryBarrierWithGroupSync();

    if (groupIndex == 0)
    {
        const uint startIndex = GroupCandidateMeshletBase;
        const uint endIndex = GroupCandidateMeshletBase + GroupCandidateMeshletCount;

        for (uint index = startIndex; index < endIndex;)
        {
            const uint batchIndex = index / DAG_CULL_GROUP_SIZE;
            const uint nextIndex = (index & ~(DAG_CULL_GROUP_SIZE - 1u)) + DAG_CULL_GROUP_SIZE;
            const uint maxIndex = min(nextIndex, endIndex);
            const uint num = maxIndex - index;
            uint temp;
            g_MeshletBatchUAV.InterlockedAdd(batchIndex * 4, num, temp);
            index = nextIndex;
        }
    }

    DeviceMemoryBarrierWithGroupSync();
    
    if (groupIndex == 0)
    {
        InterlockedAdd(g_TaskQueueStateUAV[0].PassState[passIndex].NodeCount, -batchSize);
    }
}

void ProcessMeshletBatch(uint meshletBatchStartIndex, uint meshletBatchReadySize, uint groupIndex, uint passIndex)
{
    if (groupIndex < meshletBatchReadySize)
    {
        const uint candidateIndex = meshletBatchStartIndex * DAG_CULL_GROUP_SIZE + groupIndex;
        VisibleMeshletPayload meshletPayload;
        const uint2 loadedMeshlet = g_CandidateMeshletUAV.Load2(candidateIndex * MESHLET_BYTE_STRIDE);
        meshletPayload.InstanceIndex = loadedMeshlet.x;
        meshletPayload.PackedData = loadedMeshlet.y;

        const uint meshletGroupIndex = meshletPayload.GetGroupIndex();
        StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
        GroupDataLocation groupDataLocation = groupDataLocationSRV[meshletGroupIndex];

        ByteAddressBuffer geometryChunksBuffer = GetGeometryChunksBufferSRV(groupDataLocation.ChunkIndex);
        const uint meshletHeaderStart = groupDataLocation.ByteOffset + sizeof(GroupHeader);
        const uint meshletIndexInGroup = meshletPayload.GetMeshletIndex();
        
        MeshletHeader meshletHeader = geometryChunksBuffer.Load<MeshletHeader>(meshletHeaderStart + meshletIndexInGroup * sizeof(MeshletHeader));
        InstanceConstant inst = LoadInstanceConstant(meshletPayload.InstanceIndex);
        MeshConstant meshInstance = LoadMeshConstant(inst.MeshBufferIdx);
        bool bAlphaBlend = (meshletHeader.GetPSOFlags() & PSO_ALPHA_BLEND) != 0u;
        CullCommonContext cullContext = GetCullCommonContext();

#ifndef DISABLE_HZB_CULL
        bool bMeshletVisible = EvaluateBoundsVisibility(
            meshletHeader.BBoxMin,
            meshletHeader.BBoxMax,
            meshInstance.WorldMatrix,
            g_DAGCull.PrevViewProjMatrix,
            g_DAGCull.PreviousHZB,
            g_HZBSampler,
            cullContext);
        
#ifdef DAG_CULL_PASS1
            bMeshletVisible = !bMeshletVisible && EvaluateBoundsVisibility(
                meshletHeader.BBoxMin,
                meshletHeader.BBoxMax,
                meshInstance.WorldMatrix,
                g_DAGCull.ViewProjMatrix,
                g_DAGCull.CurrentHZB,
                g_HZBSampler,
                cullContext);
#endif
#else
        bool bMeshletVisible = EvaluateBoundsInFrustum(
            meshletHeader.BBoxMin,
            meshletHeader.BBoxMax,
            meshInstance.WorldMatrix,
            g_DAGCull.ViewProjMatrix,
            cullContext);
#endif
        
        bMeshletVisible = bMeshletVisible && !bAlphaBlend;

        if (bMeshletVisible && meshletHeader.RefineGroupIndex != INVALID_ID)
        {
            GroupDataLocation refineGroupDataLocation = groupDataLocationSRV[meshletHeader.RefineGroupIndex];
            if (refineGroupDataLocation.ChunkIndex != INVALID_ID)
            {
                ByteAddressBuffer refineGeometryChunksBuffer = GetGeometryChunksBufferSRV(refineGroupDataLocation.ChunkIndex);
                GroupHeader refineGroupHeader = refineGeometryChunksBuffer.Load<GroupHeader>(refineGroupDataLocation.ByteOffset);
                bMeshletVisible = TestForLod(
                    meshInstance.WorldMatrix,
                    g_DAGCull.ViewerPos,
                    refineGroupHeader.ParrentError,
                    refineGroupHeader.BoundSphere,
                    g_DAGCull.LodScale,
                    g_DAGCull.PixelErrorThreshold,
                    g_DAGCull.ProjectionType == 0);
            }
        }

        if (bMeshletVisible)
        {
            uint writeOffset;
            WaveInterlockedAddScalar(g_TaskQueueStateUAV[0].PassState[passIndex].VisibleMeshletCount, true, 1, writeOffset);
            RWStructuredBuffer<VisibleMeshletPayload> payloadBufferUAV = GetVisibleMeshletBufferUAV();

#ifndef DISABLE_HZB_CULL
#ifdef DAG_CULL_PASS1
            // Write after pass0
            if (writeOffset + g_TaskQueueStateUAV[0].PassState[0].VisibleMeshletCount < MAX_VISIBLE_MESHLETS)
                payloadBufferUAV[writeOffset + g_TaskQueueStateUAV[0].PassState[0].VisibleMeshletCount] = meshletPayload;
#else
            if (writeOffset < MAX_VISIBLE_MESHLETS)
                payloadBufferUAV[writeOffset] = meshletPayload;
#endif
#else
            if (writeOffset < MAX_VISIBLE_MESHLETS)
                payloadBufferUAV[writeOffset] = meshletPayload;
#endif
        }
    }

    // Clear immediately for the next pass
    g_MeshletBatchUAV.Store(meshletBatchStartIndex * 4, 0);
}

[shader("compute")]
[numthreads(DAG_CULL_GROUP_SIZE, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
#ifndef DISABLE_HZB_CULL
#ifdef DAG_CULL_PASS1
    const uint passIndex = 1;
#else
    const uint passIndex = 0;
#endif
#else
    const uint passIndex = 0;
#endif
    
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
                    InterlockedAdd(g_TaskQueueStateUAV[0].PassState[passIndex].NodeReadOffset, MAX_BVH_NODES_PER_GROUP, GroupNodeBatchStartIndex);
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
                uint2 nodeData = g_TaskQueueUAV.Load2(nodeQueueIndex * NODE_BYTE_STRIDE);
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
            if ((readyNodeMask & 1u) != 0u)
            {
                uint batchSize = firstbitlow(~readyNodeMask);
                ProcessNodeBatch(batchSize, groupIndex, passIndex);
                if (groupIndex < batchSize)
                {
                    g_TaskQueueUAV.Store2(nodeQueueIndex * NODE_BYTE_STRIDE, uint2(INVALID_ID, INVALID_ID));
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
                InterlockedAdd(g_TaskQueueStateUAV[0].PassState[passIndex].MeshletBatchReadOffset, 1, GroupMeshletBatchStartIndex);
            }
            GroupMemoryBarrierWithGroupSync();
            meshletBatchStartIndex = GroupMeshletBatchStartIndex;
        }

        if (!processNodeQueue && GroupMeshletBatchStartIndex >= MAX_CANDIDATE_MESHLETS_BATCH)
            break;

        if (groupIndex == 0)
        {
            GroupNodeCount = g_TaskQueueStateUAV[0].PassState[passIndex].NodeCount;
            GroupMeshletBatchReadySize = g_MeshletBatchUAV.Load(meshletBatchStartIndex * 4);
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
