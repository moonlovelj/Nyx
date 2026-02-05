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
// Author:   James Stanard
//

#pragma once

#include "Animation.h"
#include "../Core/GpuBuffer.h"
#include "../Core/VectorMath.h"
#include "../Core/Camera.h"
#include "../Core/CommandContext.h"
#include "../Core/UploadBuffer.h"
#include "../Core/TextureManager.h"
#include "../Core/Math/BoundingBox.h"
#include "../Core/Math/BoundingSphere.h"
#include "MeshletStructs.h"
#include <fstream>
#include "ConstantBuffers.h"
#include "InstanceResourceManager.h"
#include <cstdint>
#include <map>
#include <vector>
#include <string>


namespace Renderer
{
    class MeshSorter;

	inline constexpr uint32_t kPageSizeInBytes = 256u * 1024u;             // 256 KB
	inline constexpr uint32_t kChunkSizeInBytes = kPageSizeInBytes * 1024u; // 256 MB

	enum GeometryCompressionType : uint32_t
	{
		kGeometryCompressionNone = 0,
		kGeometryCompressionLZ4 = 1,
	};
}

//
// To request a PSO index, provide flags that describe the kind of PSO
// you need.  If one has not yet been created, it will be created.
//
namespace PSOFlags
{
    enum : uint16_t
    { 
        kHasPosition    = 0x001,  // Required
        kHasNormal      = 0x002,  // Required
        kHasTangent     = 0x004,
        kHasUV0         = 0x008,  // Required (for now)
        kHasUV1         = 0x010,
        kAlphaBlend     = 0x020,
        kAlphaTest      = 0x040,
        kTwoSided       = 0x080,
        kHasSkin        = 0x100,  // Implies having indices and weights
    };
}

struct Mesh
{
	//uint16_t numJoints;     // Number of skeleton joints when skinning
	//uint16_t startJoint;    // Flat offset to first joint index
    uint32_t numDraws;
    uint16_t matrixIdx;
	uint16_t padding;
	struct Draw
	{
        // 存到Group里
		//uint16_t meshCBV;       // Index of mesh constant buffer
		//uint16_t materialCBV;   // Index of material constant buffer
		//uint16_t psoFlags;      // Flags needed to request a PSO
		float boundingBoxMin[3];  // Local space AABB min
		float boundingBoxMax[3];  // Local space AABB max
		uint32_t rootNodeIndex; // 指向 m_Nodes 数组的根节点
		//uint32_t groupMetadataStartIndex; // 指向 m_GroupMetadatas 数组
		//uint32_t groupCount; // 有多少个 group
	};
	Draw draw[1]; 
};

struct GraphNode // 96 bytes
{
    Math::Matrix4 xform;
    Math::Quaternion rotation;
    Math::XMFLOAT3 scale;

    uint32_t matrixIdx : 28;
    uint32_t hasSibling : 1;
    uint32_t hasChildren : 1;
    uint32_t staleMatrix : 1;
    uint32_t skeletonRoot : 1;
};

// instance 粒度
struct Joint
{
    Math::Matrix4 posXform;
    Math::Matrix3 nrmXform;
};

// Camera info
struct CameraData
{
	union
	{
		struct
		{
			float aspectRatio;
			float yfov;
		};
		struct
		{
			float xmag;
			float ymag;
		};
	};
	float znear;
	float zfar;
	uint32_t matrixIdx : 28;
	enum eType { kPerspective, kOrthographic } type : 4;
};

class Model
{
public:

    ~Model() { Destroy(); }

    void Render(Renderer::MeshSorter& sorter) const;

    uint32_t GetNumTotalDraws() const;

    Math::BoundingSphere m_BoundingSphere; // Object-space bounding sphere
    Math::AxisAlignedBox m_BoundingBox;

    ByteAddressBuffer m_MaterialConstants;

    uint32_t m_NumNodes;
    uint32_t m_NumMeshes;
    uint32_t m_NumAnimations;
	uint32_t m_NumJoints;
	uint32_t m_NumCameras;

    std::unique_ptr<GraphNode[]> m_SceneGraph;
    std::vector<TextureRef> textures;
    std::unique_ptr<uint8_t[]> m_KeyFrameData;
    std::unique_ptr<AnimationCurve[]> m_CurveData;
    std::unique_ptr<AnimationSet[]> m_Animations;
    std::unique_ptr<uint16_t[]> m_JointIndices;
	std::unique_ptr<Math::Matrix4[]> m_JointIBMs;
	std::unique_ptr<CameraData[]> m_Cameras;

	// 逻辑 Mesh 对象
	std::vector<Mesh*> m_Meshes;

	std::vector<Renderer::GroupMetadata> m_GroupMetadatas;

	std::vector<Renderer::PageMetadata> m_PageMetadatas;
	std::vector<Renderer::PageCompressionInfo> m_PageCompressionInfos;

	// BVH 树 (需要上传到 GPU，这里暂存 CPU 端)
	std::vector<Renderer::HierarchyNode> m_Nodes;

	// 用于记录流式加载
	std::wstring m_StreamingFilePath;
	uint64_t m_GeometryBlobOffsetInFile = 0;
	uint64_t m_GeometryUncompressedSize = 0;
	uint32_t m_GeometryCompressionType = 0;

protected:
    void Destroy();
};

class ModelInstance
{
public:
    ModelInstance() {}
    ~ModelInstance() {

    }
    ModelInstance( std::shared_ptr<const Model> sourceModel );
    ModelInstance( const ModelInstance& modelInstance );

    ModelInstance& operator=( std::shared_ptr<const Model> sourceModel );

    bool IsNull(void) const { return m_Model == nullptr; }

    void Update(float deltaTime, MeshConstants* meshConstantsCPU, Joint* jointCPU);
    void Render(Renderer::MeshSorter& sorter) const;
	void SetupInstanceData(InstanceConstants* instanceContants) const;

    void Resize(float newRadius);
    Math::Vector3 GetCenter() const;
    Math::Scalar GetRadius() const;
    Math::BoundingSphere GetBoundingSphere() const;
    Math::OrientedBox GetBoundingBox() const;

    size_t GetNumAnimations(void) const { return m_AnimState.size(); }
    void PlayAnimation(uint32_t animIdx, bool loop);
    void PauseAnimation(uint32_t animIdx);
    void ResetAnimation(uint32_t animIdx);
    void StopAnimation(uint32_t animIdx);
    void UpdateAnimations(float deltaTime);
    void LoopAllAnimations(void);

    void SetPosition(Math::Vector3 position);

    size_t GetNumCameras() const { return m_Cameras.size(); }
    std::vector<std::shared_ptr<Math::Camera>> GetCameras() const;

	const InstanceAllocation& GetInstanceAllocation() const { return m_Alloc; }

private:
    void GatherDrawItems() const;

    InstanceAllocation m_Alloc{};

    std::shared_ptr<const Model> m_Model;
    std::unique_ptr<Math::AffineTransform[]> m_BoundingSphereTransforms;
    Math::UniformTransform m_Locator;

    std::unique_ptr<GraphNode[]> m_AnimGraph;   // A copy of the scene graph when instancing animation
    std::vector<AnimationState> m_AnimState;    // Per-animation (not per-curve)

    std::map<uint32_t, std::shared_ptr<Math::Camera>> m_Cameras;
};
