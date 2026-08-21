#ifndef __VIEW_MODE_H_SLANG__
#define __VIEW_MODE_H_SLANG__

#define VIEW_MODE_LIT                0
#define VIEW_MODE_SHOW_MESHLET_LOD   1
#define VIEW_MODE_SHOW_MESHLET_ID    2
#define VIEW_MODE_SHOW_TRIANGLE      3
#define VIEW_MODE_SHOW_MESH_ID       4
#define VIEW_MODE_SHOW_INSTANCE_ID   5
#define VIEW_MODE_SHOW_MATERIAL_ID   6
#define VIEW_MODE_SHOW_VSM_PAGE_STATE 7
#define VIEW_MODE_SHOW_VSM_CLIPMAP_LEVEL 8

uint HashUint(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

float3 Uint32ToColorR16G16B16(uint id)
{
    uint hash_id = HashUint(id);

    // Allocate 10 bits for red (high bits)
    uint r_bits = (hash_id >> 22) & 0x3FF;

    // Allocate 11 bits for green (middle bits)
    uint g_bits = (hash_id >> 11) & 0x7FF;

    // Allocate 11 bits for blue (low bits)
    uint b_bits = hash_id & 0x7FF;

    // Normalize extracted integers to [0.0, 1.0]
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

float3 HsvToRgb(float3 hsv)
{
    float3 rgb = saturate(abs(frac(hsv.x + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
    return hsv.z * lerp(1.0f, rgb, hsv.y);
}

float3 MakeDebugColor(uint id, uint seed)
{
    uint hashed = HashUint(id + seed);
    float hue = (hashed & 0xFFFFu) / 65535.0f;
    float sat = 0.65f + 0.35f * ((hashed >> 16) & 0xFFu) / 255.0f;
    float val = 0.45f + 0.45f * ((hashed >> 24) & 0xFFu) / 255.0f;
    return HsvToRgb(float3(hue, sat, val));
}

#endif
