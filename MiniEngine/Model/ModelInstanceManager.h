#pragma once
#include "../Core/CommandContext.h"
#include "Model.h"
#include "Renderer.h"

#include <vector>

namespace ModelInstanceManager
{
	void Initialize(std::shared_ptr<Model> sourceModel, uint32_t instanceCount = 1);

	void Update(GraphicsContext& gfxContext, float deltaTime);

	void Cleanup();

	uint32_t GetNumModelInstances();
	ModelInstance& GetModelInstance(uint32_t index);
	Model* GetSourceModel();
	Math::Vector3 GetInstanceDistributionCenter();
	Math::Vector3 GetInstanceDistributionHalfExtents();
	float GetInstanceDistributionRadius();
};
