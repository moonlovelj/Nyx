#ifndef __VIEW_MODE_HLSLI__
#define __VIEW_MODE_HLSLI__

// HSV -> RGB（sRGB）
// h ∈ [0,1), s ∈ [0,1], v ∈ [0,1]
float3 HSVtoRGB(float3 hsv)
{
    float h = hsv.x, s = hsv.y, v = hsv.z;
    float c = v * s;
    float hp = (h * 6.0);
    float x = c * (1.0 - abs(fmod(hp, 2.0) - 1.0));
    float3 rgb1 =
        (hp < 1.0) ? float3(c, x, 0) :
        (hp < 2.0) ? float3(x, c, 0) :
        (hp < 3.0) ? float3(0, c, x) :
        (hp < 4.0) ? float3(0, x, c) :
        (hp < 5.0) ? float3(x, 0, c) :
        float3(c, 0, x);
    float m = v - c;
    return rgb1 + m;
}

// 通过“黄金比例色相 + S/V 循环”映射 LOD 到颜色。
float3 ColorFromLOD(uint lod)
{
    // 1) 黄金比例步进的色相分布，避免相邻LOD扎堆
    const float phi1 = 0.61803398875; // golden ratio - 1
    float h = frac(lod * phi1);       // 均匀遍历色相环 [0,1)

    // 2) 根据 LOD 取模在不同饱和度/明度组循环
    // 4 组 × 任意色相 ≈ 可稳定区分 48+ 级
    uint g = lod & 3u; // lod % 4
    float s, v;
    if (g == 0u) { s = 0.70; v = 0.95; }
    else if (g == 1u) { s = 0.85; v = 0.85; }
    else if (g == 2u) { s = 0.75; v = 0.70; }
    else /* g == 3 */ { s = 0.90; v = 0.78; }

    // 3) 轻微扰动色相，避免偶发邻近同色
    h = frac(h + (float)((lod * 37u) % 100u) * 0.001f);

    return HSVtoRGB(float3(h, s, v));
}

#endif