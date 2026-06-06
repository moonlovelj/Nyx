// End-to-end test for reflection-driven ordinary-data packing.

struct ProgramValuePackingConstants
{
    float FloatValue;
    int IntValue;
    uint UIntValue;
    bool BoolValue;

    int2 XMINT2Value;
    uint2 XMUINT2Value;
    int3 XMINT3Value;
    uint3 XMUINT3Value;
    int4 XMINT4Value;
    uint4 XMUINT4Value;

    float ScalarValue;
    float2 XMFLOAT2Value;
    float XMFLOAT2Padding;

    float3 XMFLOAT3Value;
    float XMFLOAT3Guard;
    float3 Vector3Value;
    float Vector3Guard;

    float4 XMFLOAT4Value;
    float4 Vector4Value;

    float FloatArray[2];
    float2 Float2Array[2];
    float3 Vector3Array[2];

    float3x3 ColumnMatrix3;
    float ColumnMatrix3Guard;
    row_major float3x3 RowMatrix3;
    float RowMatrix3Guard;
    float4x4 Matrix4Value;
    float3x3 Matrix3Array[2];

    uint2 Handle;
};

struct ProgramValuePackingNested
{
    float3 Value;
    float Guard;
};

struct ProgramValuePackingBlock
{
    ProgramValuePackingNested NestedArray[2];
    float2 Float2Array[2];
};

struct ProgramValuePackingRoot
{
    float3 Value;
    float Guard;
    uint UIntValue;
    bool BoolValue;
};

ConstantBuffer<ProgramValuePackingConstants> g_Constants;
ParameterBlock<ProgramValuePackingBlock> g_Block;
ParameterBlock<ProgramValuePackingRoot> g_Root;
RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    uint outputIndex = 0;

    g_Output[outputIndex++] = asuint(g_Constants.FloatValue);
    g_Output[outputIndex++] = asuint(g_Constants.IntValue);
    g_Output[outputIndex++] = g_Constants.UIntValue;
    g_Output[outputIndex++] = g_Constants.BoolValue ? 1u : 0u;

    for (uint component = 0; component < 2; ++component)
        g_Output[outputIndex++] = asuint(g_Constants.XMINT2Value[component]);
    for (uint component = 0; component < 2; ++component)
        g_Output[outputIndex++] = g_Constants.XMUINT2Value[component];
    for (uint component = 0; component < 3; ++component)
        g_Output[outputIndex++] = asuint(g_Constants.XMINT3Value[component]);
    for (uint component = 0; component < 3; ++component)
        g_Output[outputIndex++] = g_Constants.XMUINT3Value[component];
    for (uint component = 0; component < 4; ++component)
        g_Output[outputIndex++] = asuint(g_Constants.XMINT4Value[component]);
    for (uint component = 0; component < 4; ++component)
        g_Output[outputIndex++] = g_Constants.XMUINT4Value[component];

    g_Output[outputIndex++] = asuint(g_Constants.ScalarValue);

    g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT2Value.x);
    g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT2Value.y);
    g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT2Padding);

    g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT3Value.x);
    g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT3Value.y);
    g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT3Value.z);
    g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT3Guard);
    g_Output[outputIndex++] = asuint(g_Constants.Vector3Value.x);
    g_Output[outputIndex++] = asuint(g_Constants.Vector3Value.y);
    g_Output[outputIndex++] = asuint(g_Constants.Vector3Value.z);
    g_Output[outputIndex++] = asuint(g_Constants.Vector3Guard);

    for (uint component = 0; component < 4; ++component)
        g_Output[outputIndex++] = asuint(g_Constants.XMFLOAT4Value[component]);
    for (uint component = 0; component < 4; ++component)
        g_Output[outputIndex++] = asuint(g_Constants.Vector4Value[component]);

    for (uint element = 0; element < 2; ++element)
        g_Output[outputIndex++] = asuint(g_Constants.FloatArray[element]);
    for (uint element = 0; element < 2; ++element)
    {
        for (uint component = 0; component < 2; ++component)
            g_Output[outputIndex++] = asuint(g_Constants.Float2Array[element][component]);
    }
    for (uint element = 0; element < 2; ++element)
    {
        for (uint component = 0; component < 3; ++component)
            g_Output[outputIndex++] = asuint(g_Constants.Vector3Array[element][component]);
    }

    for (uint row = 0; row < 3; ++row)
    {
        for (uint column = 0; column < 3; ++column)
            g_Output[outputIndex++] = asuint(g_Constants.ColumnMatrix3[row][column]);
    }
    g_Output[outputIndex++] = asuint(g_Constants.ColumnMatrix3Guard);

    for (uint row = 0; row < 3; ++row)
    {
        for (uint column = 0; column < 3; ++column)
            g_Output[outputIndex++] = asuint(g_Constants.RowMatrix3[row][column]);
    }
    g_Output[outputIndex++] = asuint(g_Constants.RowMatrix3Guard);

    for (uint row = 0; row < 4; ++row)
    {
        for (uint column = 0; column < 4; ++column)
            g_Output[outputIndex++] = asuint(g_Constants.Matrix4Value[row][column]);
    }
    for (uint element = 0; element < 2; ++element)
    {
        for (uint row = 0; row < 3; ++row)
        {
            for (uint column = 0; column < 3; ++column)
                g_Output[outputIndex++] = asuint(g_Constants.Matrix3Array[element][row][column]);
        }
    }

    uint2 handleValue = g_Constants.Handle;
    g_Output[outputIndex++] = handleValue.x;
    g_Output[outputIndex++] = handleValue.y;

    for (uint element = 0; element < 2; ++element)
    {
        g_Output[outputIndex++] = asuint(g_Block.NestedArray[element].Value.x);
        g_Output[outputIndex++] = asuint(g_Block.NestedArray[element].Value.y);
        g_Output[outputIndex++] = asuint(g_Block.NestedArray[element].Value.z);
        g_Output[outputIndex++] = asuint(g_Block.NestedArray[element].Guard);
    }
    for (uint element = 0; element < 2; ++element)
    {
        g_Output[outputIndex++] = asuint(g_Block.Float2Array[element].x);
        g_Output[outputIndex++] = asuint(g_Block.Float2Array[element].y);
    }

    g_Output[outputIndex++] = asuint(g_Root.Value.x);
    g_Output[outputIndex++] = asuint(g_Root.Value.y);
    g_Output[outputIndex++] = asuint(g_Root.Value.z);
    g_Output[outputIndex++] = asuint(g_Root.Guard);
    g_Output[outputIndex++] = g_Root.UIntValue;
    g_Output[outputIndex++] = g_Root.BoolValue ? 1u : 0u;
}
