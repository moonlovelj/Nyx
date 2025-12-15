#include "Common.hlsli"
#include "CommonResources.hlsli"
#include "DataCodec.hlsli"
#include "ViewMode.hlsli"

struct VSOutput
{
    float3 normal;
    float4 tangent;
    float2 uv0;
    float2 uv1;
    uint meshletIndex;
};

// Flag helpers
static const uint BASECOLOR_UV_OFFSET = 0;
static const uint METALLICROUGHNESS_UV_OFFSET = 1;
static const uint OCCLUSION_UV_OFFSET = 2;
static const uint EMISSIVE_UV_OFFSET = 3;
static const uint NORMAL_UV_OFFSET = 4;

float2 ParseUV(in VSOutput vsOutput, uint offset, uint flags, uint psoFlags)
{
    if (psoFlags & PSO_HAS_UV1)
    {
        return lerp(vsOutput.uv0, vsOutput.uv1, (flags >> offset) & 1);
    }
    
    return vsOutput.uv0;
}

float3 ComputeNormal(VSOutput vsOutput, Texture2D<float3> NormalTexture, SamplerState NormalSampler)
{
    MeshletConstant meshletConstant = GetMeshletConstantSRV(vsOutput.meshletIndex);
    MaterialConstant materilConstant = GetMaterialConstantSRV(meshletConstant.MaterialConstantsIndex);
    float normalTextureScale = materilConstant.normalTextureScale;
    uint flags = materilConstant.flags;

    float3 normal = normalize(vsOutput.normal);
    
    if (!(meshletConstant.psoFlags & PSO_HAS_TANGENT))
    {
        return normal;
    }

    // Construct tangent frame
    float3 tangent = normalize(vsOutput.tangent.xyz);
    float3 bitangent = normalize(cross(normal, tangent)) * vsOutput.tangent.w;
    float3x3 tangentFrame = float3x3(tangent, bitangent, normal);

    // Read normal map and convert to SNORM (TODO:  convert all normal maps to R8G8B8A8_SNORM?)
    normal = NormalTexture.Sample(NormalSampler, ParseUV(vsOutput, NORMAL_UV_OFFSET, flags, meshletConstant.psoFlags)) * 2.0 - 1.0;

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

MaterialProperties GetMaterialProperties(VSOutput vsOutput)
{
    MeshletConstant meshletConstant = GetMeshletConstantSRV(vsOutput.meshletIndex);
    MaterialConstant materilConstant = GetMaterialConstantSRV(meshletConstant.MaterialConstantsIndex);
    float4 baseColorFactor = materilConstant.baseColorFactor;
    float3 emissiveFactor = materilConstant.emissiveFactor;
    float normalTextureScale = materilConstant.normalTextureScale;
    float2 metallicRoughnessFactor = materilConstant.metallicRoughnessFactor;
    uint flags = materilConstant.flags;
    uint TextureStartIndex = materilConstant.TextureStartIndex;
    uint SamplerStartIndex = materilConstant.SamplerStartIndex;

    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[TextureStartIndex];
    Texture2D<float3> MetallicRoughnessTexture = ResourceDescriptorHeap[TextureStartIndex + 1];
    Texture2D<float1> OcclusionTexture = ResourceDescriptorHeap[TextureStartIndex + 2];
    Texture2D<float3> EmissiveTexture = ResourceDescriptorHeap[TextureStartIndex + 3];
    Texture2D<float3> NormalTexture = ResourceDescriptorHeap[TextureStartIndex + 4];

    SamplerState BaseColorSampler = SamplerDescriptorHeap[SamplerStartIndex];
    SamplerState MetallicRoughnessSampler = SamplerDescriptorHeap[SamplerStartIndex + 1];
    SamplerState OcclusionSampler = SamplerDescriptorHeap[SamplerStartIndex + 2];
    SamplerState EmissiveSampler = SamplerDescriptorHeap[SamplerStartIndex + 3];
    SamplerState NormalSampler = SamplerDescriptorHeap[SamplerStartIndex + 4];

    MaterialProperties MatProps;
    MatProps.BaseColor = baseColorFactor * BaseColorTexture.Sample(BaseColorSampler, ParseUV(vsOutput, BASECOLOR_UV_OFFSET, flags, meshletConstant.psoFlags));
    float2 metallicRoughness = metallicRoughnessFactor *
        MetallicRoughnessTexture.Sample(MetallicRoughnessSampler, ParseUV(vsOutput, METALLICROUGHNESS_UV_OFFSET, flags, meshletConstant.psoFlags)).bg;
    metallicRoughness.y = max(0.001, metallicRoughness.y);
    MatProps.Metallic = metallicRoughness.x;
    MatProps.Roughness = metallicRoughness.y;
    MatProps.Occlusion = OcclusionTexture.Sample(OcclusionSampler, ParseUV(vsOutput, OCCLUSION_UV_OFFSET, flags, meshletConstant.psoFlags));
    MatProps.Emissive = emissiveFactor * EmissiveTexture.Sample(EmissiveSampler, ParseUV(vsOutput, EMISSIVE_UV_OFFSET, flags, meshletConstant.psoFlags));
    MatProps.Normal = ComputeNormal(vsOutput, NormalTexture, NormalSampler);
    return MatProps;
}

// 边缘函数：计算二维向量叉乘 (相当于三角形有向面积的2倍)
float EdgeFunction(float2 a, float2 b, float2 c)
{
    return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

struct BarycentricDerivates
{
    float3 lambda; // 透视矫正后的重心坐标
    float3 ddx; // 用于纹理采样的导数 (可选)
    float3 ddy; // 用于纹理采样的导数 (可选)
};

float3 CalculateBarycentrics(float3 worldPos[3], float2 pixelPos, float4x4 viewProj, float2 screenSize)
{
    float4 clipPos[3];
    float2 screenPos[3];

    // 1. 将顶点变换到 Clip Space (由 World 到 Clip)
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        clipPos[i] = mul(viewProj, float4(worldPos[i], 1.0f));
        
        // 2. 变换到 Screen Space (NDC -> Pixel Coordinates)
        // 注意：这里没有做完整的 Clipping，假设 VBuffer 既然存了这个 ID，该像素一定在三角形内
        // 除以 w 得到 NDC
        float2 ndc = clipPos[i].xy / clipPos[i].w;
        
        // 映射到 [0, screenSize]
        // 注意 Y 轴翻转问题，DX 中 NDC Y 向上，屏幕空间 Y 向下 (通常 UV 也是左上角 0,0)
        // 下面公式假设 Y 轴向下为正 (视具体图形 API 而定，DX需注意 y = -y)
        screenPos[i] = (float2(ndc.x, -ndc.y) * 0.5f + 0.5f) * screenSize;
    }

    // 3. 计算屏幕空间有向面积 (三角形总面积)
    // 为了数值稳定性，最好加上微小偏移或确保不为0，但在光栅化覆盖的像素上它不会为0
    float area = EdgeFunction(screenPos[0], screenPos[1], screenPos[2]);
    float invArea = 1.0f / area;

    // 4. 计算当前像素相对于三个边的重心坐标 (Screen Space Barycentrics)
    // 这里的 lambda 是线性的（屏幕空间线性），还没有做透视矫正
    float3 lambdaScreen;
    // 像素中心通常要 +0.5
    float2 centerPos = pixelPos + 0.5f;
    
    lambdaScreen.x = EdgeFunction(screenPos[1], screenPos[2], centerPos) * invArea;
    lambdaScreen.y = EdgeFunction(screenPos[2], screenPos[0], centerPos) * invArea;
    lambdaScreen.z = EdgeFunction(screenPos[0], screenPos[1], centerPos) * invArea;
    // 或者 lambdaScreen.z = 1.0 - lambdaScreen.x - lambdaScreen.y;

    // 5. 透视矫正 (Perspective Correction)
    // 属性插值需要除以 w，因为屏幕空间的线性插值对应 1/w 空间的线性插值
    float3 oneOverW = float3(1.0f / clipPos[0].w, 1.0f / clipPos[1].w, 1.0f / clipPos[2].w);
    
    // 计算当前像素的 1/w
    float pixelOneOverW = lambdaScreen.x * oneOverW.x +
                          lambdaScreen.y * oneOverW.y +
                          lambdaScreen.z * oneOverW.z;
    
    // 最终的重心坐标 (用于插值 UV、Normal、WorldPos 等)
    float3 lambda = (lambdaScreen * oneOverW) / pixelOneOverW;

    return lambda;
}

struct PrimitiveAttributes
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 uv0;
    float2 uv1;
};

PrimitiveAttributes LoadPrimitiveAttributes(
    ByteAddressBuffer geometryData, 
    uint vertexBufferOffset,
    uint psoFlags
    )
{
    PrimitiveAttributes attr;
    attr.position = asfloat(geometryData.Load3(vertexBufferOffset));
    vertexBufferOffset += 12;
    
    uint PackedNormal = geometryData.Load(vertexBufferOffset);
    attr.normal = DecodeR10G10B10A2UNORMToFloat4(PackedNormal).xyz * 2 - 1;
    vertexBufferOffset += 4;
    
    if (psoFlags & PSO_HAS_TANGENT)
    {
        uint PackedTangent = geometryData.Load(vertexBufferOffset);
        attr.tangent = DecodeR10G10B10A2UNORMToFloat4(PackedTangent) * 2 - 1;
        vertexBufferOffset += 4;
    }
    else
    {
        attr.tangent = float4(0, 0, 1, 1);
    }
    
    uint PackedUV = geometryData.Load(vertexBufferOffset);
    vertexBufferOffset += 4;
    attr.uv0 = DecodeR16G16FLOATToFloat2(PackedUV);
    
    if (psoFlags & PSO_HAS_UV1)
    {
        uint PackedUV1 = geometryData.Load(vertexBufferOffset);
        vertexBufferOffset += 4;
        attr.uv1 = DecodeR16G16FLOATToFloat2(PackedUV1);
    }
    else
    {
        attr.uv1 = attr.uv0;
    }

    // TODO: skinning
    // TODO: 采样计算导数
    
    return attr;
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
            IndirectCommand command = GetIndirectCommandsBufferSRV(PSO_IDX_MAIN, commandIndex);
            vsOutput.meshletIndex = command.MeshletIndex;
            MeshletConstant mlet = GetMeshletConstantSRV(command.MeshletIndex);
            InstanceConstant inst = GetInstanceConstantSRV(command.InstanceIndex);
            MaterialConstant materilConstant = GetMaterialConstantSRV(mlet.MaterialConstantsIndex);
            MeshConstant meshInstance = GetMeshConstantSRV(inst.MeshConstantsBase + mlet.MeshConstantsIndexOffset);
            ByteAddressBuffer geometryData = GetGeometryBufferSRV();
            uint packedTri = geometryData.Load(mlet.MeshletPrimitivesOffset + primitiveIndex * 4);
            uint i0 = packedTri & 0xFF;
            uint i1 = (packedTri >> 8) & 0xFF;
            uint i2 = (packedTri >> 16) & 0xFF;
            uint3 localIndices = uint3(i0, i1, i2);
            uint localVertex0 = geometryData.Load(mlet.MeshletVerticesOffset + localIndices.x * 4);
            uint localVertex1 = geometryData.Load(mlet.MeshletVerticesOffset + localIndices.y * 4);
            uint localVertex2 = geometryData.Load(mlet.MeshletVerticesOffset + localIndices.z * 4);
            uint3 verticesOffset = uint3(
                mlet.VertexBufferOffset + localVertex0 * mlet.VertexStride,
                mlet.VertexBufferOffset + localVertex1 * mlet.VertexStride,
                mlet.VertexBufferOffset + localVertex2 * mlet.VertexStride);
            
            PrimitiveAttributes primAttrs[3];
            primAttrs[0] = LoadPrimitiveAttributes(geometryData, verticesOffset.x, mlet.psoFlags);
            primAttrs[1] = LoadPrimitiveAttributes(geometryData, verticesOffset.y, mlet.psoFlags);
            primAttrs[2] = LoadPrimitiveAttributes(geometryData, verticesOffset.z, mlet.psoFlags);
            
            float4x4 WorldMatrix = meshInstance.WorldMatrix;
            float4x3 WorldIT = meshInstance.WorldIT;
            
            float4 localPosition0 = float4(primAttrs[0].position, 1.0);
            float4 localPosition1 = float4(primAttrs[1].position, 1.0);
            float4 localPosition2 = float4(primAttrs[2].position, 1.0);
            float3 worldPos[3];
            worldPos[0] = mul(WorldMatrix, localPosition0).xyz;
            worldPos[1] = mul(WorldMatrix, localPosition1).xyz;
            worldPos[2] = mul(WorldMatrix, localPosition2).xyz;
            float3 barycentrics = CalculateBarycentrics(worldPos, DTid + float2(0.5, 0.5), 
                ViewProjMatrix, float2(ViewportWidth, ViewportHeight));

            float3 normal =
                primAttrs[0].normal * barycentrics.x +
                primAttrs[1].normal * barycentrics.y +
                primAttrs[2].normal * barycentrics.z;
            float4 tangent =
                primAttrs[0].tangent * barycentrics.x +
                primAttrs[1].tangent * barycentrics.y +
                primAttrs[2].tangent * barycentrics.z;
            
            vsOutput.uv0 =
                primAttrs[0].uv0 * barycentrics.x +
                primAttrs[1].uv0 * barycentrics.y +
                primAttrs[2].uv0 * barycentrics.z;
            
            vsOutput.uv1 = 
                primAttrs[0].uv1 * barycentrics.x +
                primAttrs[1].uv1 * barycentrics.y +
                primAttrs[2].uv1 * barycentrics.z;
            
            vsOutput.normal = mul(WorldIT, normal).xyz;
            vsOutput.tangent = float4(mul(WorldIT, tangent.xyz).xyz, tangent.w);
            
            MaterialProperties MatProps = GetMaterialProperties(vsOutput);
            SceneColorUAV[DTid] = float4(MatProps.Emissive, 1.0f);
            GBufferAUAV[DTid] = float4(MatProps.Normal, 1.0);
            GBufferBUAV[DTid] = MatProps.BaseColor;
            GBufferCUAV[DTid] = float4(MatProps.Metallic, MatProps.Roughness, MatProps.Occlusion, 0.f);
            GBufferDUAV[DTid] = float4(0, 0, 0, ViewMode);
            if (ViewMode == VIEW_MODE_SHOW_MESHLET_LOD)
            {
                GBufferDUAV[DTid].rgb = Uint32ToColorR16G16B16(mlet.lodLevel);
            }
            else if (ViewMode == VIEW_MODE_SHOW_MESHLET_ID)
            {
                GBufferDUAV[DTid].rgb = Uint32ToColorR16G16B16(vsOutput.meshletIndex);
            }
            else if (ViewMode == VIEW_MODE_SHOW_TRIANGLE)
            {
                GBufferDUAV[DTid].rgb = Uint32ToColorR16G16B16(primitiveIndex);
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
        
        //mrt.Color = float4(MatProps.Emissive, 1.0f);
        //mrt.GBufferA = float4(MatProps.Normal, 1.0);
        //mrt.GBufferB = MatProps.BaseColor;
        //mrt.GBufferC = float4(MatProps.Metallic, MatProps.Roughness, MatProps.Occlusion, 0.f);
        //mrt.GBufferD.a = ViewMode;
    }
}