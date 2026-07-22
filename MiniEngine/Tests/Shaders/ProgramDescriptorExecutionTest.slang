// End-to-end descriptor-array consumption coverage.

struct DescriptorTestConstants
{
    uint Bias;
};

ConstantBuffer<DescriptorTestConstants> g_Constants;
StructuredBuffer<uint> g_Inputs[2];
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    g_Output[0] = g_Constants.Bias + g_Inputs[0][0] * 10 + g_Inputs[1][0];
}
