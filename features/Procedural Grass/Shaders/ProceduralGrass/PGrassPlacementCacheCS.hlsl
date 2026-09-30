#include "Common/Random.hlsli"

static const float UINT_TO_FLOAT = 1.0f / 4294967296.0f;
static const float BLADE_TO_WORLD = 2048.0f / DENSITY;
static const uint PATCHES_PER_ROW = DENSITY / 2u;
static const uint PATCHES_PER_QUADRANT = PATCHES_PER_ROW * PATCHES_PER_ROW;

#include "ProceduralGrass/PGrassBasePlacement.hlsli"
#include "ProceduralGrass/PGrassLand.hlsli"
#include "ProceduralGrass/PGrassQuadrants.hlsli"

RWStructuredBuffer<BasePlacement> Placements : register(u0);

[numthreads(64, 1, 1)] void main(uint3 dispatch : SV_DispatchThreadID) {
	uint patch = dispatch.x;
	uint quadrant = dispatch.z;
	if (patch >= PATCHES_PER_QUADRANT || (data[quadrant].flags & WORK_HAS_LAND) == 0u)
		return;
	uint2 patchPos = uint2(patch % PATCHES_PER_ROW, patch / PATCHES_PER_ROW);
	BasePlacement placement;
	uint2 gridPos = BaseGridPosition(patchPos, 0u);
	placement.hash = Random::pcg3d(uint3(gridPos, data[quadrant].quadrantHash));
	float2 quadrantXY = BaseQuadrantPosition(gridPos, placement.hash);
	SampleLandHeightSlope(placement.height, placement.slope, quadrantXY, quadrant, true);
	Placements[quadrant * PATCHES_PER_QUADRANT + patch] = placement;
}
