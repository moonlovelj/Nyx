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
    
    // 计算当前线程要处理的Node索引
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
    // 裁剪判定
    bool bVisible = true;
    if (localNodeIndex >= batchSize)
    {
        bVisible = false;
    }
    
    InstanceConstant inst = GetInstanceConstantSRV(instanceIndex);
    MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshBufferIdx);
    const bool bInFrustrum = IsSphereInFrustum(meshInstance.WorldMatrix, node.BoundSphere);
    const bool bTestForLod = !TestForLod(meshInstance.WorldMatrix, ViewerPos, node.MaxParrentError, node.BoundSphere);
    bool bNotOccluded = true;
#ifdef DAG_CULL_PASS0
    bNotOccluded = IsSphereNotOccluded(GetPrevSceneHZBSRV(FrameIndexMod2), PrevViewerPos, 
                            meshInstance.WorldMatrix, PrevViewMatrix, PrevProjMatrix, PrevViewProjMatrix, node.BoundSphere);
#endif
#ifdef DAG_CULL_PASS1
    bNotOccluded = IsSphereNotOccluded(GetCurrentSceneHZBSRV(FrameIndexMod2), ViewerPos, 
                            meshInstance.WorldMatrix, ViewMatrix, ProjMatrix, ViewProjMatrix, node.BoundSphere);
#endif
    
    
    bVisible = bVisible && bInFrustrum && bTestForLod && bNotOccluded;
    
    bool bIsLeaf = node.IsGroup();

    bool bPushNode = bVisible && !bIsLeaf && (childIndex < childNodeCount);
    
    uint nodeWriteOffsetInGroup = 0;
    if (bPushNode)
    {
        // 计算小组内需要推入多少个新 Node
        WaveInterlockedAddScalar(GroupNumCandidateNodes, bPushNode, 1u, nodeWriteOffsetInGroup);
    }

    GroupMemoryBarrierWithGroupSync();

    // 由小组长去全局队列占座 (生产者写入)
    if (groupIndex == 0)
    {
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeWriteOffset, GroupNumCandidateNodes, GroupCandidateNodesOffset);
        InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeCount, GroupNumCandidateNodes);
    }
    AllMemoryBarrierWithGroupSync();

    // 写入新任务到队列
    if (bPushNode)
    {
        uint globalIdx = (GroupCandidateNodesOffset + nodeWriteOffsetInGroup);
        taskQueueUAV.Store2(globalIdx * NODE_BYTE_STRIDE, uint2(instanceIndex, currentChildNodeIndex));
    }

    DeviceMemoryBarrierWithGroupSync();
    
    // 处理叶子节点 (Cluster)
    if (bVisible && bIsLeaf)
    {
        StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
        GroupDataLocation groupDataLocation = groupDataLocationSRV[node.GetGroupIndex()];
        if (groupDataLocation.ChunkIndex == INVALID_ID)
        {
            globallycoherent AppendStructuredBuffer<GeometryStreamingRequest> requestUAV = GetGeometryStreamingRequestBufferUAV();
            GeometryStreamingRequest request;
            request.PackedData = request.PackData(node.GetGroupIndex(), 0);
            requestUAV.Append(request);
        }
        else
        {
        
            uint NumClusters = node.GetMeshletCount();
            uint ClusterIndex = 0;
            WaveInterlockedAdd(taskStateUAV[0].PassState[passIndex].TotalMeshlets, NumClusters, ClusterIndex);

            // 溢出检查：如果当前分配的索引加上要添加的数量超过了 Buffer 最大容量
            const uint ClusterIndexEnd = min(ClusterIndex + NumClusters, MAX_CANDIDATE_MESHLETS);
            // 重新计算实际能存入的数量（防止把 Buffer 写爆导致 GPU 挂起）
            NumClusters = (uint) max((int) ClusterIndexEnd - (int) ClusterIndex, 0);
        
            uint CandidateClustersOffset = 0;
            // 在 CandidateClusters 队列中申请一段连续的空间
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
            // 计算当前索引属于哪一个 Batch (每 64 个一箱)
                const uint BatchIndex = Index / DAG_CULL_GROUP_SIZE;
    
            // 计算下一个对齐到 64 的位置 (即下一个箱子的开头)
                const uint NextIndex = (Index & ~(64 - 1u)) + 64;
    
            // 计算在当前箱子里占了几个坑位
                const uint MaxIndex = min(NextIndex, EndIndex);
                const uint Num = MaxIndex - Index;
    
            // 更新该 Batch 的就绪计数器
                globallycoherent RWByteAddressBuffer meshletBatchBufferUAV = GetMeshletBatchBufferUAV();
                uint temp;
                meshletBatchBufferUAV.InterlockedAdd(BatchIndex * 4, Num, temp);
    
            // 步进到下一个箱子
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
        //float4 meshletSphere = asfloat(geometryChunksBuffer.Load4(meshletHeaderStart
        //+ meshletIndexInGroup * sizeof(MeshletHeader)));
        InstanceConstant inst = GetInstanceConstantSRV(meshletPayload.InstanceIndex);
        MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshBufferIdx);
        const bool bIsInFustrum = IsSphereInFrustum(meshInstance.WorldMatrix, meshletHeader.BoundSphere);
        bool bNotOccluded = true;
#ifdef DAG_CULL_PASS0
        bNotOccluded = IsSphereNotOccluded(GetPrevSceneHZBSRV(FrameIndexMod2), PrevViewerPos, 
                            meshInstance.WorldMatrix, PrevViewMatrix, PrevProjMatrix, PrevViewProjMatrix, meshletHeader.BoundSphere);
#endif
#ifdef DAG_CULL_PASS1
        const bool bNotOccludedPass0 = IsSphereNotOccluded(GetPrevSceneHZBSRV(FrameIndexMod2), PrevViewerPos, 
                            meshInstance.WorldMatrix, PrevViewMatrix, PrevProjMatrix, PrevViewProjMatrix, meshletHeader.BoundSphere);
        
        bNotOccluded = (!bNotOccludedPass0) && IsSphereNotOccluded(GetCurrentSceneHZBSRV(FrameIndexMod2), ViewerPos, 
                            meshInstance.WorldMatrix, ViewMatrix, ProjMatrix, ViewProjMatrix, meshletHeader.BoundSphere);
#endif
        
        bool bTestForLod = true;
        if (meshletHeader.RefineGroupIndex == INVALID_ID)
        {
            bTestForLod = true;
        }
        else
        {
            GroupDataLocation refineGroupDataLocation = groupDataLocationSRV[meshletHeader.RefineGroupIndex];
            ByteAddressBuffer refineGometryChunksBuffer = GetGeometryChunksBufferSRV(refineGroupDataLocation.ChunkIndex);
            GroupHeader refineGroupHeader = refineGometryChunksBuffer.Load < GroupHeader > (refineGroupDataLocation.ByteOffset);
            //float projectError = GetScreenError(meshInstance.WorldMatrix, refineGroupHeader.ParrentError, refineGroupHeader.BoundSphere);
            //bLodCulled = projectError > PIXEL_ERROR_THRESHOLD;
            
            bTestForLod = TestForLod(meshInstance.WorldMatrix, ViewerPos, refineGroupHeader.ParrentError, refineGroupHeader.BoundSphere);
        }
        
        if (!bTestForLod)
        {
            StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
            GroupDataLocation groupDataLocation = groupDataLocationSRV[meshletHeader.RefineGroupIndex];
            if (groupDataLocation.ChunkIndex == INVALID_ID)
                bTestForLod = true;
        }
        
        uint level = meshletHeader.GetLODLevel();
        if (bIsInFustrum && bNotOccluded && bTestForLod)
        {
            globallycoherent RWStructuredBuffer<QueueState> taskStateUAV = GetTaskQueueStateBufferUAV();
            uint writeOffset;
            WaveInterlockedAddScalar(taskStateUAV[0].PassState[passIndex].VisibleMeshletCount, true, 1, writeOffset);
            RWStructuredBuffer<VisibleMeshletPayload> payloadBufferUAV = GetVisibleMeshletBufferUAV();
#ifdef DAG_CULL_PASS1
            // 在Pass0后面开始写
            if (writeOffset + taskStateUAV[0].PassState[0].VisibleMeshletCount < MAX_VISIBLE_MESHLETS)
                payloadBufferUAV[writeOffset + taskStateUAV[0].PassState[0].VisibleMeshletCount] = meshletPayload;
#else
            if (writeOffset < MAX_VISIBLE_MESHLETS)
                payloadBufferUAV[writeOffset] = meshletPayload;
#endif
        }
    }

    // 直接清空为了下一个Pass使用
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
				// 收集新一轮数据
                if (groupIndex == 0)
                {
                    InterlockedAdd(taskStateUAV[0].PassState[passIndex].NodeReadOffset, MAX_BVH_NODES_PER_GROUP, GroupNodeBatchStartIndex);
                }
                
                GroupMemoryBarrierWithGroupSync();

                // 重置Node状态
                NodeBatchReadyOffset = 0;
                // 获取group在task队列中的起始位置
                NodeBatchStartIndex = GroupNodeBatchStartIndex;
                if (NodeBatchStartIndex >= MAX_NODES)
                {
                    // 超过了队列容量限制
                    bProcessNodes = false;
                    continue;
                }
            }
            
            // 计算当前线程的Node索引
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

			// 判断第一个Node数据是否就绪
            if (NodeReadyMask & 1u)
            {
                uint batchSize = firstbitlow(~NodeReadyMask);
                ProcessNodeBatch(batchSize, groupIndex, passIndex);
                if (groupIndex < batchSize)
                {
                    // 直接清空为了下一个Pass使用
                    taskQueueUAV.Store2(NodeIndex * NODE_BYTE_STRIDE, uint2(INVALID_ID, INVALID_ID));
                }

                NodeBatchReadyOffset += batchSize;
                continue;
            }
        }
        
        // 没有node需要处理，去处理cluster        
        // 收集一组新的Cluster批次
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
        if (!bProcessNodes && ClusterBatchReadySize == 0)	// 没有cluster需要处理
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