#ifndef __CULLING_COMMON_HLSLI__
#define __CULLING_COMMON_HLSLI__

#include "SharedHeader.hlsli"

#define CULLING_RESULT_NO_CULLED		    0x0
#define CULLING_RESULT_OF_FRUSTUM			0x1
#define CULLING_RESULT_OF_OCCLUSION_PASS1	0x2
#define CULLING_RESULT_OF_OCCLUSION_PASS2	0x3

#define CULLING_FRUSTUM_VISIBLE			    0x1
#define CULLING_OCCLUSION_PASS1_VISIBLE	    0x2
#define CULLING_OCCLUSION_PASS2_VISIBLE	    0x4


cbuffer CullConstants : register(b3)
{
    uint startCommand;
    uint maxCommands;
    float screenErrorConstant; // 计算meshlet屏幕误差时使用的提前计算的常量 (cotHalfFov * screenHeight) / 2.0
    uint psoIdx;
    uint indirectBufferOffset;
    uint cullingStage;
};

#endif