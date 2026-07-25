// Base level of the grass occlusion max-depth pyramid: reduces the scene depth copy 4x.
//
// MAX rather than MIN: depth here is standard (0 = near, 1 = far, see SharedData::GetScreenDepth),
// so the largest value in a block is the FARTHEST occluding surface. An instance is hidden only if
// its nearest point is behind that, which means every occluder in the block is in front of it.
//
// The consequence is a conservative test in the safe direction: a block containing sky, or any
// gap, reduces to 1.0 and can never cull. Coarse blocks cost culling opportunities but can never
// remove grass that is actually visible.
//
// One thread owns one output texel and serially maxes the 4x4 source block beneath it — no
// groupshared, no barriers. Reducing only 4x here and letting the mip chain carry on to coarser
// levels also gives the cull a finer base to test small clumps against.

// Always the game's kPOST_ZPREPASS_COPY (R24_UNORM_X8_TYPELESS), never TerrainBlending's
// blendedDepthTexture — that one is produced at the end of the frame and would be a frame stale
// here, which made grass flicker during fast camera movement.
Texture2D<unorm float> SrcDepth : register(t0);

RWTexture2D<float> HiZ : register(u0);

cbuffer HiZParams : register(b0)
{
	uint2 SrcSize;
	uint2 DstSize;
};

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (any(tid.xy >= DstSize))
		return;

	const int2 src = int2(tid.xy) * 4;

	// Out-of-bounds reads as "far", so a partial tile at the screen edge reduces to 1.0 and can
	// only under-cull.
	float d = 0.0;
	[unroll] for (int y = 0; y < 4; ++y)
	{
		[unroll] for (int x = 0; x < 4; ++x)
		{
			const int2 p = src + int2(x, y);
			d = max(d, all(p < int2(SrcSize)) ? SrcDepth.Load(int3(p, 0)) : 1.0);
		}
	}

	HiZ[tid.xy] = d;
}
