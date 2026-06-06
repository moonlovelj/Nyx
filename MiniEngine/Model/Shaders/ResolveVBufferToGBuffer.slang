#include "CommonResources.slang"
#include "GeometryCommon.slang"
#include "ViewMode.h.slang"

struct ResolveVBufferToGBufferConstants
{
    float4x4 ViewProjMatrix;
    float2 InvViewportSize;
    uint ViewportWidth;
    uint ViewportHeight;
    uint ViewMode;
};

ConstantBuffer<ResolveVBufferToGBufferConstants> g_ResolveVBufferToGBuffer;

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
    float2 uv; // Interpolated UV
    float2 uv_dx;
    float2 uv_dy;
};

float2 ParseUV(in VSOutput vsOutput, uint offset, uint flags, uint psoFlags)
{
    if ((psoFlags & PSO_HAS_UV1) != 0)
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
    if ((psoFlags & PSO_HAS_UV1) != 0)
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
    MaterialConstant materilConstant = LoadMaterialConstant(meshletHeader.GetMaterialBufferIndex());
    uint psoFlags = meshletHeader.GetPSOFlags();
    float normalTextureScale = materilConstant.normalTextureScale;
    uint flags = materilConstant.flags;

    float3 normal = normalize(vsOutput.normal);
    if (!isFrontFacing && ((psoFlags & PSO_TWO_SIDED) != 0))
    {
        normal = -normal;
    }

    if ((psoFlags & PSO_HAS_TANGENT) == 0)
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
    MaterialConstant materilConstant = LoadMaterialConstant(meshletHeader.GetMaterialBufferIndex());
    uint psoFlags = meshletHeader.GetPSOFlags();
    float4 baseColorFactor = materilConstant.baseColorFactor;
    float3 emissiveFactor = materilConstant.emissiveFactor;
    float2 metallicRoughnessFactor = materilConstant.metallicRoughnessFactor;
    uint flags = materilConstant.flags;
    uint TextureStartIndex = materilConstant.TextureStartIndex;
    uint SamplerStartIndex = materilConstant.SamplerStartIndex;

    Texture2D<float4> BaseColorTexture = GetResourceDescriptorHandle<Texture2D<float4>>(TextureStartIndex);
    Texture2D<float4> MetallicRoughnessTexture = GetResourceDescriptorHandle<Texture2D<float4>>(TextureStartIndex + 1);
    Texture2D<float4> OcclusionTexture = GetResourceDescriptorHandle<Texture2D<float4>>(TextureStartIndex + 2);
    Texture2D<float4> EmissiveTexture = GetResourceDescriptorHandle<Texture2D<float4>>(TextureStartIndex + 3);
    Texture2D<float4> NormalTexture = GetResourceDescriptorHandle<Texture2D<float4>>(TextureStartIndex + 4);

    SamplerState BaseColorSampler = GetSamplerDescriptor(SamplerStartIndex);
    SamplerState MetallicRoughnessSampler = GetSamplerDescriptor(SamplerStartIndex + 1);
    SamplerState OcclusionSampler = GetSamplerDescriptor(SamplerStartIndex + 2);
    SamplerState EmissiveSampler = GetSamplerDescriptor(SamplerStartIndex + 3);
    SamplerState NormalSampler = GetSamplerDescriptor(SamplerStartIndex + 4);

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

struct Barycentrics
{
    float3 Value;
    float3 ValueDDX;
    float3 ValueDDY;
};

float2 PixelToClipSpace(float2 pixelPos, float2 invViewportSize)
{
    return pixelPos * invViewportSize * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
}

float EdgeFunction(float2 a, float2 b, float2 p)
{
    float2 ap = p - a;
    float2 ab = b - a;
    return ap.x * ab.y - ap.y * ab.x;
}

void BuildTriangleEdgeDerivatives(float2 p0, float2 p1, float2 p2, out float3 dEdgeDx, out float3 dEdgeDy)
{
    // e0 = edge(p1, p2, p), e1 = edge(p2, p0, p), e2 = edge(p0, p1, p)
    dEdgeDx = float3(
        p2.y - p1.y,
        p0.y - p2.y,
        p1.y - p0.y);

    dEdgeDy = float3(
        p1.x - p2.x,
        p2.x - p0.x,
        p0.x - p1.x);
}

/*
    Perspective-correct barycentrics from clip-space edge equations
    ---------------------------------------------------------------
    Let clip vertex i be Vi = (xi, yi, wi), and NDC vertex pi = (xi/wi, yi/wi).
    For sample position p (in clip/NDC xy), define edge values:

        e0 = edge(p1, p2, p)
        e1 = edge(p2, p0, p)
        e2 = edge(p0, p1, p)

    Perspective-correct weights:

        ni = ei / wi
        H  = n0 + n1 + n2
        lambda_i = ni / H = (ei/wi) / sum_k(ek/wk)

    Derivatives in clip space (u in {x_clip, y_clip}):

        d(lambda_i)/du = (d(ni)/du * H - ni * dH/du) / H^2
        dH/du          = sum_k d(nk)/du
        d(ni)/du       = d(ei)/du / wi

    Edge derivatives are constants per triangle:

        de0/dx = p2.y - p1.y,   de0/dy = p1.x - p2.x
        de1/dx = p0.y - p2.y,   de1/dy = p2.x - p0.x
        de2/dx = p1.y - p0.y,   de2/dy = p0.x - p1.x

    Clip->pixel derivative mapping:

        x_clip =  2 * x_px * InvViewportWidth  - 1
        y_clip = -2 * y_px * InvViewportHeight + 1

        d/dx_px = (2 * InvViewportWidth)  * d/dx_clip
        d/dy_px = (-2 * InvViewportHeight) * d/dy_clip
*/
Barycentrics CalculateTriangleBarycentrics(
    float4 clipPos[3],
    float2 pixelPos,
    float2 ViewInvSize)
{
    Barycentrics bary;
    const float2 pixelClip = PixelToClipSpace(pixelPos, ViewInvSize);
    const float3 invW = rcp(float3(clipPos[0].w, clipPos[1].w, clipPos[2].w));

    // NDC positions used to evaluate edge equations in clip space.
    const float2 ndc0 = clipPos[0].xy * invW.x;
    const float2 ndc1 = clipPos[1].xy * invW.y;
    const float2 ndc2 = clipPos[2].xy * invW.z;

    const float3 edge = float3(
        EdgeFunction(ndc1, ndc2, pixelClip),
        EdgeFunction(ndc2, ndc0, pixelClip),
        EdgeFunction(ndc0, ndc1, pixelClip));

    float3 dEdgeDx;
    float3 dEdgeDy;
    BuildTriangleEdgeDerivatives(ndc0, ndc1, ndc2, dEdgeDx, dEdgeDy);

    // Perspective-correct barycentrics: lambda = (edge / w) / sum(edge / w).
    const float3 numer = edge * invW;
    const float denom = dot(numer, 1.0f.xxx);

    // Degenerate or near-degenerate triangles can produce unstable derivatives.
    if (abs(denom) < 1e-20f)
    {
        bary.Value = float3(1.0f, 0.0f, 0.0f);
        bary.ValueDDX = 0.0f;
        bary.ValueDDY = 0.0f;
        return bary;
    }

    const float invDenom = rcp(denom);
    const float invDenom2 = invDenom * invDenom;

    bary.Value = numer * invDenom;

    const float3 dNumerDx = dEdgeDx * invW;
    const float3 dNumerDy = dEdgeDy * invW;
    const float dDenomDx = dot(dNumerDx, 1.0f.xxx);
    const float dDenomDy = dot(dNumerDy, 1.0f.xxx);

    // Quotient rule in clip space, then map clip derivatives to pixel derivatives.
    const float3 dLambdaDxClip = (dNumerDx * denom - numer * dDenomDx) * invDenom2;
    const float3 dLambdaDyClip = (dNumerDy * denom - numer * dDenomDy) * invDenom2;

    bary.ValueDDX = dLambdaDxClip * (2.0f * ViewInvSize.x);
    bary.ValueDDY = dLambdaDyClip * (-2.0f * ViewInvSize.y);
    return bary;
}

float4 InterpolateOnly(float4 val[3], Barycentrics barycentris)
{
    return val[0] * barycentris.Value.x +
           val[1] * barycentris.Value.y +
           val[2] * barycentris.Value.z;
}

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

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint2 DTid : SV_DispatchThreadID)
{
    if (DTid.x < g_ResolveVBufferToGBuffer.ViewportWidth && DTid.y < g_ResolveVBufferToGBuffer.ViewportHeight)
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
            InstanceConstant inst = LoadInstanceConstant(payload.InstanceIndex);
            MeshConstant meshInstance = LoadMeshConstant(inst.MeshBufferIdx);

            StructuredBuffer<GroupDataLocation> groupDataLocationSRV = GetGroupDataLocationBufferSRV();
            GroupDataLocation groupDataLocation = groupDataLocationSRV[payload.GetGroupIndex()];
            uint groupByteOffset = groupDataLocation.ByteOffset;
            ByteAddressBuffer geometryChunksBuffer = GetGeometryChunksBufferSRV(groupDataLocation.ChunkIndex);
            MeshletHeader meshletHeader = geometryChunksBuffer.Load<MeshletHeader>(
                groupByteOffset + sizeof(GroupHeader) + payload.GetMeshletIndex() * sizeof(MeshletHeader));
            MaterialConstant materilConstant = LoadMaterialConstant(meshletHeader.GetMaterialBufferIndex());
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
            clipPos[0] = mul(g_ResolveVBufferToGBuffer.ViewProjMatrix, float4(worldPos[0], 1.0f));
            clipPos[1] = mul(g_ResolveVBufferToGBuffer.ViewProjMatrix, float4(worldPos[1], 1.0f));
            clipPos[2] = mul(g_ResolveVBufferToGBuffer.ViewProjMatrix, float4(worldPos[2], 1.0f));

            Barycentrics barycentris =
                CalculateTriangleBarycentrics(clipPos, DTid + float2(0.5, 0.5), g_ResolveVBufferToGBuffer.InvViewportSize);

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
            GBufferDUAV[DTid] = float4(0, 0, 0, g_ResolveVBufferToGBuffer.ViewMode);
            if (g_ResolveVBufferToGBuffer.ViewMode == VIEW_MODE_SHOW_MESHLET_LOD)
            {
                if (meshletHeader.GetLODLevel() == 0)
                    GBufferDUAV[DTid].rgb = float3(0.8, 0.8, 0.8);
                else
                    GBufferDUAV[DTid].rgb = MakeDebugColor(meshletHeader.GetLODLevel(), 0x1f3d5b79u);
            }
            else if (g_ResolveVBufferToGBuffer.ViewMode == VIEW_MODE_SHOW_MESHLET_ID)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(vsOutput.meshletIndex, 0x6a09e667u);
            }
            else if (g_ResolveVBufferToGBuffer.ViewMode == VIEW_MODE_SHOW_TRIANGLE)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(primitiveIndex, 0xbb67ae85u);
            }
            else if (g_ResolveVBufferToGBuffer.ViewMode == VIEW_MODE_SHOW_MESH_ID)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(inst.MeshBufferIdx, 0x3c6ef372u);
            }
            else if (g_ResolveVBufferToGBuffer.ViewMode == VIEW_MODE_SHOW_INSTANCE_ID)
            {
                GBufferDUAV[DTid].rgb = MakeDebugColor(payload.InstanceIndex, 0xa54ff53au);
            }
            else if (g_ResolveVBufferToGBuffer.ViewMode == VIEW_MODE_SHOW_MATERIAL_ID)
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
            GBufferAUAV[DTid] = float4(0, 0, 0, 0);
            GBufferBUAV[DTid] = float4(0, 0, 0, 0);
            GBufferCUAV[DTid] = float4(0, 0, 0, 0);
            GBufferDUAV[DTid] = float4(0, 0, 0, 0);
        }
    }
}
