//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
//

#pragma once

#include <cstddef>

namespace Renderer
{

	// ==================== Nanite LOD 配置参数 ====================
	struct NaniteLODConfig
	{
		// ========== Meshlet 参数 ==========
		size_t meshletMaxVertices = 64;
		size_t meshletMaxTriangles = 128;
		float meshletBackfaceCullingConeWeight = 0.0f;

		// ========== 分组参数 ==========
		size_t groupSize = 16;  // 每组合并的 meshlet 数量

		// ========== 简化参数 ==========
		float simplificationDecimateFactor = 2.0f;  // 每层简化到多少倍（2.0 = 减半）
		float simplificationTargetError = 0.05f;    // meshoptimizer 的 target_error
		float simplificationTargetErrorMultiplier = 1.1f;  // 每层误差增长倍数

		// ========== 简化率要求 ==========
		// 单组简化率要求（> 此值则认为简化失败）
		float simplificationFactorRequirement = 1.1f;  // 1.1 = 禁用检查，0.97 = 至少简化 3%

		// 层间简化率要求（< 此值则停止整个流程）
		float simplificationFactorRequirementBetweenLevels = 0.97f;  // 至少简化 3%

		// ========== 动态简化率参数 ==========
		struct DynamicSimplificationRates
		{
			size_t largeThreshold = 10000;   // 大网格阈值（三角形数）
			float largeRate = 0.5f;          // 大网格目标简化率

			size_t mediumThreshold = 1000;   // 中等网格阈值
			float mediumRate = 0.5f;         // 中等网格目标简化率

			size_t smallThreshold = 100;     // 小网格阈值
			float smallRate = 0.5f;          // 小网格目标简化率

			float tinyRate = 0.6f;           // 极小网格目标简化率
		};
		DynamicSimplificationRates dynamicRates;

		// ========== 激进简化参数 ==========
		float aggressiveSimplificationRate = 0.9f;      // 激进模式简化率阈值
		float aggressiveErrorMultiplier = 5.0f;         // 激进模式误差放宽倍数
		float aggressiveTargetDivisor = 4.0f;           // 激进模式目标三角形数除数

		// ========== LOD 层级参数 ==========
		size_t maxLods = 20;                 // 最大 LOD 层数
		size_t minRootTriangles = 128;        // 根节点最少三角形数

		// ========== 屏幕空间误差参数 ==========
		float maxScreenErrorPixels = 0.5f;   // 最大屏幕误差（像素）
		float assumedDistance = 100.0f;      // 假设的观察距离
		float assumedScreenHeight = 1080.0f; // 假设的屏幕高度
		float assumedFOV = 1.0472f;          // 假设的 FOV（60 度 = 1.0472 弧度）

		// ========== 调试参数 ==========
		bool enableDetailedLogging = true;   // 启用详细日志
		bool enablePerGroupLogging = true;   // 启用每组的详细日志
	};

	// 预设配置
	namespace NaniteLODPresets
	{
		// 默认配置（平衡质量和性能）
		inline NaniteLODConfig Default()
		{
			return NaniteLODConfig{};
		}

		// 高质量配置（更多层级，更保守的简化）
		inline NaniteLODConfig HighQuality()
		{
			NaniteLODConfig cfg;
			cfg.maxLods = 24;
			cfg.simplificationTargetError = 0.02f;
			cfg.simplificationFactorRequirement = 0.95f;
			cfg.simplificationFactorRequirementBetweenLevels = 0.95f;
			cfg.dynamicRates.largeRate = 0.4f;
			cfg.dynamicRates.mediumRate = 0.5f;
			cfg.dynamicRates.smallRate = 0.6f;
			cfg.maxScreenErrorPixels = 0.25f;
			return cfg;
		}

		// 性能优先配置（更少层级，更激进的简化）
		inline NaniteLODConfig Performance()
		{
			NaniteLODConfig cfg;
			cfg.maxLods = 12;
			cfg.simplificationTargetError = 0.1f;
			cfg.simplificationFactorRequirement = 1.1f;  // 禁用单组检查
			cfg.simplificationFactorRequirementBetweenLevels = 0.9f;
			cfg.dynamicRates.largeRate = 0.2f;
			cfg.dynamicRates.mediumRate = 0.3f;
			cfg.dynamicRates.smallRate = 0.4f;
			cfg.maxScreenErrorPixels = 1.0f;
			cfg.aggressiveErrorMultiplier = 10.0f;
			return cfg;
		}

		// 极致质量配置
		inline NaniteLODConfig UltraQuality()
		{
			NaniteLODConfig cfg = HighQuality();
			cfg.maxLods = 30;
			cfg.simplificationTargetError = 0.01f;
			cfg.maxScreenErrorPixels = 0.1f;
			cfg.minRootTriangles = 32;
			return cfg;
		}
	}

} // namespace Renderer