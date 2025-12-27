#include "CommonRS.hlsli"

cbuffer CB : register(b0)
{
    uint ClearValue;
    uint NumElements;
}

RWByteAddressBuffer DstBuffer : register(u0);

[RootSignature(Common_RootSig)]
[numthreads(64, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.x < NumElements)
    {
        DstBuffer.Store(DTid.x * 4, ClearValue);
    }
}