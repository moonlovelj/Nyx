#ifndef NYX_VIEW_COMMON_H_SLANG
#define NYX_VIEW_COMMON_H_SLANG

float3 ReconstructWorldPosition(
    uint2 pixelPosition,
    float sceneDepth,
    uint2 viewportSize,
    float4x4 inverseViewProjMatrix)
{
    float2 pixelCenter = float2(pixelPosition) + 0.5f;
    float2 ndcPosition;
    ndcPosition.x = pixelCenter.x / float(viewportSize.x) * 2.0f - 1.0f;
    ndcPosition.y = 1.0f - pixelCenter.y / float(viewportSize.y) * 2.0f;

    float4 worldPosition = mul(inverseViewProjMatrix, float4(ndcPosition, sceneDepth, 1.0f));
    return worldPosition.xyz / worldPosition.w;
}

#endif // NYX_VIEW_COMMON_H_SLANG
