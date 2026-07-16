Texture2D<float> SrcDepth : register(t0);
RWTexture2D<float> DstMip : register(u0);

cbuffer HiZParams : register(b0)
{
    uint2 DstDims;
    uint2 SrcDims;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= DstDims))
        return;
    DstMip[id.xy] = SrcDepth[min(id.xy, SrcDims - 1)];
}