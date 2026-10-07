#ifndef __PGRASS_PLACEMENT_HLSLI__
#define __PGRASS_PLACEMENT_HLSLI__

#include "ProceduralGrass/PGrassCoverage.hlsli"

float SampleTerrainHeightMap(float2 world2D)
{
	return lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, world2D * heightMapScale + heightMapOffset, 0));
}

float TerrainHeightSlopeAt(out float2 slope, float2 world2D, float2 quadWorldPos, uint quadrant, bool hasLand)
{
	float height;
	if (SampleLandHeightSlope(height, slope, world2D - quadWorldPos, quadrant, hasLand))
		return height;

	float coarseHeight = SampleTerrainHeightMap(world2D);
	float eps = QUADRANT_GRASS_SPACING;
	float hR = SampleTerrainHeightMap(world2D + float2(eps, 0.0f));
	float hU = SampleTerrainHeightMap(world2D + float2(0.0f, eps));
	slope = float2(hR - coarseHeight, hU - coarseHeight) * (1.0f / eps);
	return coarseHeight;
}

#if defined(LOW_LOD)
/**
 * @brief Returns how far to raise a root onto the rendered terrain at a world position, blended between map cells.
 * Both distant tiers share the lift throughout their overlap. The surface height caps it at LAND crests;
 * the lift prevents coarse height interpolation from raising roots in LAND depressions.
 * Approach distance controls the transition, rather than time or cell loading.
 */
float GetTerrainLift(float2 world2D, float landHeight)
{
	float2 uv = frac(world2D * (1.0f / (TerrainLiftCellSize * TerrainLiftDim)));
	float lift = TerrainSurfaceLift.SampleLevel(LinearSampler, uv, 0);
	float surfaceHeight = TerrainSurfaceHeight.SampleLevel(LinearSampler, uv, 0);
	float2 offset = abs(world2D - grassLodOrigin);
	lift = min(lift, max(surfaceHeight - landHeight, 0.0f));
	return clamp(lift, 0.0f, TerrainLiftMax) * GetTerrainLiftBlend(max(offset.x, offset.y));
}
#endif

uint LoadGrassCell(float2 quadLocalPos, uint quadrant)
{
	float2 grassSample = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);
	int2 baseSample = int2(grassSample);
	return QuadrantGrassCells[quadrant * 256u + baseSample.y * 16u + baseSample.x];
}

bool PatchHasGrass(uint2 patchPos, uint quadrant)
{
	float patchWidth = 2.0f * BLADE_TO_WORLD;
	float noise = max(miscParams.x, 0.0f);
	int2 minCell = clamp(int2(floor((float2(patchPos) * patchWidth - noise) * (1.0f / QUADRANT_GRASS_SPACING))), int2(0, 0), int2(15, 15));
	int2 maxCell = clamp(int2(floor(((float2(patchPos) + 1.0f) * patchWidth + noise) * (1.0f / QUADRANT_GRASS_SPACING))), int2(0, 0), int2(15, 15));
	uint mask = ((1u << (maxCell.x - minCell.x + 1)) - 1u) << minCell.x;
	[loop] for (int y = minCell.y; y <= maxCell.y; ++y)
	{
		if ((OccupancyRows[quadrant * OCCUPANCY_TILES_PER_AXIS + y] & mask) != 0u)
			return true;
	}
	return false;
}

float2 GrassMapSamplePos(float2 bladeQuadPos2D, float2 bladeWorldPos2D, uint3 hash)
{
	float2 jitter = float2(hash.xy) * (2.0f * UINT_TO_FLOAT) - 1.0f;
	return bladeQuadPos2D + lerp(jitter, GetGrassMapWarp(bladeWorldPos2D), GrassMapWarpShare) * miscParams.x;
}

void ComputeGrassType(out uint type, uint packedGrassCell, float2 quadLocalPos, float typeRandom, float2 bladeWorldPos2D, float edgeRandom)
{
	uint firstType = packedGrassCell & 0xFFu;
	if (packedGrassCell == firstType * 0x01010101u) {
		type = firstType;
		return;
	}

	float2 grassSample = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);
	float2 sampleFraction = frac(grassSample);
	float2 inverseFraction = 1.0f - sampleFraction;
	float4 weights = float4(inverseFraction.x * inverseFraction.y, sampleFraction.x * inverseFraction.y, inverseFraction.x * sampleFraction.y,
		sampleFraction.x * sampleFraction.y);

	// Cells without a bare corner stay fully covered; only cells reaching bare LAND get the edge fade.
	uint4 ids = (packedGrassCell.xxxx >> uint4(0u, 8u, 16u, 24u)) & 0xFFu;
	float4 grassWeights = ids != 0u ? weights : 0.0f;
	float presence = dot(grassWeights, 1.0f);
	[branch] if (any(ids == 0u))
	{
		if (edgeRandom >= GetGrassMapEdgeCoverage(bladeWorldPos2D, presence)) {
			type = 0u;
			return;
		}
	}

	// Pick among the grass corners by their bilinear weight.
	float pick = typeRandom * presence;
	float firstThreshold = grassWeights.x;
	float secondThreshold = firstThreshold + grassWeights.y;
	float thirdThreshold = secondThreshold + grassWeights.z;

	if (pick < firstThreshold && ids.x != 0u)
		type = ids.x;
	else if (pick < secondThreshold && ids.y != 0u)
		type = ids.y;
	else if (pick < thirdThreshold && ids.z != 0u)
		type = ids.z;
	else
		type = ids.w != 0u ? ids.w : (ids.z != 0u ? ids.z : (ids.y != 0u ? ids.y : ids.x));
}

#if defined(FAR_LOD)
/** @brief Returns the share of Far candidates its handoff, distance and unload fades keep at a position. */
float GetFarLODKeep(float2 bladeWorldPos2D, bool nearCovered)
{
	float2 lodOffset = abs(bladeWorldPos2D - grassLodOrigin);
	float lodDistanceSq = dot(lodOffset, lodOffset);
	float handoffDistance = max(lodOffset.x, lodOffset.y);
	float inRamp = 1.0f;
	if (nearCovered) {
		// Use the complement of Low's fade-out throughout their shared band.
		inRamp = smoothstep(0.0f, 1.0f, (handoffDistance - lodFadeIn.x) * lodFadeIn.y);
	}

	float fullKeepRadius = min(lodFadeOut.x, farParams.x);
	// Low fades on square distance, so retain Far through the same handoff band.
	if (handoffDistance <= fullKeepRadius)
		return inRamp;

	float unloadFadeEnd = lodFadeIn.w + rcp(max(lodFadeOut.w, 1.0e-6f));
	if (lodFadeOut.w > 0.0f && lodDistanceSq >= unloadFadeEnd * unloadFadeEnd)
		return 0.0f;

	float lodDistance = sqrt(lodDistanceSq);
	float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
	float unloadRamp = 1.0f - saturate((lodDistance - lodFadeIn.w) * lodFadeOut.w);
	outRamp *= unloadRamp;
	float projectedKeep = GetFarPerformanceKeep(lodDistance, FrameBuffer::CameraProj._m00);
	float keep = min(inRamp, outRamp) * projectedKeep;

	// Ease back to radial thinning after Low is gone, including at diagonal corners.
	float handoffEnd = lodFadeIn.x + rcp(max(lodFadeIn.y, 1.0e-6f));
	float seamKeep = inRamp * (1.0f - saturate((handoffDistance - handoffEnd) * lodFadeIn.y)) * unloadRamp;
	return max(keep, seamKeep);
}
#endif

#endif
