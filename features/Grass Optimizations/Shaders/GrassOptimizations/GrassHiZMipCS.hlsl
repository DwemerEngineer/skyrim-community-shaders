// Builds one level of the grass occlusion max-depth pyramid from the level above it.
//
// A max reduction is what makes the pyramid usable for conservative culling: a texel at level N
// is exactly the farthest depth of everything under it, so testing a large clump against a single
// coarse texel is equivalent to testing it against every fine texel it covers. That is what lets
// the cull bound its sample count without ever underestimating the max — underestimating would
// cull grass that is actually visible.
//
// Not GenerateMips: that averages, and an averaged depth is neither the max nor meaningful on a
// nonlinear depth buffer.

Texture2D<float> SrcMip : register(t0);
RWTexture2D<float> DstMip : register(u0);

cbuffer MipParams : register(b0)
{
	uint2 SrcSize;
	uint2 DstSize;
};

[numthreads(8, 8, 1)] void main(uint3 tid
								: SV_DispatchThreadID) {
	if (any(tid.xy >= DstSize))
		return;

	const int2 src = int2(tid.xy) * 2;

	// Clamp so odd dimensions re-read the edge texel rather than reading out of bounds. Repeating
	// a real depth keeps the result a true max of the covered area.
	const int2 maxSrc = int2(SrcSize) - 1;
	float d = SrcMip.Load(int3(min(src, maxSrc), 0));
	d = max(d, SrcMip.Load(int3(min(src + int2(1, 0), maxSrc), 0)));
	d = max(d, SrcMip.Load(int3(min(src + int2(0, 1), maxSrc), 0)));
	d = max(d, SrcMip.Load(int3(min(src + int2(1, 1), maxSrc), 0)));

	DstMip[tid.xy] = d;
}
