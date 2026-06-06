#ifndef __GEOMETRY_COMMON_SLANG__
#define __GEOMETRY_COMMON_SLANG__

#include "DataCodec.h.slang"

// Return the triangle's meshlet-local indices
uint3 LoadAndUnpackTriangle(ByteAddressBuffer geometryChunksBuffer, uint indexByteOffset, uint localTriID)
{
    uint byteOffset = indexByteOffset + localTriID * 3;

    // Align down to a 4-byte boundary
    uint alignedOffset = byteOffset & ~3u;

    // Compute bit shift
    // This is the bit distance from alignedOffset to the desired data
    uint bitShift = (byteOffset & 3u) * 8;

    // Read two uints at once (8 bytes)
    // This ensures that even if 3 bytes cross a uint boundary, they are in these 64 bits
    uint2 raw = geometryChunksBuffer.Load2(alignedOffset);

    uint64_t data64 = (uint64_t(raw.y) << 32) | uint64_t(raw.x);

    // Shift the desired data down to the lowest bits
    data64 >>= bitShift;

    // Unpack (3 x 8 bits)
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

    if ((psoFlags & PSO_HAS_TANGENT) != 0)
    {
        uint PackedTangent = geometryChunksBuffer.Load(vertexOffset);
        attrs.tangent = DecodeR10G10B10A2UNORMToFloat4(PackedTangent) * 2 - 1;
        vertexOffset += 4;
    }
    else
    {
        attrs.tangent = float4(0, 0, 1, 1);
    }

    if ((psoFlags & PSO_HAS_UV0) != 0)
    {
        uint PackedUV = geometryChunksBuffer.Load(vertexOffset);
        vertexOffset += 4;
        attrs.uv0 = DecodeR16G16FLOATToFloat2(PackedUV);
    }
    else
    {
        attrs.uv0 = float2(0.0f, 0.0f);
    }

    if ((psoFlags & PSO_HAS_UV1) != 0)
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

bool IsFrontFacing(float4 h0, float4 h1, float4 h2)
{
    return determinant(float3x3(h0.xyw, h1.xyw, h2.xyw)) >= 0;
}

#endif
