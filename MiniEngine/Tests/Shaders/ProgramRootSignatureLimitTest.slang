// Negative coverage for Nyx's reflected root-signature limits.

#ifndef ROOT_SIGNATURE_TEST_CASE
#define ROOT_SIGNATURE_TEST_CASE 0
#endif

struct SmallConstants
{
    uint Value;
};

#if ROOT_SIGNATURE_TEST_CASE == 1

ConstantBuffer<SmallConstants> g_CB0;
ConstantBuffer<SmallConstants> g_CB1;
ConstantBuffer<SmallConstants> g_CB2;
ConstantBuffer<SmallConstants> g_CB3;
ConstantBuffer<SmallConstants> g_CB4;
ConstantBuffer<SmallConstants> g_CB5;
ConstantBuffer<SmallConstants> g_CB6;
ConstantBuffer<SmallConstants> g_CB7;
ConstantBuffer<SmallConstants> g_CB8;
ConstantBuffer<SmallConstants> g_CB9;
ConstantBuffer<SmallConstants> g_CB10;
ConstantBuffer<SmallConstants> g_CB11;
ConstantBuffer<SmallConstants> g_CB12;
ConstantBuffer<SmallConstants> g_CB13;
ConstantBuffer<SmallConstants> g_CB14;
ConstantBuffer<SmallConstants> g_CB15;
ConstantBuffer<SmallConstants> g_CB16;
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    g_Output[0] = g_CB0.Value + g_CB1.Value + g_CB2.Value + g_CB3.Value +
        g_CB4.Value + g_CB5.Value + g_CB6.Value + g_CB7.Value +
        g_CB8.Value + g_CB9.Value + g_CB10.Value + g_CB11.Value +
        g_CB12.Value + g_CB13.Value + g_CB14.Value + g_CB15.Value + g_CB16.Value;
}

#elif ROOT_SIGNATURE_TEST_CASE == 2

struct OversizedRootConstants
{
    uint4 Values[16];
};

ParameterBlock<OversizedRootConstants> g_Root;
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    g_Output[0] = g_Root.Values[0].x;
}

#elif ROOT_SIGNATURE_TEST_CASE == 3

StructuredBuffer<uint> g_Inputs[33];
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    g_Output[0] = g_Inputs[0][0];
}

#elif ROOT_SIGNATURE_TEST_CASE == 4

StructuredBuffer<uint> g_First : register(t0, space0);
StructuredBuffer<uint> g_Second : register(t0, space0);
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    g_Output[0] = g_First[0] + g_Second[0];
}

#else

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
}

#endif
