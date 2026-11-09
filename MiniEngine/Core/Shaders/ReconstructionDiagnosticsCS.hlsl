Texture2D<float4> Source : register(t0);
Texture2D<float> ExposureTexture : register(t1);
RWTexture2D<float4> Output : register(u0);

cbuffer Constants : register(b0)
{
    uint View;
    uint OutputWidth;
    uint OutputHeight;
    float NearClip;
    float FarClip;
    float MotionRange;
    float DepthRange;
    float Padding;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= OutputWidth || id.y >= OutputHeight)
        return;
    uint width, height;
    Source.GetDimensions(width, height);
    uint2 pixel = min(uint2((id.xy + 0.5) / float2(OutputWidth, OutputHeight) * float2(width, height)),
        uint2(width - 1, height - 1));
    float4 sampleValue = Source.Load(int3(pixel, 0));
    float exposure = ExposureTexture[uint2(0, 0)];
    float3 color;
    if (!all(isfinite(sampleValue)) || !isfinite(exposure))
        color = float3(1, 0, 1);
    else if (View == 1)
    {
        float distance = NearClip * FarClip / (NearClip + sampleValue.x * (FarClip - NearClip));
        color = sampleValue.x == 0 ? 0 : 1 - saturate(distance / DepthRange);
    }
    else if (View == 2)
        color = float3(saturate(0.5 + sampleValue.xy / (2 * MotionRange)), 0.5);
    else if (View == 3)
        color = saturate((log2(max(sampleValue.x, 1e-6)) + 10) / 20);
    else
    {
        float3 hdr = max(sampleValue.rgb, 0) * max(exposure, 0);
        color = hdr / (1 + hdr);
    }
    // Linear display color; Present performs SDR/HDR output encoding.
    Output[id.xy] = float4(color, 1);
}
