// Used to create HiZ mips instead of the built-in GenerateMips, which provides average depth instead of the needed max depth.
Texture2D<float> SrcMip : register(t0);
RWTexture2D<float> DstMip : register(u0);

cbuffer MipParams : register(b0)
{
	uint2 SrcSize;
	uint2 DstSize;
};

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (any(tid.xy >= DstSize))
		return;

	const int2 src = int2(tid.xy) * 2;

	// Clamped so odd dimensions re-read the edge texel, ensuring accurate max depth
	const int2 maxSrc = int2(SrcSize) - 1;
	float d = SrcMip.Load(int3(min(src, maxSrc), 0));
	d = max(d, SrcMip.Load(int3(min(src + int2(1, 0), maxSrc), 0)));
	d = max(d, SrcMip.Load(int3(min(src + int2(0, 1), maxSrc), 0)));
	d = max(d, SrcMip.Load(int3(min(src + int2(1, 1), maxSrc), 0)));

	DstMip[tid.xy] = d;
}
