#include "CommonRS.hlsli"

RWTexture2D<float> OutMip1 : register(u0);
RWTexture2D<float> OutMip2 : register(u1);
RWTexture2D<float> OutMip3 : register(u2);
RWTexture2D<float> OutMip4 : register(u3);

Texture2D<float> SrcMip : register(t0);

cbuffer CB0 : register(b0)
{
    uint SrcMipLevel; // Texture level of source mip
    uint NumMipLevels; // Number of OutMips to write: [1, 4]
    uint SrcWidth;
    uint SrcHeight;
}

groupshared float gs_src[64];

void StoreDepth(uint Index, float Depth)
{
    gs_src[Index] = Depth;
}

float LoadDepth(uint Index)
{
    return gs_src[Index];
}

[RootSignature(Common_RootSig)]
[numthreads(8, 8, 1)]
void main(uint GI : SV_GroupIndex, uint3 DTid : SV_DispatchThreadID)
{
    int2 srcBase = DTid.xy << 1; // * 2
    float d00 = SrcMip.Load(int3(min(srcBase, int2(SrcWidth - 1, SrcHeight - 1)), SrcMipLevel));
    float d10 = SrcMip.Load(int3(min(srcBase + int2(1, 0), int2(SrcWidth - 1, SrcHeight - 1)), SrcMipLevel));
    float d01 = SrcMip.Load(int3(min(srcBase + int2(0, 1), int2(SrcWidth - 1, SrcHeight - 1)), SrcMipLevel));
    float d11 = SrcMip.Load(int3(min(srcBase + int2(1, 1), int2(SrcWidth - 1, SrcHeight - 1)), SrcMipLevel));

    float MinDepth = min(min(d00, d10), min(d01, d11));
    OutMip1[DTid.xy] = MinDepth;
    
    if (NumMipLevels == 1)
        return;
    
    StoreDepth(GI, MinDepth);
    GroupMemoryBarrierWithGroupSync();
    
    if ((GI & 0x9) == 0)
    {
        float Src2 = LoadDepth(GI + 0x01);
        float Src3 = LoadDepth(GI + 0x08);
        float Src4 = LoadDepth(GI + 0x09);
        MinDepth = min(min(MinDepth, Src2), min(Src3, Src4));
        OutMip2[DTid.xy / 2] = MinDepth;
        StoreDepth(GI, MinDepth);
    }

    if (NumMipLevels == 2)
        return;

    GroupMemoryBarrierWithGroupSync();

    // This bit mask (binary: 011011) checks that X and Y are multiples of four.
    if ((GI & 0x1B) == 0)
    {
        float Src2 = LoadDepth(GI + 0x02);
        float Src3 = LoadDepth(GI + 0x10);
        float Src4 = LoadDepth(GI + 0x12);
        MinDepth = min(min(MinDepth, Src2), min(Src3, Src4));
        OutMip3[DTid.xy / 4] = MinDepth;
        StoreDepth(GI, MinDepth);
    }

    if (NumMipLevels == 3)
        return;

    GroupMemoryBarrierWithGroupSync();

    // This bit mask would be 111111 (X & Y multiples of 8), but only one
    // thread fits that criteria.
    if (GI == 0)
    {
        float Src2 = LoadDepth(GI + 0x04);
        float Src3 = LoadDepth(GI + 0x20);
        float Src4 = LoadDepth(GI + 0x24);
        MinDepth = min(min(MinDepth, Src2), min(Src3, Src4));
        OutMip4[DTid.xy / 8] = MinDepth;
    }

}