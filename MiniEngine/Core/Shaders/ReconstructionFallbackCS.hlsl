Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);
SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    OutputColor.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height)))
        return;
    const float2 uv = (id.xy + 0.5) / float2(width, height);
    OutputColor[id.xy] = InputColor.SampleLevel(LinearClamp, uv, 0);
}
