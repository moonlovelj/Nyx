#ifndef __MATH_HLSLI__
#define __MATH_HLSLI__

#define FLT_MIN         1.175494351e-38F        // min positive value
#define FLT_MAX         3.402823466e+38F        // max value
#define PI				3.1415926535f
#define INV_PI          0.31830988618f
#define TWO_PI			6.283185307f

float Pow5(float x)
{
    float xSq = x * x;
    return xSq * xSq * x;
}

float Pow4(float x)
{
    return x * x * x * x;
}

float Pow2(float x)
{
    return x * x;
}

#endif