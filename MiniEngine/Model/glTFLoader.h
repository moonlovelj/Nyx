#pragma once

#include "../ThirdParty/cgltf/cgltf.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace glTF
{
	struct Accessor
	{
		enum { kByte, kUnsignedByte, kShort, kUnsignedShort, kSignedInt, kUnsignedInt, kFloat };
		enum { kScalar, kVec2, kVec3, kVec4, kMat2, kMat3, kMat4 };

		const uint8_t* dataPtr;
		uint32_t stride;
		uint32_t count;
		uint16_t componentType;
		uint16_t type;
	};

	struct Primitive
	{
		enum eAttribType { kPosition, kNormal, kTangent, kTexcoord0, kTexcoord1, kColor0, kJoints0, kWeights0, kNumAttribs };
	};

	struct Material
	{
		enum eMaterialTexture { kBaseColor, kMetallicRoughness, kOcclusion, kEmissive, kNormal, kSpecular, kSpecularColor, kNumTextures };
	};

	static_assert((uint32_t)Material::eMaterialTexture::kNumTextures <= 8, "Num of material textures can not much than 8");

	struct AnimChannel {
		enum ePath { kTranslation, kRotation, kScale, kWeights };
	};

	struct AnimSampler {
		enum eInterpolation { kLinear, kStep, kCatmullRomSpline, kCubicSpline };
	};

	class GltfAsset
	{
	public:
		GltfAsset(const std::wstring& filePath);
		~GltfAsset();

		bool IsValid() const { return m_Data != nullptr; }

		cgltf_data* m_Data = nullptr;
		std::wstring m_BasePath;

		// Helper: convert cgltf enums to legacy-compatible enums
		static uint16_t MapComponentType(cgltf_component_type type);
		static uint16_t MapType(cgltf_type type);
		static Accessor MakeAccessor(const cgltf_accessor* src);
	};
}
