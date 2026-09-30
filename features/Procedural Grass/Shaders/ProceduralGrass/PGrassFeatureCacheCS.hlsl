#include "Common/Random.hlsli"
#include "ProceduralGrass/PGrassCommon.hlsli"
#include "ProceduralGrass/PGrassQuadrants.hlsli"

RWStructuredBuffer<PackedClumpFeature> FeaturePoints : register(u0);

[numthreads(CLUMP_FEATURE_PITCH, CLUMP_FEATURE_PITCH, 1)] void main(uint3 group : SV_GroupID, uint2 localCell : SV_GroupThreadID) {
	int2 quadrantCell = int2(floor(data[group.z].quadWorldPos * inverseVoronoiGridSize));
	int2 cell = quadrantCell + int2(localCell) - 1;
	uint3 hash = Random::pcg3d(uint3(asuint(cell), 0u));
	PackedClumpFeature feature;
	feature.fraction = (hash.x >> 16u) | (hash.y & 0xFFFF0000u);
	feature.random = hash.z;
	FeaturePoints[group.z * CLUMP_FEATURE_PITCH * CLUMP_FEATURE_PITCH + localCell.y * CLUMP_FEATURE_PITCH + localCell.x] = feature;
}
