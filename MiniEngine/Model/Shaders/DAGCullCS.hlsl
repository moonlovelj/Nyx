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

groupshared uint GroupClusterBatchStartIndex;
groupshared uint GroupClusterBatchReadySize;

void ProcessNodeBatch(uint batchSize, uint groupIndex, uint passIndex)
{
    globallycoherent RWStructuredBuffer<QueueState> taskStateUAV = GetTaskQueueStateBufferUAV();
    globallycoherent RWByteAddressBuffer taskQueueUAV = GetTaskQueueBufferUAV();
    
    // Compute the node index this thread handles
    const uint localNodeIndex = (groupIndex >> MAX_BVH_NODE_CHILDREN_BITS_NUM);
    const uint childIndex = groupIndex & (MAX_BVH_NODE_CHILDREN_NUM - 1);
    const uint fetchIndex = min(localNodeIndex, batchSize - 1);
    
    uint2 nodeData = GroupNodeData[fetchIndex];
    uint instanceIndex = nodeData.x;
    uint hierarchyNodeIndex = nodeData.y;

    StructuredBuffer<HierarchyNode> nodeBufferSRV = GetHierarchyNodesBufferSRV();
    HierarchyNode nodeParent = nodeBufferSRV[hierarchyNodeIndex];

    uint childNodeCount = nodeParent.GetChildNodeCount();
    uint childNodeStartIndex = nodeParent.GetChildNodeStartIndex();
    uint currentChildNodeIndex = childIndex + childNodeStartIndex;
    HierarchyNode node;
    node.Init();

    if (childIndex < childNodeCount)
    {
        node = nodeBufferSRV[currentChildNodeIndex];
    }
    // Culling test
    bool bVisible = true;
    if (localNodeIndex >= batchSize)
    {
        bVisible = false;
    }
    
    InstanceConstant inst = GetInstanceConstantSRV(instanceIndex);
    MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshBufferIdx);

    
    Texture2D<float> hzbTexture;
#ifdef DAG_CULL_PASS0
    hzbTexture = GetPrevSceneHZBSRV(FrameIndexMod2);
    float4 clipMin, clipMax;
    bool bClipValid;
    const bool bInFrustrum = BBoxIntersectFrustum(node.BBoxMin, node.BBoxMax,
                                        meshInstance.WorldMatrix,
                                        PrevViewProjMatrix,
                                        clipMin, clipMax,
                                        bClipValid);
    bVisible = bVisible && bInFrustrum && (!bClipValid || (LargeEnough(clipMin, clipMax, 1.0) && HZBVisible(hzbTexture, clipMin, clipMax)));
#endif
#ifdef DAG_CULL_PASS1
    hzbTexture = GetCurrentSceneHZBSRV(FrameIndexMod2);
    float4 clipMin, clipMax;
    bool bClipValid;
    const bool bInFrustrum = BBoxIntersectFrustum(node.BBoxMin, node.BBoxMax,
                                        meshInstance.WorldMatrix,
                                        ViewProjMatrix,
                                        clipMin, clipMax,
                                        bClipValid);

     bVisible = bVisible && bInFrustrum && (!bClipValid || (LargeEnough(clipMin, clipMax, 1.0) && HZBVisible(hzbTexture, clipMin, clipMax)));
#endif

    bVisible = bVisible &&
    !TestForLod(meshInstance.WorldMatrix, ViewerPos, node.MaxParrentError, node.BoundSphere);
    
    bool bIsLeaf = node.IsGroup();

    bool bPushNode = bVisible && !bIsLeaf && (childIndex < childNodeCount);
    
    uint nodeWriteOffsetInGroup = 0;
    if (bPushNode)
    {
        // Compute how many new nodes to push in this group
        WaveInterlockedAddScalar(GroupNumCandidateNodes, bPushNode, 1u, nodeWriteOffsetInGroup);
    }

    GroupMemoryBarrierWithGroupSync();

    // Group leader reserves space in the global queue (producer write)
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
        taskQueueUAV.Store2(globalIdx * NODE_BYTE_STRIDE, uint2(instanceIndex, currentChildNodeIndex));
    }

    DeviceMemoryBarrierWithGroupSync();
    
    // Process leaf nodes (clusters)
    if (bVisible && bIsLeaf)
    {
        const uint groupID = node.GetGroupIndex();
        
        globallycoherent RWByteAddressBuffer requestMaskUAV = GetGeometryStreamingRequestMaskBufferUAV();
        const uint requestMaskAddress = (groupID >> 5) << 2;
        const uint requestBitMask = 1u << (groupID & 31);
        uint originalTemp;
        requestMaskUAV.InterlockedOr(requestMaskAddress, requestBitMask, originalTemp);
        
        StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
        GroupDataLocation groupDataLocation = groupDataLocationSRV[groupID];

        if (groupDataLocation.ChunkIndex != INVALID_ID)
        {
        
            uint NumClusters = node.GetMeshletCount();
            uint ClusterIndex = 0;
            WaveInterlockedAdd(taskStateUAV[0].PassState[passIndex].TotalMeshlets, NumClusters, ClusterIndex);

            // Overflow check: if allocated index plus new count exceeds buffer capacity
            const uint ClusterIndexEnd = min(ClusterIndex + NumClusters, MAX_CANDIDATE_MESHLETS);
            // Recompute actual storable count (avoid buffer overflow causing GPU hang)
            NumClusters = (uint) max((int) ClusterIndexEnd - (int) ClusterIndex, 0);
        
            uint CandidateClustersOffset = 0;
            // Reserve a contiguous range in the CandidateClusters queue
            WaveInterlockedAdd(taskStateUAV[0].PassState[passIndex].CandidateMeshletWriteOffset, NumClusters, CandidateClustersOffset);
        
            const uint StartIndex = CandidateClustersOffset;
            const uint EndIndex = min(CandidateClustersOffset + NumClusters, MAX_CANDIDATE_MESHLETS);
            for (uint Index = StartIndex; Index < EndIndex; Index++)
            {
                VisibleMeshletPayload meshletPayload;
                meshletPayload.InstanceIndex = instanceIndex;
                meshletPayload.PackedData = meshletPayload.PackData(node.GetGroupIndex(), (Index - StartIndex));
                globallycoherent RWByteAddressBuffer candicateMeshletBufferUAV = GetCandidateMeshletBufferUAV();
                candicateMeshletBufferUAV.Store2(Index * MESHLET_BYTE_STRIDE, uint2(meshletPayload.InstanceIndex, meshletPayload.PackedData));
            }
        
            DeviceMemoryBarrier();
        
            for (uint Index = StartIndex; Index < EndIndex;)
            {
            // Compute which batch this index belongs to (one bin per 64)
                const uint BatchIndex = Index / DAG_CULL_GROUP_SIZE;
    
            // Compute the next 64-aligned index (start of next bin)
                const uint NextIndex = (Index & ~(64 - 1u)) + 64;
    
            // Compute how many slots are used in the current bin
                const uint MaxIndex = min(NextIndex, EndIndex);
                const uint Num = MaxIndex - Index;
    
            // Update the ready counter for this batch
                globallycoherent RWByteAddressBuffer meshletBatchBufferUAV = GetMeshletBatchBufferUAV();
                uint temp;
                meshletBatchBufferUAV.InterlockedAdd(BatchIndex * 4, Num, temp);
    
            // Advance to the next bin
                Index = NextIndex;
            }
        }
    }
    
    DeviceMemoryBarrierWithGroupSync();
    
    if (groupIndex == 0)
    {
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeCount, -batchSize);
    }
}

void ProcessClusterBatch(uint clusterBatchStartIndex, uint clusterBatchReadySize, uint groupIndex, uint passIndex)
{
    if (groupIndex < clusterBatchReadySize)
    {
        const uint CandidateIndex = clusterBatchStartIndex * DAG_CULL_GROUP_SIZE + groupIndex;
        globallycoherent RWByteAddressBuffer candicateMeshletBufferUAV = GetCandidateMeshletBufferUAV();
        VisibleMeshletPayload meshletPayload;
        const uint2 loadedMeshlet = candicateMeshletBufferUAV.Load2(CandidateIndex * MESHLET_BYTE_STRIDE);
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
        bool bAlphaBlend = (meshletHeader.GetPSOFlags() & PSO_ALPHA_BLEND);
        float4 clipMin, clipMax;
        bool bClipValid;
        bool bClusterVisible = BBoxIntersectFrustum(meshletHeader.BBoxMin, meshletHeader.BBoxMax,
                                        meshInstance.WorldMatrix,
                                        PrevViewProjMatrix,
                                        clipMin, clipMax,
                                        bClipValid);

        bClusterVisible = bClusterVisible && (!bClipValid || (LargeEnough(clipMin, clipMax, 1.0) &&
            HZBVisible(GetPrevSceneHZBSRV(FrameIndexMod2), clipMin, clipMax)));
        
        if (!bClusterVisible)
        {

#ifdef DAG_CULL_PASS1

        bClusterVisible = BBoxIntersectFrustum(meshletHeader.BBoxMin, meshletHeader.BBoxMax,
                                        meshInstance.WorldMatrix,
                                        ViewProjMatrix,
                                        clipMin, clipMax,
                                        bClipValid);
            
        bClusterVisible = bClusterVisible && (!bClipValid || (LargeEnough(clipMin, clipMax, 1.0) &&
            HZBVisible(GetCurrentSceneHZBSRV(FrameIndexMod2), clipMin, clipMax)));
#endif
        }
        
        bClusterVisible = bClusterVisible && !bAlphaBlend;

        if (bClusterVisible && meshletHeader.RefineGroupIndex != INVALID_ID)
        {
            GroupDataLocation refineGroupDataLocation = groupDataLocationSRV[meshletHeader.RefineGroupIndex];
            if (refineGroupDataLocation.ChunkIndex != INVALID_ID)
            {
                ByteAddressBuffer refineGometryChunksBuffer = GetGeometryChunksBufferSRV(refineGroupDataLocation.ChunkIndex);
                GroupHeader refineGroupHeader = refineGometryChunksBuffer.Load < GroupHeader > (refineGroupDataLocation.ByteOffset);
                bClusterVisible = TestForLod(meshInstance.WorldMatrix, ViewerPos, refineGroupHeader.ParrentError, refineGroupHeader.BoundSphere);
            }
        }

        uint level = meshletHeader.GetLODLevel();
        if (bClusterVisible)
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
    meshletBatchBufferUAV.Store(clusterBatchStartIndex * 4, 0);
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
    
    bool bProcessNodes = true;
    uint NodeBatchReadyOffset = MAX_BVH_NODES_PER_GROUP;
    uint NodeBatchStartIndex = 0;
    uint ClusterBatchStartIndex = INVALID_ID;
    
    while (true)
    {
        GroupMemoryBarrierWithGroupSync();
        if (groupIndex == 0)
        {
            GroupNumCandidateNodes = 0;
            GroupNodeMask = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        
        uint NodeReadyMask = 0;
        if (bProcessNodes)
        {
            if (NodeBatchReadyOffset == MAX_BVH_NODES_PER_GROUP)
            {
				// Collect a new batch of data
                if (groupIndex == 0)
                {
                    InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeReadOffset, MAX_BVH_NODES_PER_GROUP, GroupNodeBatchStartIndex);
                }
                
                GroupMemoryBarrierWithGroupSync();

                // Reset node state
                NodeBatchReadyOffset = 0;
                // Get the group's start index in the task queue
                NodeBatchStartIndex = GroupNodeBatchStartIndex;
                if (NodeBatchStartIndex >= MAX_NODES)
                {
                    // Exceeded queue capacity limit
                    bProcessNodes = false;
                    continue;
                }
            }
            
            // Compute this thread's node index
            const uint NodeIndex = NodeBatchStartIndex + NodeBatchReadyOffset + groupIndex;
            bool bNodeReady = (NodeBatchReadyOffset + groupIndex < MAX_BVH_NODES_PER_GROUP);
            if (bNodeReady)
            {
                uint2 nodeData = taskQueueUAV.Load2(NodeIndex * NODE_BYTE_STRIDE);
                bNodeReady = nodeData.x != INVALID_ID && nodeData.y != INVALID_ID;
                if (bNodeReady)
                {
                    GroupNodeData[groupIndex] = nodeData;
                }

            }
            if (bNodeReady)
            {
                InterlockedOr(GroupNodeMask, 1u << groupIndex);
            }
            AllMemoryBarrierWithGroupSync();
            
            NodeReadyMask = GroupNodeMask;

			// Check whether the first node data is ready
            if (NodeReadyMask & 1u)
            {
                uint batchSize = firstbitlow(~NodeReadyMask);
                ProcessNodeBatch(batchSize, groupIndex, passIndex);
                if (groupIndex < batchSize)
                {
                    // Clear immediately for the next pass
                    taskQueueUAV.Store2(NodeIndex * NODE_BYTE_STRIDE, uint2(INVALID_ID, INVALID_ID));
                }

                NodeBatchReadyOffset += batchSize;
                continue;
            }
        }
        
        // No nodes to process; handle clusters
        // Collect a new cluster batch
        if (ClusterBatchStartIndex == INVALID_ID)
        {
            if (groupIndex == 0)
            {
                InterlockedAdd(taskStateUAV[0].PassState[passIndex].MeshletBatchReadOffset, 1, GroupClusterBatchStartIndex);
            }
            GroupMemoryBarrierWithGroupSync();
            ClusterBatchStartIndex = GroupClusterBatchStartIndex;
        }

        if (!bProcessNodes && GroupClusterBatchStartIndex >= MAX_CANDIDATE_MESHLETS_BATCH)
            break;

        if (groupIndex == 0)
        {
            GroupNodeCount = taskStateUAV[0].PassState[passIndex].NodeCount;
            globallycoherent RWByteAddressBuffer meshletBatchUAV = GetMeshletBatchBufferUAV();
            GroupClusterBatchReadySize = meshletBatchUAV.Load(ClusterBatchStartIndex * 4);
        }
        GroupMemoryBarrierWithGroupSync();

        uint ClusterBatchReadySize = GroupClusterBatchReadySize;
        if (!bProcessNodes && ClusterBatchReadySize == 0)	// No clusters to process
            break;

        if ((bProcessNodes && ClusterBatchReadySize == DAG_CULL_GROUP_SIZE) || (!bProcessNodes && ClusterBatchReadySize > 0))
        {
            ProcessClusterBatch(ClusterBatchStartIndex, ClusterBatchReadySize, groupIndex, passIndex);
            ClusterBatchStartIndex = INVALID_ID;
        }

        if (bProcessNodes && GroupNodeCount == 0)
        {
            bProcessNodes = false;
        }
    }
}

#endif
