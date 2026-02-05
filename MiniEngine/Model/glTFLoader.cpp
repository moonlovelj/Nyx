#define CGLTF_IMPLEMENTATION

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "glTFLoader.h"
#include "../Core/Utility.h"

namespace
{
	// Use memory-mapped file I/O to avoid heap allocation
	cgltf_result CgltfReadFile(const struct cgltf_memory_options* memory_options, const struct cgltf_file_options* file_options, const char* path, cgltf_size* size, void** data)
	{
		memory_options;file_options;path;size;data;

		std::wstring wpath = Utility::UTF8ToWideString(path);

		HANDLE file = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) return cgltf_result_file_not_found;

		LARGE_INTEGER fileSize;
		if (!GetFileSizeEx(file, &fileSize))
		{
			CloseHandle(file);
			return cgltf_result_io_error;
		}

		HANDLE mapping = CreateFileMapping(file, NULL, PAGE_READONLY, 0, 0, NULL);
		CloseHandle(file);

		if (mapping == NULL) return cgltf_result_io_error;

		void* ptr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
		CloseHandle(mapping);

		if (ptr == NULL) return cgltf_result_io_error;

		*size = (cgltf_size)fileSize.QuadPart;
		*data = ptr;

		return cgltf_result_success;
	}

	void CgltfReleaseFile(const struct cgltf_memory_options* memory_options, const struct cgltf_file_options* file_options, void* data, cgltf_size size)
	{
		memory_options;file_options;data;size;
		if (data) UnmapViewOfFile(data);
	}
}

namespace glTF
{
	GltfAsset::GltfAsset(const std::wstring& filePath)
	{
		m_BasePath = Utility::GetBasePath(filePath);
		std::string path = Utility::WideStringToUTF8(filePath);

		cgltf_options options = {};
		options.file.read = CgltfReadFile;
		options.file.release = CgltfReleaseFile;

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
