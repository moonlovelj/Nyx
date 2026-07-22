// Compute-to-graphics root-state switching coverage on one command list.

struct ContextSwitchConstants
{
    uint Value;
};

ConstantBuffer<ContextSwitchConstants> g_Constants;
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    g_Output[0] = g_Constants.Value + 1;
}

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
    g_Output[0] = g_Constants.Value + 2;
    return float4(0.0, float(g_Output[0]) / 255.0, 0.0, 1.0);
}
