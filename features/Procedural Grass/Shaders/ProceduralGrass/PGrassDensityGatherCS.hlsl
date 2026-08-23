#define CSHADER

#include "ProceduralGrass/PGrassCommon.hlsli"

// Build grass coverage directly from the world-space presence map.

Texture2D<uint> GrassPresence : register(t0);  // world-space grass id per sample (0 = bare), 177x177 around the player
RWTexture2D<uint> GrassDensity : register(u0);

// Full coverage saturates the pixel shader's 1..64 grassAODensity range. Partial coverage fades smoothly.
static const float DENSITY_FULL_COVER = 64.0f;

float LoadPresence(int2 coord, int dim)
{
	if (any(coord < 0) || any(coord >= dim))
		return 0.0f;
	
	return GrassPresence[coord] != 0 ? 1.0f : 0.0f;
}

float SamplePresence(float2 sampleCoord, int dim)
{
	int2 s0 = int2(floor(sampleCoord));
	float2 f = sampleCoord - float2(s0);
	float p00 = LoadPresence(s0, dim);
	float p10 = LoadPresence(s0 + int2(1, 0), dim);
	float p01 = LoadPresence(s0 + int2(0, 1), dim);
	float p11 = LoadPresence(s0 + int2(1, 1), dim);

	return lerp(lerp(p00, p10, f.x), lerp(p01, p11, f.x), f.y);
}

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	uint dim = (uint)grassAOParams.x;
	if (tid.x >= dim || tid.y >= dim)
		return;

	// Convert the density texel centre to world space.
	float2 texelUV = (float2(tid.xy) + 0.5f) / dim;
	float2 worldPos = occlusionParams.xy + (texelUV - 0.5f) * (occlusionHalfExtent * 2.0f);

	// Convert to presence-map space. Out-of-bounds samples are zero so the filter fades at map and loaded-LAND edges.
	int presDim = (int)grassPresenceParams.w;
	float2 sampleCoord = (worldPos - grassPresenceParams.xy) * grassPresenceParams.z;
	static const float FILTER_OFFSET = 4.0f;
	
	// Keep density zero outside the original gather coverage.
	bool insideOriginalCoverage = all(sampleCoord >= 0.0f) && all(sampleCoord < float(presDim - 1));
	float rawPresence = insideOriginalCoverage ? SamplePresence(sampleCoord, presDim) : 0.0f;
	
	float filteredPresence = rawPresence * 4.0f;
	filteredPresence += SamplePresence(sampleCoord + float2(FILTER_OFFSET, 0.0f), presDim) * 2.0f;
	filteredPresence += SamplePresence(sampleCoord - float2(FILTER_OFFSET, 0.0f), presDim) * 2.0f;
	filteredPresence += SamplePresence(sampleCoord + float2(0.0f, FILTER_OFFSET), presDim) * 2.0f;
	filteredPresence += SamplePresence(sampleCoord - float2(0.0f, FILTER_OFFSET), presDim) * 2.0f;
	filteredPresence += SamplePresence(sampleCoord + float2(FILTER_OFFSET, FILTER_OFFSET), presDim);
	filteredPresence += SamplePresence(sampleCoord + float2(FILTER_OFFSET, -FILTER_OFFSET), presDim);
	filteredPresence += SamplePresence(sampleCoord + float2(-FILTER_OFFSET, FILTER_OFFSET), presDim);
	filteredPresence += SamplePresence(sampleCoord - float2(FILTER_OFFSET, FILTER_OFFSET), presDim);
	filteredPresence *= 1.0f / 16.0f;

	// Map a 0.5 filtered edge to zero so the fade ends at the boundary without a fade.
	float inwardFade = saturate((filteredPresence - 0.5f) * 2.0f);
	float inwardFade2 = inwardFade * inwardFade;
	inwardFade = inwardFade2 * inwardFade * (inwardFade * (inwardFade * 6.0f - 15.0f) + 10.0f);
	float presence = rawPresence * inwardFade;

	GrassDensity[tid.xy] = (uint)(presence * DENSITY_FULL_COVER + 0.5f);
}
