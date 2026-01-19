//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):  James Stanard
//             Chuck Walbourn (ATG)
//
// This code depends on DirectXTex
//

#include "MeshConvert.h"
#include "TextureConvert.h"
#include "glTFLoader.h"
#include "Model.h"
#include "IndexOptimizePostTransform.h"
#include "../Core/VectorMath.h"
#include "DirectXMesh.h"

using namespace DirectX;
using namespace glTF;
using namespace Math;

static DXGI_FORMAT JointIndexFormat(const Accessor& accessor)
{
    switch (accessor.componentType)
    {
    case Accessor::kUnsignedByte:  return DXGI_FORMAT_R8G8B8A8_UINT;
    case Accessor::kUnsignedShort: return DXGI_FORMAT_R16G16B16A16_UINT;
    default:
        ASSERT("Invalid joint index format");
        return DXGI_FORMAT_UNKNOWN;
    }
}

static DXGI_FORMAT AccessorFormat(const Accessor& accessor)
{
    switch (accessor.componentType)
    {
    case Accessor::kUnsignedByte:
        switch (accessor.type)
        {
        case Accessor::kScalar: return DXGI_FORMAT_R8_UNORM;
        case Accessor::kVec2:   return DXGI_FORMAT_R8G8_UNORM;
        default:                return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    case Accessor::kUnsignedShort:
        switch (accessor.type)
        {
        case Accessor::kScalar: return DXGI_FORMAT_R16_UNORM;
        case Accessor::kVec2:   return DXGI_FORMAT_R16G16_UNORM;
        default:                return DXGI_FORMAT_R16G16B16A16_UNORM;
        }
    case Accessor::kFloat:
        switch (accessor.type)
        {
        case Accessor::kScalar: return DXGI_FORMAT_R32_FLOAT;
        case Accessor::kVec2:   return DXGI_FORMAT_R32G32_FLOAT;
        case Accessor::kVec3:   return DXGI_FORMAT_R32G32B32_FLOAT;
        default:                return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
    default:
        ASSERT("Invalid accessor format");
        return DXGI_FORMAT_UNKNOWN;
    }
}

void OptimizeMesh(Renderer::Primitive& outPrim, 
    const cgltf_data* data,
    const cgltf_primitive& inPrim, 
    const Math::Matrix4& localToObject)
{
	// 获取 Position 属性查找
	const cgltf_accessor* posAcc = nullptr;
	for (size_t i = 0; i < inPrim.attributes_count; ++i)
		if (inPrim.attributes[i].type == cgltf_attribute_type_position) posAcc = inPrim.attributes[i].data;

    ASSERT(posAcc != nullptr, "Must have POSITION");

    uint32_t vertexCount = (uint32_t)posAcc->count;
	// --- 处理索引 ---
	uint32_t indexCount = inPrim.indices ? (uint32_t)inPrim.indices->count : vertexCount;
	outPrim.IB = std::make_shared<std::vector<unsigned char>>(4 * indexCount);
	if (inPrim.indices)
	{
		cgltf_accessor_unpack_indices(inPrim.indices, outPrim.IB->data(), 4, indexCount);
	}
	else
	{
		uint32_t* tmp = (uint32_t*)outPrim.IB->data();
		for (uint32_t i = 0; i < indexCount; ++i) tmp[i] = i;
	}

	
	outPrim.primCount = indexCount;
	outPrim.index32 = 1;

	if (inPrim.material)
		outPrim.materialIdx = (uint16_t)cgltf_material_index(data, inPrim.material);
	else
		outPrim.materialIdx = 0x7FFF;

	const uint8_t* indices = outPrim.IB->data();

    if (inPrim.indices)
    {
        switch (inPrim.type)
        {
        default:
        case cgltf_primitive_type::cgltf_primitive_type_points :// POINT LIST
        case cgltf_primitive_type::cgltf_primitive_type_lines : // LINE LIST
        case cgltf_primitive_type::cgltf_primitive_type_line_loop: // LINE LOOP
        case cgltf_primitive_type::cgltf_primitive_type_line_strip: // LINE STRIP
            Utility::Printf("Found unsupported primitive topology\n");
            return;
        case cgltf_primitive_type::cgltf_primitive_type_triangles: // TRIANGLE LIST
            break;
        case cgltf_primitive_type::cgltf_primitive_type_triangle_strip: // TODO: Convert TRIANGLE STRIP
        case cgltf_primitive_type::cgltf_primitive_type_triangle_fan: // TODO: Convert TRIANGLE FAN
            Utility::Printf("Found an index buffer that needs to be converted to a triangle list\n");
            return;
        }
    }

    const bool b32BitIndices = true;

	std::unordered_map<glTF::Primitive::eAttribType, glTF::Accessor> accessors;

	auto ProcessAccessor = [&](glTF::Primitive::eAttribType targetType, cgltf_attribute_type type, int index = 0) {
		for (size_t i = 0; i < inPrim.attributes_count; ++i) {
			if (inPrim.attributes[i].type == type && inPrim.attributes[i].index == index) {
				accessors[targetType] = glTF::GltfAsset::MakeAccessor(inPrim.attributes[i].data);
				break;
			}
		}
		};

	ProcessAccessor(glTF::Primitive::kPosition, cgltf_attribute_type_position);
	ProcessAccessor(glTF::Primitive::kNormal, cgltf_attribute_type_normal);
	ProcessAccessor(glTF::Primitive::kTangent, cgltf_attribute_type_tangent);
	ProcessAccessor(glTF::Primitive::kTexcoord0, cgltf_attribute_type_texcoord, 0);
	ProcessAccessor(glTF::Primitive::kTexcoord1, cgltf_attribute_type_texcoord, 1);
	ProcessAccessor(glTF::Primitive::kJoints0, cgltf_attribute_type_joints, 0);
	ProcessAccessor(glTF::Primitive::kWeights0, cgltf_attribute_type_weights, 0);


    const bool HasNormals = accessors.contains(glTF::Primitive::kNormal);
	const bool HasTangents = accessors.contains(glTF::Primitive::kTangent);
	const bool HasUV0 = accessors.contains(glTF::Primitive::kTexcoord0);
	const bool HasUV1 = accessors.contains(glTF::Primitive::kTexcoord1);
	const bool HasJoints = accessors.contains(glTF::Primitive::kJoints0);
	const bool HasWeights = accessors.contains(glTF::Primitive::kWeights0);
    const bool HasSkin = HasJoints && HasWeights;
    
    std::vector<D3D12_INPUT_ELEMENT_DESC> InputElements;
    InputElements.push_back({"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, glTF::Primitive::kPosition});
    if (HasNormals)
    {
        InputElements.push_back({"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, glTF::Primitive::kNormal });
    }
    if (HasTangents)
    {
        InputElements.push_back({"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,  glTF::Primitive::kTangent });
    }
    if (HasUV0)
    {
        InputElements.push_back({ "TEXCOORD", 0,
            AccessorFormat(accessors[glTF::Primitive::kTexcoord0]),
            glTF::Primitive::kTexcoord0 });
    }
    if (HasUV1)
    {
        InputElements.push_back({ "TEXCOORD", 1,
            AccessorFormat(accessors[glTF::Primitive::kTexcoord1]),
            glTF::Primitive::kTexcoord1 });
    }
    if (HasSkin)
    {
        InputElements.push_back({ "BLENDINDICES", 0,
            JointIndexFormat(accessors[glTF::Primitive::kJoints0]),
            glTF::Primitive::kJoints0 });
        InputElements.push_back({ "BLENDWEIGHT", 0,
            AccessorFormat(accessors[glTF::Primitive::kWeights0]),
            glTF::Primitive::kWeights0 });
    }

    VBReader vbr;
    vbr.Initialize({InputElements.data(), (uint32_t)InputElements.size()});

	for (uint32_t i = 0; i < Primitive::kNumAttribs; ++i)
	{
		auto it = accessors.find((Primitive::eAttribType)i);
		if (it != accessors.end())
		{
			const Accessor& attrib = it->second;
			vbr.AddStream(attrib.dataPtr, vertexCount, i, attrib.stride);
		}
	}

	// 准备材质，处理材质为空的情况
	cgltf_material defaultIdentityMaterial = {};
	defaultIdentityMaterial.alpha_mode = cgltf_alpha_mode_opaque;
	defaultIdentityMaterial.alpha_cutoff = 0.5f;
	const cgltf_material& material = inPrim.material ? *inPrim.material : defaultIdentityMaterial;

	outPrim.psoFlags = PSOFlags::kHasPosition | PSOFlags::kHasNormal;
	if (HasTangents) outPrim.psoFlags |= PSOFlags::kHasTangent;
	if (HasUV0)     outPrim.psoFlags |= PSOFlags::kHasUV0;
	if (HasUV1)    outPrim.psoFlags |= PSOFlags::kHasUV1;
	if (HasSkin) outPrim.psoFlags |= PSOFlags::kHasSkin;
	if (material.alpha_mode == cgltf_alpha_mode_blend) outPrim.psoFlags |= PSOFlags::kAlphaBlend;
	if (material.alpha_mode == cgltf_alpha_mode_mask) outPrim.psoFlags |= PSOFlags::kAlphaTest;
	if (material.double_sided) outPrim.psoFlags |= PSOFlags::kTwoSided;

    std::unique_ptr<XMFLOAT3[]> position;
    std::unique_ptr<XMFLOAT3[]> normal;
    std::unique_ptr<XMFLOAT4[]> tangent;
    std::unique_ptr<XMFLOAT2[]> texcoord0;
    std::unique_ptr<XMFLOAT2[]> texcoord1;
    std::unique_ptr<XMFLOAT4[]> joints;
    std::unique_ptr<XMFLOAT4[]> weights;
    position.reset(new XMFLOAT3[vertexCount]);
    normal.reset(new XMFLOAT3[vertexCount]);

    ASSERT_SUCCEEDED(vbr.Read(position.get(), "POSITION", 0, vertexCount));
    {
        // Local space bounds
        Vector3 sphereCenterLS = vertexCount > 0 ? Vector3(position[0]) : Vector3(kZero);
        Scalar maxRadiusLSSq(kZero);

        // Object space bounds
        // (This would be expressed better with an AffineTransform * Vector3)
        Vector3 sphereCenterOS = Vector3(localToObject * Vector4(sphereCenterLS));
        Scalar maxRadiusOSSq(kZero);

        outPrim.m_BBoxLS = AxisAlignedBox(kZero);
        outPrim.m_BBoxOS = AxisAlignedBox(kZero);

        for (uint32_t v = 0; v < vertexCount/*maxIndex*/; ++v)
        {
            Vector3 positionLS = Vector3(position[v]);
            maxRadiusLSSq = Max(maxRadiusLSSq, LengthSquare(sphereCenterLS - positionLS));

            outPrim.m_BBoxLS.AddPoint(positionLS);

            Vector3 positionOS = Vector3(localToObject * Vector4(positionLS));
            maxRadiusOSSq = Max(maxRadiusOSSq, LengthSquare(sphereCenterOS - positionOS));

            outPrim.m_BBoxOS.AddPoint(positionOS);
        }

        outPrim.m_BoundsLS = Math::BoundingSphere(sphereCenterLS, Sqrt(maxRadiusLSSq));
        outPrim.m_BoundsOS = Math::BoundingSphere(sphereCenterOS, Sqrt(maxRadiusOSSq));
        ASSERT(outPrim.m_BoundsOS.GetRadius() > 0.0f);
    }

    if (HasNormals)
    {
        ASSERT_SUCCEEDED(vbr.Read(normal.get(), "NORMAL", 0, vertexCount));
    }
    else
    {
        const size_t faceCount = indexCount / 3;

        if (b32BitIndices)
            ComputeNormals((const uint32_t*)indices, faceCount, position.get(), vertexCount, CNORM_DEFAULT, normal.get());
        else
            ComputeNormals((const uint16_t*)indices, faceCount, position.get(), vertexCount, CNORM_DEFAULT, normal.get());
    }

    if (HasUV0)
    {
        texcoord0.reset(new XMFLOAT2[vertexCount]);
        ASSERT_SUCCEEDED(vbr.Read(texcoord0.get(), "TEXCOORD", 0, vertexCount));
    }

    if (HasUV1)
    {
        texcoord1.reset(new XMFLOAT2[vertexCount]);
        ASSERT_SUCCEEDED(vbr.Read(texcoord1.get(), "TEXCOORD", 1, vertexCount));
    }

    if (HasTangents)
    {
        tangent.reset(new XMFLOAT4[vertexCount]);
        ASSERT_SUCCEEDED(vbr.Read(tangent.get(), "TANGENT", 0, vertexCount));
    }
    else
    {
        //ASSERT(maxIndex < vertexCount);
        ASSERT(indexCount % 3 == 0);

        HRESULT hr = S_OK;

		bool useUV1ForTangent = false;
		if (material.normal_texture.texture && material.normal_texture.texcoord == 1)
		{
			if (HasUV1)
			{
				useUV1ForTangent = true;
			}
			else if (HasUV0)
			{
				Utility::Printf("Warning: Normal map requests UV1 but mesh missing it. Fallback to UV0 for tangents.\n");
				useUV1ForTangent = false;
			}
		}

        if (HasUV0 && !useUV1ForTangent)
        {
            tangent.reset(new XMFLOAT4[vertexCount]);
            if (b32BitIndices)
            {
                hr = ComputeTangentFrame((uint32_t*)indices, indexCount / 3, position.get(), normal.get(), texcoord0.get(),
                    vertexCount, tangent.get());
            }
            else
            {
                hr = ComputeTangentFrame((uint16_t*)indices, indexCount / 3, position.get(), normal.get(), texcoord0.get(),
                    vertexCount, tangent.get());
            }
        }
        else if (HasUV1 && useUV1ForTangent)
        {
            tangent.reset(new XMFLOAT4[vertexCount]);
            if (b32BitIndices)
            {
                hr = ComputeTangentFrame((uint32_t*)indices, indexCount / 3, position.get(), normal.get(), texcoord1.get(),
                    vertexCount, tangent.get());
            }
            else
            {
                hr = ComputeTangentFrame((uint16_t*)indices, indexCount / 3, position.get(), normal.get(), texcoord1.get(),
                    vertexCount, tangent.get());
            }
        }

        ASSERT_SUCCEEDED(hr, "Error generating a tangent frame");
    }

    if (HasSkin)
    {
        joints.reset(new XMFLOAT4[vertexCount]);
        weights.reset(new XMFLOAT4[vertexCount]);
        ASSERT_SUCCEEDED(vbr.Read(joints.get(), "BLENDINDICES", 0, vertexCount));
        ASSERT_SUCCEEDED(vbr.Read(weights.get(), "BLENDWEIGHT", 0, vertexCount));
    }

    // Use VBWriter to generate a new, interleaved and compressed vertex buffer
    std::vector<D3D12_INPUT_ELEMENT_DESC> OutputElements;

    outPrim.psoFlags = PSOFlags::kHasPosition | PSOFlags::kHasNormal;
    OutputElements.push_back({"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT});
    OutputElements.push_back({"NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT});
    if (tangent.get())
    {
        OutputElements.push_back({"TANGENT", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT});
        outPrim.psoFlags |= PSOFlags::kHasTangent;
    }
    if (texcoord0.get())
    {
        OutputElements.push_back({"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT});
        outPrim.psoFlags |= PSOFlags::kHasUV0;
    }
    if (texcoord1.get())
    {
        OutputElements.push_back({"TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT});
        outPrim.psoFlags |= PSOFlags::kHasUV1;
    }
    if (HasSkin)
    {
        OutputElements.push_back({ "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT });
        OutputElements.push_back({ "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT });
        outPrim.psoFlags |= PSOFlags::kHasSkin;
    }

    D3D12_INPUT_LAYOUT_DESC layout = {OutputElements.data(), (uint32_t)OutputElements.size()};

    VBWriter vbw;
    vbw.Initialize(layout);

    uint32_t offsets[10];
    uint32_t strides[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    ComputeInputLayout(layout, offsets, strides);
    uint32_t stride = strides[0];

    outPrim.VB = std::make_shared<std::vector<unsigned char>>(stride * vertexCount);
    ASSERT_SUCCEEDED(vbw.AddStream(outPrim.VB->data(), vertexCount, 0, stride));

    vbw.Write( position.get(), "POSITION", 0, vertexCount );
    vbw.Write( normal.get(), "NORMAL", 0, vertexCount, true );
    if (tangent.get())
        vbw.Write( tangent.get(), "TANGENT", 0, vertexCount, true );
    if (texcoord0.get())
        vbw.Write( texcoord0.get(), "TEXCOORD", 0, vertexCount );
    if (texcoord1.get())
        vbw.Write( texcoord1.get(), "TEXCOORD", 1, vertexCount );
    if (HasSkin)
    {
        vbw.Write(joints.get(), "BLENDINDICES", 0, vertexCount);
        vbw.Write(weights.get(), "BLENDWEIGHT", 0, vertexCount);
    }

    // Now write a VB for positions only (or positions and UV when alpha testing)
    uint32_t depthStride = 12;
    std::vector<D3D12_INPUT_ELEMENT_DESC> DepthElements;
    DepthElements.push_back({"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT});
    if (material.alpha_mode == cgltf_alpha_mode_mask)
    {
        depthStride += 4;
        DepthElements.push_back({"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT});
    }
    if (HasSkin)
    {
        depthStride += 16;
        DepthElements.push_back({ "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT });
        DepthElements.push_back({ "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT });
    }

    VBWriter dvbw;
    dvbw.Initialize({DepthElements.data(), (uint32_t)DepthElements.size()});

    outPrim.DepthVB = std::make_shared<std::vector<unsigned char>>(depthStride * vertexCount);
    ASSERT_SUCCEEDED(dvbw.AddStream(outPrim.DepthVB->data(), vertexCount, 0, depthStride));

    dvbw.Write( position.get(), "POSITION", 0, vertexCount );
    if (material.alpha_mode == cgltf_alpha_mode_mask)
    {
		// 获取 Base Color 的 UV 索引，默认为 0
		int texCoordIndex = material.pbr_metallic_roughness.base_color_texture.texture ?
			material.pbr_metallic_roughness.base_color_texture.texcoord : 0;

		const XMFLOAT2* texcoordData = (texCoordIndex == 1) ? texcoord1.get() : texcoord0.get();
        if (!texcoordData)
        {
            //TODO 暂时特殊处理，按理说alphatest的材质一定会有UV的
			std::vector<XMFLOAT2> tempUVs(vertexCount, XMFLOAT2(0.0f, 0.0f));
            dvbw.Write(tempUVs.data(), "TEXCOORD", 0, vertexCount);
        }
        else
        {
            dvbw.Write(texcoordData, "TEXCOORD", 0, vertexCount);
        }
    }
    if (HasSkin)
    {
        dvbw.Write(joints.get(), "BLENDINDICES", 0, vertexCount);
        dvbw.Write(weights.get(), "BLENDWEIGHT", 0, vertexCount);
    }

    ASSERT(outPrim.materialIdx < 0x8000, "Only 15-bit material indices allowed");

    outPrim.vertexStride = (uint16_t)stride;
    outPrim.index32 = b32BitIndices ? 1 : 0;
    //outPrim.materialIdx = material.index;

    outPrim.primCount = indexCount;

    // TODO:  Generate optimized depth-only streams
}

