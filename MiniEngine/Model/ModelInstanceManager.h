#pragma once
#include "../Core/CommandContext.h"
#include "Model.h"
#include "Renderer.h"

#include <vector>

class ModelInstanceManager
{
public:
	static ModelInstanceManager& Get()
	{
		static ModelInstanceManager g;
		return g;
	}

	void Initialize(std::shared_ptr<Model> sourceModel, uint32_t instanceCount = 1);

	void Render(Renderer::MeshSorter& sorter) const;

	void Update(GraphicsContext& gfxContext, float deltaTime);

	void Cleanup();

	uint32_t GetNumModelInstances() const { return (uint32_t)m_ModelInstances.size(); }
	ModelInstance& GetModelInstance(uint32_t index);
	Model* GetSourceModel() const { return m_SourceModel.get(); }

private:
	std::shared_ptr<Model> m_SourceModel;
	std::vector<ModelInstance> m_ModelInstances;
	UploadBuffer m_InstanceConstantsCPU;
	ByteAddressBuffer m_InstanceConstantsGPU;
};