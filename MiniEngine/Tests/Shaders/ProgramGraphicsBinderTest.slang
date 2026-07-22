// Graphics ProgramBinder and multi-entry-point coverage.

struct GraphicsTestConstants
{
    uint BaseValue;
};

ConstantBuffer<GraphicsTestConstants> g_Constants;
Texture2D<float4> g_Texture;
SamplerState g_Sampler;
RWStructuredBuffer<uint> g_Output;

struct VertexOutput
{
    float4 Position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexID : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };

    VertexOutput output;
    output.Position = float4(positions[vertexID], 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 pixelMain(VertexOutput input) : SV_Target0
{
    uint textureValue = uint(round(g_Texture.SampleLevel(g_Sampler, float2(0.5), 0.0).x));
    g_Output[0] = g_Constants.BaseValue + textureValue;
    return float4(float(g_Output[0]) / 255.0, 0.0, 0.0, 1.0);
}
