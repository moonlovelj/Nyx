#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "CullingCommon.hlsli"

float CalcMipLevel(float2 uvRectSize)
{
    float2 pixelSize = uvRectSize * float2(ViewportWidth, ViewportHeight);
    float maxDim = max(pixelSize.x, pixelSize.y);
    float logicalMip = floor(log2(max(maxDim, 1.0)));
    float hardwareMip = logicalMip - 1.0;
    return max(hardwareMip, 0.0);
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
    
    float4 clipPos = mul(vpMatrix, float4(sphereWS.xyz, 1.0));
    float viewDepth = clipPos.w;
    
    float safeDepth = max(viewDepth, 0.001);
    float radiusUV = abs(sphereWS.w * max(abs(vpMatrix._m00), abs(vpMatrix._m11)) / safeDepth * 0.5);
    
    float3 ndc = clipPos.xyz / clipPos.w;
    float2 centerUV = ndc.xy * 0.5 + 0.5;
    centerUV.y = 1.0 - centerUV.y;
    
    float4 uvRect = float4(
        centerUV.x - radiusUV,
        centerUV.y - radiusUV,
        centerUV.x + radiusUV,
        centerUV.y + radiusUV
    );
    
    // 视锥 XY 平面剔除 (球完全在屏幕左/右/上/下 之外)
    if (uvRect.z < 0 || uvRect.x > 1 || uvRect.w < 0 || uvRect.y > 1) 
        return false;
    
    uvRect = saturate(uvRect);
    
    float3 viewDir = normalize(sphereWS.xyz-viewPos);
    float3 nearPointWorld = sphereWS.xyz - viewDir * sphereWS.w;
    float4 nearPointClip = mul(vpMatrix, float4(nearPointWorld, 1.0));
    float sphereClosestDepth = nearPointClip.z / nearPointClip.w;
    
    float mip = CalcMipLevel(uvRect.zw - uvRect.xy);
    float2 HZBUVScale = float2((ViewportWidth * 0.5f) * HZBSizeAndInv.z, (ViewportHeight * 0.5f) * HZBSizeAndInv.w);
    float4 texUVRect = uvRect * HZBUVScale.xyxy;

    float d1 = hzb.SampleLevel(pointSampler, texUVRect.xy, mip).r;
    float d2 = hzb.SampleLevel(pointSampler, texUVRect.zw, mip).r;
    float d3 = hzb.SampleLevel(pointSampler, texUVRect.xw, mip).r;
    float d4 = hzb.SampleLevel(pointSampler, texUVRect.zy, mip).r;
    
    float hzbMinDepth = min(min(d1, d2), min(d3, d4));
    return sphereClosestDepth >= hzbMinDepth;
}

[numthreads(CULLING_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint index = (groupId.x * CULLING_THREAD_GROUP_SIZE) + groupIndex;
    if (index < maxCommands)
    {
        RWByteAddressBuffer visibleFlagUAV = GetVisibleFlagUAV(psoIdx);
        IndirectCommand inCommand = GetIndirectCommandBufferSRV(indirectBufferOffset, index + startCommand);

        MeshletConstant meshletConstant = GetMeshletConstantSRV(inCommand.MeshletIndex);
        InstanceConstant instanceConstant = GetInstanceConstantSRV(inCommand.InstanceIndex);
        MeshConstant meshConstant = GetMeshConstantSRV(instanceConstant.MeshConstantsBase + meshletConstant.MeshConstantsIndexOffset);
        float4x4 WorldMatrix = meshConstant.WorldMatrix;
        
        uint visible = visibleFlagUAV.Load(index * 4);
        
//#define OCCLUSION_CULL_PASS1 1
//#define OCCLUSION_CULL_PASS2 1
        
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