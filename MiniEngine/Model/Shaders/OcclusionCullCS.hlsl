#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "CullingCommon.hlsli"

#define FLT_MAX 3.402823466e+38

float GetMipLevel(float2 texelRectSize)
{
    float maxDim = max(texelRectSize.x, texelRectSize.y);
    float logicalMip = floor(log2(max(maxDim, 1.0)));
    return max(logicalMip, 0.0);
}

float4 ComputeAABBUVRect(float3 boxMin, float3 boxMax, float4x4 vpMatrix)
{
    float3 corners[8];
    corners[0] = float3(boxMin.x, boxMin.y, boxMin.z);
    corners[1] = float3(boxMax.x, boxMin.y, boxMin.z);
    corners[2] = float3(boxMin.x, boxMax.y, boxMin.z);
    corners[3] = float3(boxMax.x, boxMax.y, boxMin.z);
    corners[4] = float3(boxMin.x, boxMin.y, boxMax.z);
    corners[5] = float3(boxMax.x, boxMin.y, boxMax.z);
    corners[6] = float3(boxMin.x, boxMax.y, boxMax.z);
    corners[7] = float3(boxMax.x, boxMax.y, boxMax.z);
    
    float2 uvMin = float2(FLT_MAX, FLT_MAX);
    float2 uvMax = float2(-FLT_MAX, -FLT_MAX);

    uint validCount = 0;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        float4 clip = mul(vpMatrix, float4(corners[i], 1.0));
        
        if (clip.w <= 0.0f)
            continue;

        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * 0.5f + 0.5f;
        uv.y = 1.0f - uv.y;

        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        validCount++;
    }

    if (validCount == 0)
    {
        return float4(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    }
    
    if (uvMax.x < 0 || uvMin.x > 1 || uvMax.y < 0 || uvMin.y > 1)
    {
        return float4(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    }

    float4 uvRect = float4(uvMin.x, uvMin.y, uvMax.x, uvMax.y);
    uvRect = saturate(uvRect);

    return uvRect;
}

float SampleHZBMinDepth(Texture2D<float> hzb, float4 texUVRect, float mip)
{
    float2 rectSize = float2(texUVRect.z - texUVRect.x, texUVRect.w - texUVRect.y);
    // 采用4x4采样网格，避免深度空洞产生的错误剔除
    const uint grid = 4;
    float2 step = rectSize / (grid - 1);
    float minDepth = FLT_MAX;
    float2 start = float2(texUVRect.x, texUVRect.y);

    [unroll]
    for (uint iy = 0; iy < grid; ++iy)
    {
        float v = start.y + step.y * iy;
        [unroll]
        for (uint ix = 0; ix < grid; ++ix)
        {
            float u = start.x + step.x * ix;
            float d = hzb.SampleLevel(pointSampler, float2(u, v), mip).r;
            minDepth = min(minDepth, d);
        }
    }
    return minDepth;
}

bool IsSphereVisible(
    Texture2D<float> hzb,
    float3 viewPos,
    float4x4 vpMatrix,
    float4x4 WorldMatrix, 
    float4 sphereLS)
{
    float3 column0 = float3(WorldMatrix._m00, WorldMatrix._m10, WorldMatrix._m20);
    float3 column1 = float3(WorldMatrix._m01, WorldMatrix._m11, WorldMatrix._m21);
    float3 column2 = float3(WorldMatrix._m02, WorldMatrix._m12, WorldMatrix._m22);
    float sphereScale = max(length(column0), max(length(column1), length(column2)));
    float4 sphereWS = float4(mul(WorldMatrix, float4(sphereLS.xyz, 1)).xyz, sphereScale * sphereLS.w);
    
    float3 boxMin = sphereWS.xyz - sphereWS.w;
    float3 boxMax = sphereWS.xyz + sphereWS.w;
    float4 uvRect = ComputeAABBUVRect(boxMin, boxMax, vpMatrix);

    // 如何矩形不合法（比如和近裁剪面相交等），就采用保守策略，不剔除，因为经过视锥剔除，这个meshlet肯定在视锥内
    if (uvRect.z < 0 || uvRect.x > 1 || uvRect.w < 0 || uvRect.y > 1)
        return true;

    float2 HZBUVScale = float2((ViewportWidth * 0.5f) * HZBSizeAndInv.z, (ViewportHeight * 0.5f) * HZBSizeAndInv.w);
    float4 texUVRect = uvRect * HZBUVScale.xyxy;
    float2 texelRectSize = float2(texUVRect.z - texUVRect.x, texUVRect.w - texUVRect.y) * HZBSizeAndInv.xy;
    float mip = GetMipLevel(texelRectSize);
    float hzbMinDepth = SampleHZBMinDepth(hzb, texUVRect, mip);
    
    float3 viewDir = normalize(sphereWS.xyz - viewPos);
    float3 nearPointWorld = sphereWS.xyz - viewDir * sphereWS.w;
    float4 nearPointClip = mul(vpMatrix, float4(nearPointWorld, 1.0));
    float sphereClosestDepth = nearPointClip.z / max(nearPointClip.w, 1e-6);
    return sphereClosestDepth >= hzbMinDepth;
}

[numthreads(CULLING_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint index = (groupId.x * CULLING_THREAD_GROUP_SIZE) + groupIndex;
    if (index < MaxCommands)
    {
        RWByteAddressBuffer visibleFlagUAV = GetIndirectVisibleFlagsBufferUAV(PsoIdx);
        IndirectCommand inCommand = GetIndirectCommandsBufferSRV(PsoIdx, index);

        MeshletConstant meshletConstant = GetMeshletConstantSRV(inCommand.MeshletIndex);
        InstanceConstant instanceConstant = GetInstanceConstantSRV(inCommand.InstanceIndex);
        MeshConstant meshConstant = GetMeshConstantSRV(instanceConstant.MeshConstantsBase + meshletConstant.MeshConstantsIndexOffset);
        float4x4 WorldMatrix = meshConstant.WorldMatrix;
        
        uint visible = visibleFlagUAV.Load(index * 4);
           
#ifdef OCCLUSION_CULL_PASS1

        // Pass1
        if (visible & CULLING_FRUSTUM_VISIBLE &&
            IsSphereVisible(GetPrevSceneHZBSRV(FrameIndexMod2), PrevViewerPos, PrevViewProjMatrix, WorldMatrix, meshletConstant.BoundingSphere))
        {
            visibleFlagUAV.Store(index * 4, visible | CULLING_OCCLUSION_PASS1_VISIBLE);
        }
#endif
        
#ifdef OCCLUSION_CULL_PASS2
        
        // Pass2
        if ((visible & CULLING_FRUSTUM_VISIBLE) && !(visible & CULLING_OCCLUSION_PASS1_VISIBLE) &&
            IsSphereVisible(GetCurrentSceneHZBSRV(FrameIndexMod2), ViewerPos, ViewProjMatrix, WorldMatrix, meshletConstant.BoundingSphere))
        {
            visibleFlagUAV.Store(index * 4, visible | CULLING_OCCLUSION_PASS2_VISIBLE);
        }
#endif

    }
}