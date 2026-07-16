Texture2D<float> SrcMip : register(t0);
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

	const uint2 src = id.xy * 2;
	const uint2 lim = SrcDims - 1;

	// Standard Z: the FARTHEST occluder is the MAXIMUM value. Taking the max keeps
	// the test conservative — a tile's stored depth is the deepest occluder in it,
	// so we only cull instances behind everything covering their footprint.
	float d = SrcMip[min(src, lim)];
	d = max(d, SrcMip[min(src + uint2(1, 0), lim)]);
	d = max(d, SrcMip[min(src + uint2(0, 1), lim)]);
	d = max(d, SrcMip[min(src + uint2(1, 1), lim)]);

	// Odd source dimensions: a plain 2x2 skips the last column/row. A skipped texel
	// can only LOWER the max, which under-reports the farthest occluder and causes
	// false culls (visible pops) — fold in the extra samples rather than lose them.
	const bool oddX = (SrcDims.x & 1) != 0;
	const bool oddY = (SrcDims.y & 1) != 0;
	if (oddX)
	{
		d = max(d, SrcMip[min(src + uint2(2, 0), lim)]);
		d = max(d, SrcMip[min(src + uint2(2, 1), lim)]);
	}
	if (oddY)
	{
		d = max(d, SrcMip[min(src + uint2(0, 2), lim)]);
		d = max(d, SrcMip[min(src + uint2(1, 2), lim)]);
	}
	if (oddX && oddY)
		d = max(d, SrcMip[min(src + uint2(2, 2), lim)]);

	DstMip[id.xy] = d;
}