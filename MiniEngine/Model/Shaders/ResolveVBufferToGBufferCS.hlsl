#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "DataCodec.hlsli"
#include "ViewMode.hlsli"
#include "../MeshletStructs.h"
#include "../StructsIO.h"
#include "GeometryCommon.hlsli"

struct VSOutput
{
    float3 normal;
    float4 tangent;
    float2 uv0;
    float2 uv1;
    float2 uv0_dx;
    float2 uv0_dy;
    float2 uv1_dx;
    float2 uv1_dy;
    uint meshletIndex;
};

// Flag helpers
static const uint BASECOLOR_UV_OFFSET = 0;
static const uint METALLICROUGHNESS_UV_OFFSET = 1;
static const uint OCCLUSION_UV_OFFSET = 2;
static const uint EMISSIVE_UV_OFFSET = 3;
static const uint NORMAL_UV_OFFSET = 4;

struct Derivs
{
    float2 uv; // 插值的UV
    float2 uv_dx;
    float2 uv_dy;
};

float2 ParseUV(in VSOutput vsOutput, uint offset, uint flags, uint psoFlags)
{
    if (psoFlags & PSO_HAS_UV1)
    {
        return lerp(vsOutput.uv0, vsOutput.uv1, (flags >> offset) & 1);
    }
    
    return vsOutput.uv0;
}

float4 SampleTextureWithDerivs(
    in VSOutput vsOutput,
    Texture2D<float4> texture,
    SamplerState texSampler,
    uint offset, 
    uint flags, 
    uint psoFlags)
{
    float2 uv = vsOutput.uv0;
    float2 uv_dx = vsOutput.uv0_dx;
    float2 uv_dy = vsOutput.uv0_dy;
    if (psoFlags & PSO_HAS_UV1)
    {
        uv = lerp(vsOutput.uv0, vsOutput.uv1, (flags >> offset) & 1);
        uv_dx = lerp(vsOutput.uv0_dx, vsOutput.uv1_dx, (flags >> offset) & 1);
        uv_dy = lerp(vsOutput.uv0_dy, vsOutput.uv1_dy, (flags >> offset) & 1);
    }
    return texture.SampleGrad(texSampler, uv, uv_dx, uv_dy);
}

float3 ComputeNormal(VSOutput vsOutput, Texture2D<float4> NormalTexture, 
    SamplerState NormalSampler, MeshletHeader meshletHeader, bool isFrontFacing)
{
    MaterialConstant materilConstant = GetMaterialConstantSRV(meshletHeader.GetMaterialBufferIndex());
    uint psoFlags = meshletHeader.GetPSOFlags();
    float normalTextureScale = materilConstant.normalTextureScale;
    uint flags = materilConstant.flags;

    float3 normal = normalize(vsOutput.normal);
    if (!isFrontFacing && (psoFlags & PSO_TWO_SIDED))
    {
        normal = -normal;
    }
    
    if (!(psoFlags & PSO_HAS_TANGENT))
    {
        return normal;
    }

    // Construct tangent frame
    float3 tangent = normalize(vsOutput.tangent.xyz);
    float3 bitangent = normalize(cross(normal, tangent)) * vsOutput.tangent.w;
    float3x3 tangentFrame = float3x3(tangent, bitangent, normal);

    // Read normal map and convert to SNORM (TODO:  convert all normal maps to R8G8B8A8_SNORM?)
    normal = SampleTextureWithDerivs(vsOutput, NormalTexture, NormalSampler, NORMAL_UV_OFFSET, flags, psoFlags).rgb * 2.0 - 1.0;

    // glTF spec says to normalize N before and after scaling, but that's excessive
    normal = normalize(normal * float3(normalTextureScale, normalTextureScale, 1));

    // Multiply by transpose (reverse order)
    return mul(normal, tangentFrame);
}

struct MaterialProperties
{
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float Occlusion;
    float3 Emissive;
    float3 Normal;
};

MaterialProperties GetMaterialProperties(VSOutput vsOutput, MeshletHeader meshletHeader, bool isFrontFacing)
{
    MaterialConstant materilConstant = GetMaterialConstantSRV(meshletHeader.GetMaterialBufferIndex());
    uint psoFlags = meshletHeader.GetPSOFlags();
    float4 baseColorFactor = materilConstant.baseColorFactor;
    float3 emissiveFactor = materilConstant.emissiveFactor;
    float normalTextureScale = materilConstant.normalTextureScale;
    float2 metallicRoughnessFactor = materilConstant.metallicRoughnessFactor;
    uint flags = materilConstant.flags;
    uint TextureStartIndex = materilConstant.TextureStartIndex;
    uint SamplerStartIndex = materilConstant.SamplerStartIndex;

    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[TextureStartIndex];
    Texture2D<float4> MetallicRoughnessTexture = ResourceDescriptorHeap[TextureStartIndex + 1];
    Texture2D<float4> OcclusionTexture = ResourceDescriptorHeap[TextureStartIndex + 2];
    Texture2D<float4> EmissiveTexture = ResourceDescriptorHeap[TextureStartIndex + 3];
    Texture2D<float4> NormalTexture = ResourceDescriptorHeap[TextureStartIndex + 4];

    SamplerState BaseColorSampler = SamplerDescriptorHeap[SamplerStartIndex];
    SamplerState MetallicRoughnessSampler = SamplerDescriptorHeap[SamplerStartIndex + 1];
    SamplerState OcclusionSampler = SamplerDescriptorHeap[SamplerStartIndex + 2];
    SamplerState EmissiveSampler = SamplerDescriptorHeap[SamplerStartIndex + 3];
    SamplerState NormalSampler = SamplerDescriptorHeap[SamplerStartIndex + 4];

    MaterialProperties MatProps;
    MatProps.BaseColor = baseColorFactor * SampleTextureWithDerivs(vsOutput, BaseColorTexture, BaseColorSampler, BASECOLOR_UV_OFFSET, flags, psoFlags);
    float2 metallicRoughness = metallicRoughnessFactor *
        SampleTextureWithDerivs(vsOutput, MetallicRoughnessTexture, MetallicRoughnessSampler, METALLICROUGHNESS_UV_OFFSET, flags, psoFlags).bg;
    metallicRoughness.y = max(0.001, metallicRoughness.y);
    MatProps.Metallic = metallicRoughness.x;
    MatProps.Roughness = metallicRoughness.y;
    MatProps.Occlusion = SampleTextureWithDerivs(vsOutput, OcclusionTexture, OcclusionSampler, OCCLUSION_UV_OFFSET, flags, psoFlags).r;
    MatProps.Emissive = emissiveFactor * SampleTextureWithDerivs(vsOutput, EmissiveTexture, EmissiveSampler, EMISSIVE_UV_OFFSET, flags, psoFlags).rgb;
    MatProps.Normal = ComputeNormal(vsOutput, NormalTexture, NormalSampler, meshletHeader, isFrontFacing);
    return MatProps;
}

// 计算二维向量叉乘 (相当于三角形有向面积的2倍)
float EdgeFunction(float2 a, float2 b, float2 c)
{
    return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

struct BarycentricDerivs
{
    float3 lambda; // 重心坐标
    float3 lambda_correction; // 透视矫正重心坐标
    float3 ddx_lambda; // 重心坐标对 Screen X 的偏导
    float3 ddy_lambda; // 重心坐标对 Screen Y 的偏导
    float3 oneOverW; // 预计算好的 1/w0, 1/w1, 1/w2
};

BarycentricDerivs CalculateBarycentricsAndDerivs(
    float3 p[3], // 三角形三个顶点的世界坐标
    float2 pixelPos, // 当前像素坐标
    float4x4 viewProj,
    float2 screenSize)
{
    BarycentricDerivs output;
    
    float4 clipPos[3];
    float2 screenPos[3];
    
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        clipPos[i] = mul(viewProj, float4(p[i], 1.0f));
        // NDC -> Screen
        float2 ndc = clipPos[i].xy / clipPos[i].w;
        // 注意 Y 轴方向，DX 是左上角(0,0)，NDC Y向上，所以 ndc.y 要反转
        screenPos[i] = (float2(ndc.x, -ndc.y) * 0.5f + 0.5f) * screenSize;
        output.oneOverW[i] = 1.0f / clipPos[i].w; 
    }

    // 计算面积 (的2倍)
    float area = EdgeFunction(screenPos[0], screenPos[1], screenPos[2]);
    float invArea = rcp(sign(area + 1e-30f) * max(abs(area), 1e-7f));

    // 计算重心坐标
    float2 centerPos = pixelPos + 0.5f;
    output.lambda.x = EdgeFunction(screenPos[1], screenPos[2], centerPos) * invArea;
    output.lambda.y = EdgeFunction(screenPos[2], screenPos[0], centerPos) * invArea;
    output.lambda.z = EdgeFunction(screenPos[0], screenPos[1], centerPos) * invArea;

    // 计算重心坐标的屏幕空间导数 (解析解)
    // d(lambda)/dx = (V_next.y - V_prev.y) / Area
    // d(lambda)/dy = (V_prev.x - V_next.x) / Area
    // 系数是 1.0/Area 还是 1.0/(2*Area) 取决于 EdgeFunction 的实现，EdgeFunction算出的是2倍面积，所以直接除以它即可
    
    output.ddx_lambda.x = (screenPos[1].y - screenPos[2].y) * invArea;
    output.ddx_lambda.y = (screenPos[2].y - screenPos[0].y) * invArea;
    output.ddx_lambda.z = (screenPos[0].y - screenPos[1].y) * invArea;

    output.ddy_lambda.x = (screenPos[2].x - screenPos[1].x) * invArea;
    output.ddy_lambda.y = (screenPos[0].x - screenPos[2].x) * invArea;
    output.ddy_lambda.z = (screenPos[1].x - screenPos[0].x) * invArea;
    
    float3 oneOverW = float3(1.0f / clipPos[0].w, 1.0f / clipPos[1].w, 1.0f / clipPos[2].w);
    
    // 计算当前像素的 1/w
    float pixelOneOverW = output.lambda.x * oneOverW.x +
                          output.lambda.y * oneOverW.y +
                          output.lambda.z * oneOverW.z;
    
    // 最终的重心坐标 (用于插值 UV、Normal、WorldPos 等)
    output.lambda_correction = (output.lambda * oneOverW) / pixelOneOverW;

    return output;
}

struct Barycentrics
{
    float3 Value;
    float3 ValueDDX;
    float3 ValueDDY;
};

/** Calculates perspective correct barycentric coordinates and partial derivatives using screen derivatives. */
Barycentrics CalculateTriangleBarycentrics(
    float4 clipPos[3],
    float2 pixelPos, // 当前像素坐标
    float4x4 viewProj,
    float2 ViewInvSize)
{
    Barycentrics barycentrics;
    const float2 PixelClip = pixelPos * ViewInvSize * float2(2, -2) + float2(-1, 1);

    const float3 RcpW = rcp(float3(clipPos[0].w, clipPos[1].w, clipPos[2].w));
    const float3 Pos0 = clipPos[0].xyz * RcpW.x;
    const float3 Pos1 = clipPos[1].xyz * RcpW.y;
    const float3 Pos2 = clipPos[2].xyz * RcpW.z;

    const float3 Pos120X = float3(Pos1.x, Pos2.x, Pos0.x);
    const float3 Pos120Y = float3(Pos1.y, Pos2.y, Pos0.y);
    const float3 Pos201X = float3(Pos2.x, Pos0.x, Pos1.x);
    const float3 Pos201Y = float3(Pos2.y, Pos0.y, Pos1.y);

    const float3 C_dx = Pos201Y - Pos120Y;
    const float3 C_dy = Pos120X - Pos201X;

    const float3 C = C_dx * (PixelClip.x - Pos120X) + C_dy * (PixelClip.y - Pos120Y); // Evaluate the 3 edge functions
    const float3 G = C * RcpW;

    const float H = dot(C, RcpW);
    const float RcpH = rcp(H);

	// UVW = C * RcpW / dot(C, RcpW)
    barycentrics.Value = G * RcpH;

	// Texture coordinate derivatives:
	// UVW = G / H where G = C * RcpW and H = dot(C, RcpW)
	// UVW' = (G' * H - G * H') / H^2
	// float2 TexCoordDX = UVW_dx.y * TexCoord10 + UVW_dx.z * TexCoord20;
	// float2 TexCoordDY = UVW_dy.y * TexCoord10 + UVW_dy.z * TexCoord20;
    const float3 G_dx = C_dx * RcpW;
    const float3 G_dy = C_dy * RcpW;

    const float H_dx = dot(C_dx, RcpW);
    const float H_dy = dot(C_dy, RcpW);

    barycentrics.ValueDDX = (G_dx * H - G * H_dx) * (RcpH * RcpH) * (2.0f * ViewInvSize.x);
    barycentrics.ValueDDY = (G_dy * H - G * H_dy) * (RcpH * RcpH) * (-2.0f * ViewInvSize.y);

    return barycentrics;
}

float4 InterpolateOnly(float4 val[3], Barycentrics barycentris)
{
    return val[0] * barycentris.Value.x +
           val[1] * barycentris.Value.y +
           val[2] * barycentris.Value.z;
}

// 属性插值并计算导数 (透视矫正链式法则)
// val: 顶点的属性数组 (如 uv0, uv1, uv2)
// w: 顶点的 w 数组 (clipPos.w)
// bary: 重心数据
Derivs InterpolateWithDerivs(float2 val[3], Barycentrics barycentris)
{
    Derivs d;
    d.uv = val[0] * barycentris.Value.x +
            val[1] * barycentris.Value.y +
            val[2] * barycentris.Value.z;
    
    d.uv_dx = val[0] * barycentris.ValueDDX.x +
               val[1] * barycentris.ValueDDX.y +
               val[2] * barycentris.ValueDDX.z;

    d.uv_dy = val[0] * barycentris.ValueDDY.x + 
               val[1] * barycentris.ValueDDY.y +
               val[2] * barycentris.ValueDDY.z;
    
    return d;
}

[RootSignature(Renderer_RootSig)]
[numthreads(8, 8, 1)]
void main( uint2 DTid : SV_DispatchThreadID )
{
    if (DTid.x < ViewportWidth && DTid.y < ViewportHeight)
    {
        Texture2D<uint2> VBuffer = GetVBufferSRV();
        RWTexture2D<float4> SceneColorUAV = GetSceneColorUAV();
        RWTexture2D<float4> GBufferAUAV = GetGBufferAUAV();
        RWTexture2D<float4> GBufferBUAV = GetGBufferBUAV();
        RWTexture2D<float4> GBufferCUAV = GetGBufferCUAV();
        RWTexture2D<float4> GBufferDUAV = GetGBufferDUAV();
        uint2 VBufferRaw = VBuffer[DTid];
        
        if (VBufferRaw.g > 0)
        {
            VSOutput vsOutput;
            float depth = asfloat(VBufferRaw.g);
            uint primitiveIndex = VBufferRaw.r & 0x7F;
            uint commandIndex = (VBufferRaw.r >> 7);
            
            VisibleMeshletPayload payload = GetVisibleMeshletPayload(commandIndex);
            InstanceConstant inst = GetInstanceConstantSRV(payload.InstanceIndex);
            MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshBufferIdx);
    
            StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
            GroupDataLocation groupDataLocation = groupDataLocationSRV[payload.GetGroupIndex()];
            uint groupByteOffset = groupDataLocation.ByteOffset;
            ByteAddressBuffer geometryChunksBuffer = GetGeometryChunksBufferSRV(groupDataLocation.ChunkIndex);
            MeshletHeader meshletHeader = geometryChunksBuffer.Load < MeshletHeader > (
            groupByteOffset + sizeof(GroupHeader) + payload.GetMeshletIndex() * sizeof(MeshletHeader));
            MaterialConstant materilConstant = GetMaterialConstantSRV(meshletHeader.GetMaterialBufferIndex());
            uint psoFlags = meshletHeader.GetPSOFlags();
            vsOutput.meshletIndex = payload.GetMeshletIndex();
            
            uint vertexByteOffset = groupByteOffset + meshletHeader.VertexOffset;
            uint indexByteOffset = groupByteOffset + meshletHeader.TriangleOffset;
            uint vertexStride = meshletHeader.GetVertexStride();
            uint3 triIndices = LoadAndUnpackTriangle(geometryChunksBuffer, indexByteOffset, primitiveIndex);
            VertexAttributes vertexAttrs[3];
            vertexAttrs[0] = LoadVertexAttributes(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.x, psoFlags);
            vertexAttrs[1] = LoadVertexAttributes(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.y, psoFlags);
            vertexAttrs[2] = LoadVertexAttributes(geometryChunksBuffer, vertexByteOffset, vertexStride, triIndices.z, psoFlags);
            
            float4x4 WorldMatrix = meshInstance.WorldMatrix;
            float4x3 WorldIT = meshInstance.WorldIT;
            
            float4 localPosition0 = float4(vertexAttrs[0].position, 1.0);
            float4 localPosition1 = float4(vertexAttrs[1].position, 1.0);
            float4 localPosition2 = float4(vertexAttrs[2].position, 1.0);
            float3 worldPos[3];
            worldPos[0] = mul(WorldMatrix, localPosition0).xyz;
            worldPos[1] = mul(WorldMatrix, localPosition1).xyz;
            worldPos[2] = mul(WorldMatrix, localPosition2).xyz;

            float4 clipPos[3];
            clipPos[0] = mul(ViewProjMatrix, float4(worldPos[0], 1.0f));
            clipPos[1] = mul(ViewProjMatrix, float4(worldPos[1], 1.0f));
            clipPos[2] = mul(ViewProjMatrix, float4(worldPos[2], 1.0f));
            
            Barycentrics barycentris =
                CalculateTriangleBarycentrics(clipPos, DTid + float2(0.5, 0.5),
                ViewProjMatrix, float2(InvViewportWidth, InvViewportHeight));

            float3 worldPosition = worldPos[0] * barycentris.Value.x +
                                  worldPos[1] * barycentris.Value.y +
                                  worldPos[2] * barycentris.Value.z;
            
            float4 normals[3] = {
                float4(vertexAttrs[0].normal, 0),
                float4(vertexAttrs[1].normal, 0),
                float4(vertexAttrs[2].normal, 0)
            };
            float3 normal = InterpolateOnly(normals, barycentris).xyz;

            float4 tangents[3] = {
                vertexAttrs[0].tangent,
                vertexAttrs[1].tangent,
                vertexAttrs[2].tangent
            };
            
            float4 tangent = InterpolateOnly(tangents, barycentris);
            float2 uvs0[3] = {
                vertexAttrs[0].uv0,
                vertexAttrs[1].uv0,
                vertexAttrs[2].uv0
            };
            
            Derivs derivs0 = InterpolateWithDerivs(uvs0, barycentris);

            float2 uvs1[3] = {
                vertexAttrs[0].uv1,
                vertexAttrs[1].uv1,
                vertexAttrs[2].uv1
            };
            Derivs derivs1 = InterpolateWithDerivs(uvs1, barycentris);
            
            vsOutput.normal = mul(WorldIT, normal).xyz;
            vsOutput.tangent = float4(mul(WorldIT, tangent.xyz).xyz, tangent.w);
            vsOutput.uv0 = derivs0.uv;
            vsOutput.uv0_dx = derivs0.uv_dx;
            vsOutput.uv0_dy = derivs0.uv_dy;
            vsOutput.uv1 = derivs1.uv;
            vsOutput.uv1_dx = derivs1.uv_dx;
            vsOutput.uv1_dy = derivs1.uv_dy;
            
            float3x3 worldRotationScale = (float3x3) meshInstance.WorldMatrix;
            float detWorld = determinant(worldRotationScale);
            bool isWorldFlipped = detWorld < 0.0;
            bool logicalFrontFacing = IsFrontFacing(clipPos[0], clipPos[1], clipPos[2]) ^ isWorldFlipped;
            MaterialProperties MatProps = GetMaterialProperties(vsOutput, meshletHeader, logicalFrontFacing);
            SceneColorUAV[DTid] = float4(MatProps.Emissive, 1.0f);
            GBufferAUAV[DTid] = float4(MatProps.Normal, 1.0);
            GBufferBUAV[DTid] = MatProps.BaseColor;
            GBufferCUAV[DTid] = float4(MatProps.Metallic, MatProps.Roughness, MatProps.Occlusion, 0.f);
            GBufferDUAV[DTid] = float4(0, 0, 0, ViewMode);
            if (ViewMode == VIEW_MODE_SHOW_MESHLET_LOD)
            {
                if (meshletHeader.GetLODLevel() == 0)
                    GBufferDUAV[DTid].rgb = float3(0.8, 0.8, 0.8);
                else
                    GBufferDUAV[DTid].rgb = MakeDebugColor(meshletHeader.GetLODLevel(), 0x1f3d5b79u);
            }
            else if (ViewMode == VIEW_MODE_SHOW_MESHLET_ID)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(vsOutput.meshletIndex, 0x6a09e667u);
            }
            else if (ViewMode == VIEW_MODE_SHOW_TRIANGLE)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(primitiveIndex, 0xbb67ae85u);
            }
            else if (ViewMode == VIEW_MODE_SHOW_MESH_ID)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(inst.MeshBufferIdx, 0x3c6ef372u);
            }
            else if (ViewMode == VIEW_MODE_SHOW_INSTANCE_ID)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(payload.InstanceIndex, 0xa54ff53au);
            }
            else if (ViewMode == VIEW_MODE_SHOW_MATERIAL_ID)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(meshletHeader.GetMaterialBufferIndex(), 0x510e527fu);
            }
            else
            {
                GBufferDUAV[DTid].rgb = 0;
            }
        }
        else
        {
            //SceneColorUAV[DTid] = float4(0, 0, 0, 1);
            GBufferAUAV[DTid] = float4(0, 0, 0, 0);
            GBufferBUAV[DTid] = float4(0, 0, 0, 0);
            GBufferCUAV[DTid] = float4(0, 0, 0, 0);
            GBufferDUAV[DTid] = float4(0, 0, 0, 0);
        }
    }
}