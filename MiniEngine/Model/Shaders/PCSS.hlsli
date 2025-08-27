#ifndef __PCSS_H__
#define __PCSS_H__

static const float2 POISSON_SAMPLES[16] = {
    float2(-0.907369, -0.236419),
    float2(-0.827094, 0.460778),
    float2(-0.615339, -0.549559),
    float2(-0.668904, 0.102155),
    float2(-0.485906, 0.867576),
    float2(-0.277483, -0.741231),
    float2(-0.296313, 0.021575),
    float2(-0.011060, -0.217886),
    float2(-0.075976, 0.479496),
    float2(0.228945, -0.831120),
    float2(0.437710, -0.442156),
    float2(0.354185, -0.068162),
    float2(0.463106, 0.291099),
    float2(0.460147, 0.880168),
    float2(0.784787, -0.608451),
    float2(0.873409, 0.106515)
};

#define BLOCKER_SEARCH_NUM_SAMPLES 16
#define PCF_NUM_SAMPLES 16
#define LIGHT_SIZE_UV 0.03
#define PI 3.1415926535f

//#define FORCE_PCF   1

float rand_2to1(float2 uv )
{ 
	// 0 - 1
	const float a = 12.9898, b = 78.233, c = 43758.5453;
	float dt = dot( uv.xy, float2( a,b ) ), sn = fmod( dt, PI );
	return frac(sin(sn) * c);
}

float PenumbraSize(float zReceiver, float zBlocker)
{
	return (zBlocker - zReceiver) / max(1.0 - zBlocker, 1e-6);
}

void FindBlocker(Texture2D<float> shadowMapTex, SamplerState pointSampler, 
	inout float avgBlockerDepth, inout float numBlockers, float2 screenUV,
    float2 uv, float zReceiver, float bias, float4 shadowTexelSize)
{
    float randomAngle = rand_2to1(screenUV) * 2.0 * PI;
    float s, c;
    sincos(randomAngle, s, c);
    float2x2 rotationMatrix = float2x2(c, -s, 
                                       s,  c);
	//This uses similar triangles to compute what area of the shadow map we should search
	float searchWidth = LIGHT_SIZE_UV * shadowTexelSize.y;
	float blockerSum = 0;
	for(int i = 0; i < BLOCKER_SEARCH_NUM_SAMPLES; ++i) 
    {
        float2 offset = POISSON_SAMPLES[i] * searchWidth;
        float2 rotatedOffset = mul(rotationMatrix, offset);
		float2 sampleUV = uv + rotatedOffset;
		float shadowMapDepth = shadowMapTex.SampleLevel(pointSampler, sampleUV, 0);
		if ( shadowMapDepth > zReceiver + bias)
		{
			blockerSum += shadowMapDepth;
			numBlockers++;
		}
    }

    if (numBlockers > 0)
    {
		avgBlockerDepth = blockerSum / numBlockers;
    }
}

float PCF_Filter(Texture2D<float> shadowMapTex, SamplerComparisonState shadowSampler, float2 screenUV,
	float2 uv, float zReceiver, float filterRadiusUV, float bias)
{

    float randomAngle = rand_2to1(screenUV) * 2.0 * PI;
    float s, c;
    sincos(randomAngle, s, c);
    float2x2 rotationMatrix = float2x2(c, -s, 
                                       s,  c);
	float sum = 0.0f;
	for ( int i = 0; i < PCF_NUM_SAMPLES; ++i ) 
    {
		float2 offset = POISSON_SAMPLES[i] * filterRadiusUV;
        float2 rotatedOffset = mul(rotationMatrix, offset);
		sum += shadowMapTex.SampleCmpLevelZero(shadowSampler, uv + rotatedOffset, zReceiver + bias);
    }
	return sum / PCF_NUM_SAMPLES;
}

float PCSS(Texture2D<float> shadowMapTex, 
	SamplerComparisonState shadowSampler, 
	SamplerState pointSampler, 
	float2 screenUV, float3 coords, float4 shadowTexelSize)
{
	// shadowTexelSize.y light size scale
	// shadowTexelSize.z shadow bias scale
    float2 uv = coords.xy;
	float zReceiver = coords.z;

    //float4 QuadValue = shadowMapTex.GatherRed(pointSampler, uv);
	//float DDX = 0.5 * (abs(QuadValue.y - QuadValue.x) + abs(QuadValue.z - QuadValue.w));
	//float DDY = 0.5 * (abs(QuadValue.x - QuadValue.w) + abs(QuadValue.y - QuadValue.z));
	//float Slope = length(float2(DDX, DDY));
    float shadowBias = 1e-3 * shadowTexelSize.z;

#ifdef FORCE_PCF
    return PCF_Filter(shadowMapTex, shadowSampler, screenUV, uv, zReceiver, shadowTexelSize.y * 2 * shadowTexelSize.x, shadowBias);
#else
	// STEP 1: blocker search
	float avgBlockerDepth = 0;
	float numBlockers = 0;
	FindBlocker(shadowMapTex, pointSampler, avgBlockerDepth, numBlockers, screenUV, uv, zReceiver, shadowBias, shadowTexelSize);
	if( numBlockers < 1 ) 
	{
		return 1.0f;
	}

	// STEP 2: penumbra size
	float penumbraRatio = PenumbraSize(zReceiver, avgBlockerDepth);
	float filterRadiusUV = clamp(penumbraRatio * LIGHT_SIZE_UV * shadowTexelSize.y, 0, 0.5);

	// STEP 3: filtering
	return PCF_Filter(shadowMapTex, shadowSampler, screenUV, uv, zReceiver, filterRadiusUV, shadowBias);
#endif
}

#endif