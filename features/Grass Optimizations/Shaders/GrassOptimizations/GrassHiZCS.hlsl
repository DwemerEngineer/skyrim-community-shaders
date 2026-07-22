// Reduces the scene depth copy to a 1/16-resolution MAX-depth buffer used by the grass cull for
// occlusion testing.
//
// MAX rather than MIN: depth here is standard (0 = near, 1 = far, see SharedData::GetScreenDepth),
// so the largest value in a tile is the FARTHEST occluding surface. An instance is hidden only if
// its nearest point is behind that, which means every occluder in the tile is in front of it.
//
// The consequence is a conservative test in the safe direction: a tile containing sky, or any gap,
// reduces to 1.0 and can never cull. Coarse tiles therefore cost us culling opportunities but can
// never remove grass that is actually visible.

// TERRAIN_BLENDING ON  -> bound to TerrainBlending::blendedDepthTexture (R32_FLOAT) — must NOT be unorm.
// TERRAIN_BLENDING OFF -> bound to the game's kPOST_ZPREPASS_COPY (R24_UNORM_X8_TYPELESS) — unorm.
#if defined(TERRAIN_BLENDING)
Texture2D<float> SrcDepth : register(t0);
#else
Texture2D<unorm float> SrcDepth : register(t0);
#endif

RWTexture2D<float> HiZ : register(u0);

cbuffer HiZParams : register(b0)
{
	uint2 SrcSize;
	uint2 DstSize;
};

groupshared float g_max[256];

// One 16x16 group consumes one 16x16 source block and emits a single texel, so every source texel
// is read exactly once and the reads stay coalesced.
[numthreads(16, 16, 1)] void main(uint3 gid
								  : SV_GroupID, uint3 tid
								  : SV_GroupThreadID, uint gi
								  : SV_GroupIndex) {
	const uint2 src = gid.xy * 16 + tid.xy;

	// Out of bounds reads as "far" so a partial edge tile can only under-cull.
	float d = 1.0;
	if (all(src < SrcSize))
		d = SrcDepth.Load(int3(src, 0));

	g_max[gi] = d;
	GroupMemoryBarrierWithGroupSync();

	for (uint s = 128; s > 0; s >>= 1) {
		if (gi < s)
			g_max[gi] = max(g_max[gi], g_max[gi + s]);
		GroupMemoryBarrierWithGroupSync();
	}

	if (gi == 0 && all(gid.xy < DstSize))
		HiZ[gid.xy] = g_max[0];
}
