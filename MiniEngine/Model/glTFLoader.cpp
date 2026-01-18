#define CGLTF_IMPLEMENTATION
#include "glTFLoader.h"
#include "../Core/Utility.h"

namespace glTF
{
	GltfAsset::GltfAsset(const std::wstring& filePath)
	{
		m_BasePath = Utility::GetBasePath(filePath);
		std::string path = Utility::WideStringToUTF8(filePath);

		cgltf_options options = {};
		cgltf_result result = cgltf_parse_file(&options, path.c_str(), &m_Data);
		if (result != cgltf_result_success) return;

		result = cgltf_load_buffers(&options, m_Data, path.c_str());
		if (result != cgltf_result_success)
		{
			cgltf_free(m_Data);
			m_Data = nullptr;
		}
	}

	GltfAsset::~GltfAsset()
	{
		if (m_Data) cgltf_free(m_Data);
	}

	uint16_t GltfAsset::MapComponentType(cgltf_component_type type)
	{
		switch (type)
		{
		case cgltf_component_type_r_8:   return Accessor::kByte;
		case cgltf_component_type_r_8u:  return Accessor::kUnsignedByte;
		case cgltf_component_type_r_16:  return Accessor::kShort;
		case cgltf_component_type_r_16u: return Accessor::kUnsignedShort;
		case cgltf_component_type_r_32u: return Accessor::kUnsignedInt;
		case cgltf_component_type_r_32f: return Accessor::kFloat;
		default: return Accessor::kFloat;
		}
	}

	uint16_t GltfAsset::MapType(cgltf_type type)
	{
		switch (type)
		{
		case cgltf_type_scalar: return Accessor::kScalar;
		case cgltf_type_vec2:   return Accessor::kVec2;
		case cgltf_type_vec3:   return Accessor::kVec3;
		case cgltf_type_vec4:   return Accessor::kVec4;
		case cgltf_type_mat2:   return Accessor::kMat2;
		case cgltf_type_mat3:   return Accessor::kMat3;
		case cgltf_type_mat4:   return Accessor::kMat4;
		default: return Accessor::kScalar;
		}
	}

	Accessor GltfAsset::MakeAccessor(const cgltf_accessor* src)
	{
		Accessor dst = {};
		if (!src) return dst;
		dst.dataPtr = (const uint8_t*)src->buffer_view->buffer->data + src->offset + src->buffer_view->offset;
		dst.stride = (uint32_t)src->stride;
		dst.count = (uint32_t)src->count;
		dst.componentType = MapComponentType(src->component_type);
		dst.type = MapType(src->type);
		return dst;
	}
}