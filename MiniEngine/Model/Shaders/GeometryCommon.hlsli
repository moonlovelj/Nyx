#ifndef __GEOMETRY_COMMON_HLSLI__
#define __GEOMETRY_COMMON_HLSLI__

#include "CommonResources.hlsli"
#include "DataCodec.hlsli"

// 返回三角形的meshlet局部索引
uint3 LoadAndUnpackTriangle(ByteAddressBuffer geometryChunksBuffer, uint indexByteOffset, uint localTriID)
{
    uint byteOffset = indexByteOffset + localTriID * 3;

    // 向下对齐到 4 字节边界
    uint alignedOffset = byteOffset & ~3u;

    // 计算位偏移 (Bit Shift)
    // 这是需要的数据距离 alignedOffset 的位距离
    uint bitShift = (byteOffset & 3u) * 8;

    // 一次性读取 2 个 uint (8 字节)
    // 这样能保证即使 3 个字节跨越了 uint 的边界，也被包含在这 64 位里
    uint2 raw = geometryChunksBuffer.Load2(alignedOffset);

    uint64_t data64 = (uint64_t(raw.y) << 32) | uint64_t(raw.x);

    // 把需要的数据“滑”到最低位
    data64 >>= bitShift;

    // 解包 (3 x 8 bits)
    uint i0 = uint(data64 & 0xFF);
    uint i1 = uint((data64 >> 8) & 0xFF);
    uint i2 = uint((data64 >> 16) & 0xFF);

    return uint3(i0, i1, i2);
}

struct VertexAttributes
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 uv0;
    float2 uv1;
};

VertexAttributes LoadVertexAttributes(
    ByteAddressBuffer geometryChunksBuffer, uint vertexByteOffset, 
    uint vertexStride, uint localVertexID, uint psoFlags)
{
    VertexAttributes attrs;
    uint vertexOffset = vertexByteOffset + vertexStride * localVertexID;
    attrs.position = asfloat(geometryChunksBuffer.Load3(vertexOffset));
    vertexOffset += 12;

    uint PackedNormal = geometryChunksBuffer.Load(vertexOffset);
    attrs.normal = DecodeR10G10B10A2UNORMToFloat4(PackedNormal).xyz * 2 - 1;
    vertexOffset += 4;

    if (psoFlags & PSO_HAS_TANGENT)
    {
        uint PackedTangent = geometryChunksBuffer.Load(vertexOffset);
        attrs.tangent = DecodeR10G10B10A2UNORMToFloat4(PackedTangent) * 2 - 1;
        vertexOffset += 4;
    }
    else
    {
        attrs.tangent = float4(0, 0, 1, 1);
    }

    uint PackedUV = geometryChunksBuffer.Load(vertexOffset);
    vertexOffset += 4;
    attrs.uv0 = DecodeR16G16FLOATToFloat2(PackedUV);

    if (psoFlags & PSO_HAS_UV1)
    {
        uint PackedUV1 = geometryChunksBuffer.Load(vertexOffset);
        vertexOffset += 4;
        attrs.uv1 = DecodeR16G16FLOATToFloat2(PackedUV1);
    }
    else
    {
        attrs.uv1 = attrs.uv0;
    }

    // TODO: skinning normal and tangent
    return attrs;
}

#endif