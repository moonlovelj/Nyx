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

float3 ComputeNormal(VSOutput vsOutput, Texture2D<float4> NormalTexture, SamplerState NormalSampler)
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
    normal = SampleTextureWithDerivs(vsOutput, NormalTexture, NormalSampler, NORMAL_UV_OFFSET, flags, meshletConstant.psoFlags).rgb * 2.0 - 1.0;

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
    MatProps.BaseColor = baseColorFactor * SampleTextureWithDerivs(vsOutput, BaseColorTexture, BaseColorSampler, BASECOLOR_UV_OFFSET, flags, meshletConstant.psoFlags);
    float2 metallicRoughness = metallicRoughnessFactor *
        SampleTextureWithDerivs(vsOutput, MetallicRoughnessTexture, MetallicRoughnessSampler, METALLICROUGHNESS_UV_OFFSET, flags, meshletConstant.psoFlags).bg;
    metallicRoughness.y = max(0.001, metallicRoughness.y);
    MatProps.Metallic = metallicRoughness.x;
    MatProps.Roughness = metallicRoughness.y;
    MatProps.Occlusion = SampleTextureWithDerivs(vsOutput, OcclusionTexture, OcclusionSampler, OCCLUSION_UV_OFFSET, flags, meshletConstant.psoFlags).r;
    MatProps.Emissive = emissiveFactor * SampleTextureWithDerivs(vsOutput, EmissiveTexture, EmissiveSampler, EMISSIVE_UV_OFFSET, flags, meshletConstant.psoFlags).rgb;
    MatProps.Normal = ComputeNormal(vsOutput, NormalTexture, NormalSampler);
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
    float  w[3]; // 三个点的 clip w 值
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
        output.w[i] = clipPos[i].w;
    }

    // 计算面积 (的2倍)
    float area = EdgeFunction(screenPos[0], screenPos[1], screenPos[2]);
    float invArea = 1.0f / area;

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

float4 InterpolateOnly(float4 val[3], BarycentricDerivs bary)
{
    return val[0] * bary.lambda_correction.x + 
           val[1] * bary.lambda_correction.y + 
           val[2] * bary.lambda_correction.z;   
}

// 属性插值并计算导数 (透视矫正链式法则)
// val: 顶点的属性数组 (如 uv0, uv1, uv2)
// w: 顶点的 w 数组 (clipPos.w)
// bary: 重心数据
Derivs InterpolateWithDerivs(float2 val[3], BarycentricDerivs bary)
{
    Derivs d;
    
    // 属性需预除 w
    float3 oneOverW = float3(1.0f / bary.w[0], 1.0f / bary.w[1], 1.0f / bary.w[2]);
    float2 attr[3];
    attr[0] = val[0] * oneOverW.x;
    attr[1] = val[1] * oneOverW.y;
    attr[2] = val[2] * oneOverW.z;

    // 计算当前像素的 1/w 和 属性值 (Pre-divide)
    float pixelOneOverW = dot(bary.lambda, oneOverW);
    float2 pixelAttrPreDiv = attr[0] * bary.lambda.x + attr[1] * bary.lambda.y + attr[2] * bary.lambda.z;
    
    // 最终属性值 (透视矫正后)
    d.uv = pixelAttrPreDiv / pixelOneOverW;

    // 对除法求导 (u/v)' = (u'v - uv') / v^2
    // 需要求 d(pixelAttrPreDiv / pixelOneOverW) / dx
    
    // 计算 1/w 的导数
    float ddx_oneOverW = dot(bary.ddx_lambda, oneOverW);
    float ddy_oneOverW = dot(bary.ddy_lambda, oneOverW);

    // 计算 PreDiv属性 的导数
    float2 ddx_attrPre = attr[0] * bary.ddx_lambda.x + attr[1] * bary.ddx_lambda.y + attr[2] * bary.ddx_lambda.z;
    float2 ddy_attrPre = attr[0] * bary.ddy_lambda.x + attr[1] * bary.ddy_lambda.y + attr[2] * bary.ddy_lambda.z;

    // 应用除法法则
    float w_sqr = pixelOneOverW * pixelOneOverW;
    
    // ddx = (attr' * w - attr * w') / w^2
    d.uv_dx = (ddx_attrPre * pixelOneOverW - pixelAttrPreDiv * ddx_oneOverW) / w_sqr;
    d.uv_dy = (ddy_attrPre * pixelOneOverW - pixelAttrPreDiv * ddy_oneOverW) / w_sqr;

    return d;
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

    // TODO: skinning normal and tangent
    
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

            BarycentricDerivs barycentricDerivs = CalculateBarycentricsAndDerivs(worldPos, DTid + float2(0.5, 0.5),
                ViewProjMatrix, float2(ViewportWidth, ViewportHeight));

            float4 normals[3] = {
                float4(primAttrs[0].normal, 0),
                float4(primAttrs[1].normal, 0),
                float4(primAttrs[2].normal, 0)
            };
            float3 normal = InterpolateOnly(normals, barycentricDerivs).xyz;

            float4 tangents[3] = {
                primAttrs[0].tangent,
                primAttrs[1].tangent,
                primAttrs[2].tangent
            };
            
            float4 tangent = InterpolateOnly(tangents, barycentricDerivs);
            float2 uvs0[3] = {
                primAttrs[0].uv0,
                primAttrs[1].uv0,
                primAttrs[2].uv0
            };
            
            Derivs derivs0 = InterpolateWithDerivs(uvs0, barycentricDerivs);

            float2 uvs1[3] = {
                primAttrs[0].uv1,
                primAttrs[1].uv1,
                primAttrs[2].uv1
            };
            Derivs derivs1 = InterpolateWithDerivs(uvs1, barycentricDerivs);
            
            vsOutput.normal = mul(WorldIT, normal).xyz;
            vsOutput.tangent = float4(mul(WorldIT, tangent.xyz).xyz, tangent.w);
            vsOutput.uv0 = derivs0.uv;
            vsOutput.uv0_dx = derivs0.uv_dx;
            vsOutput.uv0_dy = derivs0.uv_dy;
            vsOutput.uv1 = derivs1.uv;
            vsOutput.uv1_dx = derivs1.uv_dx;
            vsOutput.uv1_dy = derivs1.uv_dy;
            
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
    }
}