// Reflection-only test for Slang DescriptorHandle layout and bindless flags.

struct ProgramDescriptorHandleConstants
{
    DescriptorHandle<Texture2D<float4>> TextureHandle;
};

ConstantBuffer<ProgramDescriptorHandleConstants> g_Constants;
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    uint2 handleValue = uint2(g_Constants.TextureHandle);
    g_Output[0] = handleValue.x ^ handleValue.y;
}
