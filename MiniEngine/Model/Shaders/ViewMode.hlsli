#ifndef __VIEW_MODE_HLSLI__
#define __VIEW_MODE_HLSLI__

uint HashUint(uint x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

float3 Uint32ToColorR16G16B16(uint id)
{
    uint hash_id = HashUint(id);

    // 为红色通道分配10位 (高位)
    uint r_bits = (hash_id >> 22) & 0x3FF;

    // 为绿色通道分配11位 (中位)
    uint g_bits = (hash_id >> 11) & 0x7FF;

    // 为蓝色通道分配11位 (低位)
    uint b_bits = hash_id & 0x7FF;

    // 将提取出的整数值归一化到 [0.0, 1.0] 范围
    float r = float(r_bits) / 1023.0f;
    float g = float(g_bits) / 2047.0f;
    float b = float(b_bits) / 2047.0f;

    return float3(r, g, b);
}

float3 Uint2ToColorR16G16B16(uint2 id)
{
    uint h = HashUint(id.x) + (HashUint(id.y) * 33);
    return Uint32ToColorR16G16B16(h);
}

#endif