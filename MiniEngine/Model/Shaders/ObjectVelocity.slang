#include "CommonResources.slang"
#include "ObjectMotionShared.h.slang"
#include "../../Core/Shaders/PixelPacking_Velocity.hlsli"

struct ObjectVelocityConstants
{
    float4x4 InverseViewProjMatrix;
    float4x4 PrevViewProjMatrix;
    uint2 ViewportSize;
    float2 InvViewportSize;
    float2 JitterDelta;
    float RcpFarClip;
    uint MotionCount;
};

ConstantBuffer<ObjectVelocityConstants> g_ObjectVelocity;
Texture2D<uint2> g_VBuffer;
Texture2D<float> g_LinearDepth;
StructuredBuffer<VisibleMeshletPayload> g_VisibleMeshlets;
ByteAddressBuffer g_Instances;
StructuredBuffer<ObjectMotionData> g_ObjectMotion;
RWTexture2D<uint> g_Velocity;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= g_ObjectVelocity.ViewportSize))
        return;
    const uint2 visibility = g_VBuffer[pixel];
    if (visibility.y == 0)
        return;

    const uint commandIndex = visibility.x >> 7;
    const VisibleMeshletPayload payload = g_VisibleMeshlets[commandIndex];
    const InstanceConstant instance = LoadInstanceConstant(g_Instances, payload.InstanceIndex);
    if (instance.MeshBufferIdx >= g_ObjectVelocity.MotionCount)
    {
        g_Velocity[pixel] = INVALID_VELOCITY;
        return;
    }
    const ObjectMotionData motion = g_ObjectMotion[instance.MeshBufferIdx];
    if (motion.Flags == OBJECT_MOTION_STATIC)
        return;

    // Keep invalid correspondences out of TAA while retaining zero XY for motion blur.
    g_Velocity[pixel] = INVALID_VELOCITY;
    if (motion.Flags != OBJECT_MOTION_VALID)
        return;

    const float2 currentPixel = pixel + 0.5;
    const float2 ndc = currentPixel * g_ObjectVelocity.InvViewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float4 worldH = mul(g_ObjectVelocity.InverseViewProjMatrix, float4(ndc, asfloat(visibility.y), 1.0));
    if (!all(isfinite(worldH)) || abs(worldH.w) <= 1e-20)
        return;
    const float4 previousWorld = mul(motion.WorldToPreviousWorld, float4(worldH.xyz / worldH.w, 1.0));
    const float4 previousClip = mul(g_ObjectVelocity.PrevViewProjMatrix, previousWorld);
    if (!all(isfinite(previousClip)) || previousClip.w <= 1e-6 ||
        previousClip.z < 0.0 || previousClip.z > previousClip.w)
        return;
    const float2 previousUV = previousClip.xy / previousClip.w * float2(0.5, -0.5) + 0.5;
    if (any(previousUV < 0.0) || any(previousUV >= 1.0))
        return;

    const float3 velocity = float3(
        previousUV * g_ObjectVelocity.ViewportSize - currentPixel - g_ObjectVelocity.JitterDelta,
        previousClip.w * g_ObjectVelocity.RcpFarClip - g_LinearDepth[pixel]);
    if (all(isfinite(velocity)))
        g_Velocity[pixel] = PackVelocity(velocity);
}
