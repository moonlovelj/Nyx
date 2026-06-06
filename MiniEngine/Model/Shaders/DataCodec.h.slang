#ifndef __DATA_CODEC_H_SLANG__
#define __DATA_CODEC_H_SLANG__

float4 DecodeR10G10B10A2UNORMToFloat4(uint packed)
{

    uint r = (packed >> 0) & 0x3FF; // 10 bits
    uint g = (packed >> 10) & 0x3FF;
    uint b = (packed >> 20) & 0x3FF;
    uint a = (packed >> 30) & 0x3;

    float3 rgb = float3(r, g, b) / 1023.0;
    return float4(rgb, a / 3.0);
}

float4 DecodeR16G16B16A16UNORMToFloat4(uint2 packed)
{
    uint r = (packed.x >> 0) & 0xFFFF;
    uint g = (packed.x >> 16) & 0xFFFF;
    uint b = (packed.y >> 0) & 0xFFFF;
    uint a = (packed.y >> 16) & 0xFFFF;

    float4 rgb = float4(r, g, b, a) / 65535.0;
    return rgb;
}

uint4 DecodeR16G16B16A16UINTToUint4(uint2 packed)
{
    uint4 unpacked = uint4(
        (packed.x) & 0xFFFF,
        (packed.x >> 16),
        (packed.y) & 0xFFFF,
        (packed.y >> 16));

    return unpacked;
}

float2 DecodeR16G16FLOATToFloat2(uint packed)
{
    float2 unpacked = float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
    return unpacked;
}

#endif
