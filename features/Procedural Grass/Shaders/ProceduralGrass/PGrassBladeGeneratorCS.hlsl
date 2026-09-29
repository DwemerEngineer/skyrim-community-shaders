#include "Common/FastMath.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

#define PSHADER
#include "Common/SharedData.hlsli"
#undef PSHADER

#ifndef CSHADER
#	define CSHADER
#endif

#include "ProceduralGrass/PGrassCommon.hlsli"

#if defined(PGRASS_CACHED_COLLISION)
#	define GRASS_COLLISION_CBUFFER_REGISTER b11
#	if defined(MID_LOD)
#		define GRASS_COLLISION_CURRENT_ONLY
#	endif
#	include "GrassCollision/GrassCollision.hlsli"
#endif

#if defined(SKYLIGHTING) && !defined(LOW_LOD)
#	define SKYLIGHTING_PROBE_REGISTER t50
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(HIGH_LOD) && defined(TERRAIN_SHADOWS)
#	include "TerrainShadows/TerrainShadows.hlsli"
#endif

#if defined(HIGH_LOD) && defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

#define FRAMEBUFFER

#if defined(__INTELLISENSE__)
#	define THREADGROUP_SIZE 8
#	define DENSITY 192
#	define PATCH_BLADE_COUNT 4
#	define SLOPE_EXTRA_BLADES 1
#endif

// Supply defaults for editor parsing and validation builds.
#if !defined(PATCH_BLADE_COUNT)
#	define PATCH_BLADE_COUNT 1
#endif
#if !defined(SLOPE_EXTRA_BLADES)
#	define SLOPE_EXTRA_BLADES 0
#endif

// Match the constant-buffer array to the renderer's quadrant capacity.
#if !defined(QUADRANT_DATA_SIZE)
#	if defined(HIGH_LOD)
#		define QUADRANT_DATA_SIZE 9
#	elif defined(MID_LOD)
#		define QUADRANT_DATA_SIZE 16
#	else  // LOW_LOD
#		define QUADRANT_DATA_SIZE 75
#	endif
#endif

static const int TG_DIM_X = THREADGROUP_SIZE;
static const int TG_DIM_Y = 1;
static const uint BLADES_PER_ROW = DENSITY;

static const uint PATCHES_PER_QUADRANT = BLADES_PER_ROW * BLADES_PER_ROW / 4;
static const float BLADE_TO_WORLD = 2048.0f / BLADES_PER_ROW;

static const float UINT_TO_FLOAT = 1.0f / 4294967296.0f;

struct QuadrantData
{
	float2 quadWorldPos;
	uint quadrantHash;
	uint flags;
};

cbuffer QuadrantData : register(b7)
{
	float4 lodFadeIn;   // x: fade-in start, y: inverse range, z: Far seam-fill retention, w: fade-out endpoint
	float4 lodFadeOut;
	QuadrantData data[QUADRANT_DATA_SIZE];
}

RWStructuredBuffer<Blade> BladeOutput : register(u0);
RWByteAddressBuffer IndirectArgs : register(u1);

static const uint MAX_BLADES_PER_THREAD = 1u + (SLOPE_EXTRA_BLADES + PATCH_BLADE_COUNT - 1u) / PATCH_BLADE_COUNT;

#if defined(HIGH_LOD) || defined(LOW_LOD)
// Adjacent lanes occupy adjacent slots for each emission, reducing shared-memory bank conflicts.
groupshared Blade GroupBlades[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
#if defined(HIGH_GEOMETRY_LOD)
groupshared uint GroupBladeOuter[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
groupshared uint GroupOuterCount;
#endif
#if defined(LOW_LOD) && !defined(FAR_LOD)
groupshared uint GroupThreadOffset[THREADGROUP_SIZE];
#else
groupshared uint GroupInnerCount;
#endif
groupshared uint2 GroupOutputBase;
#endif

Texture2D<float> TerrainHeightTexture : register(t0);
SamplerState LinearSampler : register(s0);

static const uint QUADRANT_GRASS_PITCH = 17;
static const float QUADRANT_GRASS_SPACING = 2048.0f / 16.0f;

// Loaded quadrants use exact 17x17 LAND heights instead of the quantized heightmap.
StructuredBuffer<float> QuadrantHeights : register(t3);

// The low 12 bits select QuadrantData. The remaining bits store the blade slot and flags.
StructuredBuffer<uint> VisibleBladeTasks : register(t5);
StructuredBuffer<uint> QuadrantGrassCells : register(t6);  // Packed 2x2 LAND IDs for each quadrant cell.

#if defined(HIGH_LOD)
Texture2D<uint> GrassDensityTexture : register(t7);
#endif

Texture2D<float> GrassHiZ : register(t8);                   // Shared current-frame scene-depth pyramid.

static const uint WORK_QUADRANT_MASK = 0xFFFu;
static const uint WORK_LANE_SHIFT = 12u;
static const uint WORK_HAS_LAND = 1u << 16u;
static const uint WORK_INSIDE_FRUSTUM = 1u << 17u;
static const uint WORK_ALLOW_SLOPE_EXTRAS = 1u << 18u;
static const uint WORK_NEAR_COVERED = 1u << 19u;
static const uint WORK_COMPACT_FAR = 1u << 20u;
static const uint WORK_OCCUPIED_TILE = 1u << 21u;
static const uint WORK_TILE_SHIFT = 22u;
static const uint WORK_TILE_MASK = 0xFFu;

static const uint PATCHES_PER_ROW = BLADES_PER_ROW / 2u;
static const uint PATCH_ROWS = (PATCHES_PER_QUADRANT + PATCHES_PER_ROW - 1u) / PATCHES_PER_ROW;
static const uint OCCUPANCY_TILES_PER_AXIS = QUADRANT_GRASS_PITCH - 1u;
static const uint MAX_TILE_PATCH_WIDTH = (PATCHES_PER_ROW + OCCUPANCY_TILES_PER_AXIS - 1u) / OCCUPANCY_TILES_PER_AXIS;

// Start slope-fill seeds after High's four base slots to keep their positions consistent across tiers.
static const uint SLOPE_EXTRA_SEED_BASE = 4u;

bool IsPatchOccluded(float2 worldXY, float terrainZ, bool cullsDisabled)
{
#if defined(MID_LOD) || defined(LOW_LOD) || defined(FAR_LOD)
	if (cullsDisabled || grassHiZParams.w < 1.0f)
		return false;

	const float bladeHeight = max(grassAOParams.w, 64.0f);
	const float radius = max(grassHiZParams.z, 96.0f);
	const float3 centre = float3(worldXY, terrainZ + bladeHeight * 0.5f) - FrameBuffer::CameraPosAdjust.xyz;
	const float distanceSq = dot(centre, centre);
	// Large projected bounds make Hi-Z ineffective close to the camera.
	if (distanceSq < 4096.0f * 4096.0f)
		return false;
	const float distanceToCentre = sqrt(distanceSq);

	const float4 clipCentre = mul(FrameBuffer::CameraViewProj, float4(centre, 1.0f));
	if (clipCentre.w <= 0.0f)
		return false;

	const float2 uv = (clipCentre.xy / clipCentre.w) * float2(0.5f, -0.5f) + 0.5f;
	if (any(uv <= 0.0f) || any(uv >= 1.0f))
		return false;

	const float2 hiZSize = grassHiZParams.xy;
	const float projectionScale = max(cameraViewRow0Sum, cameraViewRow1Sum);
	const float radiusTexels = radius * projectionScale * (0.5f * max(hiZSize.x, hiZSize.y)) / clipCentre.w;
	const float wantedMip = ceil(log2(max(radiusTexels * 2.0f, 1.0f)));
	if (wantedMip > grassHiZParams.w - 1.0f)
		return false;

	const int mip = (int)wantedMip;
	const float mipScale = exp2((float)mip);
	const float2 centreTexel = uv * hiZSize / mipScale;
	const float mipRadius = radiusTexels / mipScale;
	const int2 minTexel = int2(floor(centreTexel - mipRadius));
	const int2 maxTexel = int2(floor(centreTexel + mipRadius));
	const int2 mipSize = max(int2(ceil(hiZSize / mipScale)), int2(1, 1));

	float tileMax = 0.0f;
	[unroll] for (int y = 0; y < 3; ++y) {
		[unroll] for (int x = 0; x < 3; ++x) {
			if (minTexel.x + x <= maxTexel.x && minTexel.y + y <= maxTexel.y) {
				const int2 texel = clamp(minTexel + int2(x, y), int2(0, 0), mipSize - 1);
				tileMax = max(tileMax, GrassHiZ.Load(int3(texel, mip)));
			}
		}
	}

	const float3 nearest = centre * (max(distanceToCentre - radius, 0.0f) / distanceToCentre);
	const float4 clipNearest = mul(FrameBuffer::CameraViewProj, float4(nearest, 1.0f));
	const float nearestDepth = clipNearest.z / max(clipNearest.w, 1.0e-4f);
	return nearestDepth > tileMax + 0.003f;
#else
	return false;
#endif
}

// Return bilinear LAND height and slope from the same four corner loads.
bool SampleLandHeightSlope(out float height, out float2 slope, float2 quadLocalPos, uint quadrant, bool hasLand)
{
	uint quadrantBase = quadrant * (QUADRANT_GRASS_PITCH * QUADRANT_GRASS_PITCH);

	height = 0.0f;
	slope = float2(0.0f, 0.0f);

	if (!hasLand)
		return false;

	float2 gridPosition = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);
	int2 baseSample = int2(gridPosition);
	float2 sampleFraction = gridPosition - baseSample;

	uint lowerLeftIndex = quadrantBase + baseSample.y * QUADRANT_GRASS_PITCH + baseSample.x;
	float heightLowerLeft = QuadrantHeights[lowerLeftIndex];
	float heightLowerRight = QuadrantHeights[lowerLeftIndex + 1];
	float heightUpperLeft = QuadrantHeights[lowerLeftIndex + QUADRANT_GRASS_PITCH];
	float heightUpperRight = QuadrantHeights[lowerLeftIndex + QUADRANT_GRASS_PITCH + 1];

	height = lerp(lerp(heightLowerLeft, heightLowerRight, sampleFraction.x), lerp(heightUpperLeft, heightUpperRight, sampleFraction.x), sampleFraction.y);
	// Analytic bilinear gradient in world units.
	slope = float2(
		lerp(heightLowerRight - heightLowerLeft, heightUpperRight - heightUpperLeft, sampleFraction.y),
		lerp(heightUpperLeft - heightLowerLeft, heightUpperRight - heightLowerRight, sampleFraction.x)) * (1.0f / QUADRANT_GRASS_SPACING);

	return true;
}

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

Texture2D<float> OcclusionMaskHigh : register(t2);
Texture2D<float> OcclusionMaskLow : register(t4);

float GetObjectClearance(float3 worldPos, bool cullsDisabled)
{
	if (cullsDisabled)
		return 1.0e30f;

	float2 uv = (worldPos.xy - occlusionParams.xy) * occlusionInvExtent + 0.5f;
	float2 mapUV = saturate(uv);

	if (any(mapUV != uv))
		return 1.0e30f;

	uint2 texel = min(uint2(mapUV * occlusionMapDim), uint2(occlusionMapDim - 1u, occlusionMapDim - 1u));
	float highest = OcclusionMaskHigh.Load(int3(texel, 0));

	if (highest <= worldPos.z + occlusionParams.w)
		return 1.0e30f;

	float lowest = OcclusionMaskLow.Load(int3(texel, 0));
	float clearance = lowest - worldPos.z;
	return clearance < occlusionParams.z ? clearance : 1.0e30f;
}

void ComputeClump(out uint clumpRand, out float clumpDist, out float2 clumpDir, float2 worldPos, float inverseGridSize)
{
	float2 gridPos = worldPos * inverseGridSize;
	// Floor keeps the Voronoi grid continuous across negative world coordinates.
	int2 gridCell = int2(floor(gridPos));

	uint3 centerHash = Random::pcg3d(uint3(asuint(gridCell), 0u));
	float2 centerFeature = float2(gridCell) + float2(centerHash.xy) * UINT_TO_FLOAT;
	clumpDir = centerFeature - gridPos;
	clumpDist = dot(clumpDir, clumpDir);
	clumpRand = centerHash.z;

	for (int y = gridCell.y - 1; y <= gridCell.y + 1; y++) {
		for (int x = gridCell.x - 1; x <= gridCell.x + 1; x++) {
			if (x == gridCell.x && y == gridCell.y)
				continue;

			float2 cellMin = float2(x, y);
			float2 cellOffset = clamp(gridPos, cellMin, cellMin + 1.0f) - gridPos;
			if (dot(cellOffset, cellOffset) >= clumpDist)
				continue;

			uint3 hash = Random::pcg3d(uint3(asuint(x), asuint(y), 0u));
			float2 featurePos = cellMin + float2(hash.xy) * UINT_TO_FLOAT;
			float2 offset = featurePos - gridPos;
			float distanceSquared = dot(offset, offset);

			if (distanceSquared < clumpDist) {
				clumpDist = distanceSquared;
				clumpDir = offset;
				clumpRand = hash.z;
			}
		}
	}

	clumpDist = sqrt(clumpDist);
}

uint LoadGrassCell(float2 quadLocalPos, uint quadrant)
{
	float2 grassSample = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);
	int2 baseSample = int2(grassSample);
	return QuadrantGrassCells[quadrant * 256u + baseSample.y * 16u + baseSample.x];
}

float2 GrassMapSamplePos(float2 bladeQuadPos2D, uint3 hash)
{
	return bladeQuadPos2D + (float2(hash.xy) * UINT_TO_FLOAT * 2.0f - 1.0f) * miscParams.x;
}

void ComputeGrassType(out uint type, uint packedGrassCell, float2 quadLocalPos, float typeRandom)
{
	uint firstType = packedGrassCell & 0xFFu;
	if (packedGrassCell == firstType * 0x01010101u) {
		type = firstType;
		return;
	}

	float2 grassSample = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);
	float2 sampleFraction = frac(grassSample);

	float2 inverseFraction = 1.0f - sampleFraction;
	float firstThreshold = inverseFraction.x * inverseFraction.y;
	float secondThreshold = firstThreshold + sampleFraction.x * inverseFraction.y;
	float thirdThreshold = secondThreshold + inverseFraction.x * sampleFraction.y;

	if (typeRandom < firstThreshold)
		type = packedGrassCell & 0xFFu;
	else if (typeRandom < secondThreshold)
		type = (packedGrassCell >> 8u) & 0xFFu;
	else if (typeRandom < thirdThreshold)
		type = (packedGrassCell >> 16u) & 0xFFu;
	else
		type = packedGrassCell >> 24u;
}

// Far can complete its LOD test before terrain and grass-map access.
bool PassesEarlyFarLOD(float2 bladeWorldPos2D, bool nearCovered, bool compactFar, bool cullsDisabled)
{
#if defined(FAR_LOD)
	if (cullsDisabled)
		return true;

	float2 lodOffset = abs(bladeWorldPos2D - grassLodOrigin);
	float lodDistanceSq = dot(lodOffset, lodOffset);
	float handoffDistance = max(lodOffset.x, lodOffset.y);
	float inRamp = 1.0f;
	if (nearCovered) {
		// Bring Far in before Low's cutoff so the tiers overlap.
		float handoffRamp = saturate((handoffDistance - lodFadeIn.x) * lodFadeIn.y + 0.25f);
		float fallbackRamp = saturate((handoffDistance - (lodFadeIn.x - 2.0f * rcp(lodFadeIn.y))) * (lodFadeIn.y * 0.5f));
		float fallbackKeep = lodFadeIn.z * 0.25f * fallbackRamp;
		inRamp = max(handoffRamp, fallbackKeep);
	}

	float fullKeepRadius = min(lodFadeOut.x, farParams.x);
	// Low fades on square distance, so retain Far through the same handoff band.
	if (handoffDistance <= fullKeepRadius) {
		if (!compactFar && inRamp >= 1.0f)
			return true;

		float keep = compactFar ? saturate(inRamp / max(farParams.w, 1.0e-3f)) : inRamp;
		float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;
		return dither <= keep;
	}

	float unloadFadeEnd = lodFadeIn.w + rcp(max(lodFadeOut.w, 1.0e-6f));
	if (lodFadeOut.w > 0.0f && lodDistanceSq >= unloadFadeEnd * unloadFadeEnd)
		return false;

	float lodDistance = sqrt(lodDistanceSq);
	float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
	float unloadRamp = 1.0f - saturate((lodDistance - lodFadeIn.w) * lodFadeOut.w);
	outRamp *= unloadRamp;
	float projectedKeep = GetFarPerformanceKeep(lodDistance, FrameBuffer::CameraProj._m00);
	float keep = min(inRamp, outRamp) * projectedKeep;
	// Ease back to radial thinning after Low is gone, including at diagonal corners.
	float handoffEnd = lodFadeIn.x + rcp(max(lodFadeIn.y, 1.0e-6f));
	float seamKeep = inRamp * (1.0f - saturate((handoffDistance - handoffEnd) * lodFadeIn.y)) * unloadRamp;
	keep = max(keep, seamKeep);
	if (compactFar)
		keep = saturate(keep / max(farParams.w, 1.0e-3f));
	float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;

	return dither <= keep;
#else
	return true;
#endif
}

// Return smooth world-space variation from four integer hashes.
float CalculateWindNoise(float2 worldPosition)
{
	static const float WIND_NOISE_CELL_SIZE = 512.0f;
	float2 cellPosition = worldPosition * (1.0f / WIND_NOISE_CELL_SIZE);
	int2 baseCell = int2(floor(cellPosition));
	float2 cellFraction = frac(cellPosition);
	float2 blend = cellFraction * cellFraction * (3.0f - 2.0f * cellFraction);

	float2 noiseLower = float2(
		Random::iqint3(asuint(baseCell)),
		Random::iqint3(asuint(baseCell + int2(1, 0)))) * UINT_TO_FLOAT;
	float2 noiseUpper = float2(
		Random::iqint3(asuint(baseCell + int2(0, 1))),
		Random::iqint3(asuint(baseCell + int2(1, 1)))) * UINT_TO_FLOAT;
	return lerp(lerp(noiseLower.x, noiseLower.y, blend.x), lerp(noiseUpper.x, noiseUpper.y, blend.x), blend.y) * 2.0f - 1.0f;
}

// Vanilla grass's gust waveform with smooth field variation and stable per-blade offsets.
float CalculateWindDisplacement(float2 worldPosition, float timer, float speed, float bladeHeight, float windNoise, float bladePhase, float bladeStrength)
{
	float gustAngle = 0.4f * ((worldPosition.x + worldPosition.y) * -0.0078125f + timer) + windNoise * 0.5f + bladePhase;

	float gustSin, gustCos;
	sincos(gustAngle, gustSin, gustCos);

	float gust0 = 0.2f * cos(Math::PI * gustCos);
	float gust1 = sin(Math::PI * gustSin);
	float gust2 = sin(Math::TAU * gustSin);
	float gustStrength = max(0.35f, (1.0f + windNoise * 0.35f) * bladeStrength);

	// Taller blades receive a stronger gust response. 150 units matches the maximum possible grass height.
	float heightResponse = bladeHeight * lerp(0.55f, 1.20f, saturate(bladeHeight * (1.0f / 150.0f)));
	return heightResponse * speed * gustStrength * ((gust1 + gust2) * 0.3f + gust0) * 0.5f;
}

float CalculateWindAdjustedAngle(float clumpedAngle, float angle, float rotationScale, float rotationalStiffness, float scaledWidth, float bladeHeight)
{
	float diff = angle - clumpedAngle;
	if (diff > Math::PI)
		diff -= Math::TAU;
	else if (diff < -Math::PI)
		diff += Math::TAU;

	float alignment = cos(diff) * 0.5f + 0.5f;
	float rotationFactor = lerp(0.2f, 1.0f, alignment * 0.5f);
	float totalRotation = rotationFactor * rotationScale * scaledWidth * bladeHeight;
	float reducedRotation = totalRotation * rcp(rotationalStiffness * totalRotation + 1.0f);
	float clampedRotation = min(reducedRotation, abs(diff)) * sign(diff);
	return clumpedAngle + clampedRotation;
}

#if !defined(FAR_LOD)
bool PassesBladeLOD(float2 bladeWorldPos2D, bool cullsDisabled)
{
	if (cullsDisabled)
		return true;

	float2 lodOffset = abs(bladeWorldPos2D - grassLodOrigin);

#if defined(MID_LOD)
	float lodDistance = length(lodOffset);
	float inRamp = saturate((lodDistance - lodFadeIn.x) * lodFadeIn.y);
	float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
	float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;
	return !((inRamp < 1.0f && dither <= 1.0f - inRamp) || dither > outRamp);
#else
	float lodDistanceSq = dot(lodOffset, lodOffset);
#if defined(LOW_LOD)
	float lodFadeOutDistance = max(lodOffset.x, lodOffset.y);
#else
	float lodFadeOutDistanceSq = lodDistanceSq;
#endif
	float lodFadeInEnd = lodFadeIn.x + rcp(max(lodFadeIn.y, 1.0e-6f));
	float lodFadeInStartSq = lodFadeIn.x * lodFadeIn.x;
	float lodFadeInEndSq = lodFadeInEnd * lodFadeInEnd;
#if defined(LOW_LOD)
	bool beforeFadeOut = lodFadeOutDistance <= lodFadeOut.x;
	bool afterFadeOut = lodFadeOutDistance >= lodFadeIn.w;
#else
	float lodFadeOutStartSq = lodFadeOut.x * lodFadeOut.x;
	float lodFadeOutEndSq = lodFadeIn.w * lodFadeIn.w;
	bool beforeFadeOut = lodFadeOutDistanceSq <= lodFadeOutStartSq;
	bool afterFadeOut = lodFadeOutDistanceSq >= lodFadeOutEndSq;
#endif

#if defined(LOW_LOD)
	if (lodDistanceSq <= lodFadeInStartSq)
		return false;
#endif

	if (lodDistanceSq >= lodFadeInEndSq && beforeFadeOut)
		return true;

	float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;
	if (lodDistanceSq >= lodFadeInEndSq && afterFadeOut)
		return dither <= lodFadeOut.z;

	float lodDistance = sqrt(lodDistanceSq);
	float inRamp = saturate((lodDistance - lodFadeIn.x) * lodFadeIn.y);
#if defined(LOW_LOD)
	float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodFadeOutDistance - lodFadeOut.x) * lodFadeOut.y));
#else
	float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
#endif
#if defined(LOW_LOD)
	if ((inRamp < 1.0f && dither <= 1.0f - inRamp) || dither > outRamp)
#elif defined(HIGH_LOD)
	if (dither > min(inRamp, outRamp))
#endif
		return false;
	return true;
#endif
}
#endif

// Finish one base or slope-fill candidate after establishing its terrain plane.
bool BuildBlade(uint3 initialHash, float2 mapSamplePos, float2 initialWorldPos2D, float bladeWorldZ, float2 terrainSlope, float terrainNormalZ,
	float2 quadWorldPos, uint quadrant, bool hasLand, uint packedGrassCell, bool cullsDisabled, bool insideFrustum, bool baseCandidate, out Blade b, out bool outerGeometry)
{
	b = (Blade)0;
	outerGeometry = false;

	uint3 hash = initialHash;
	float2 bladeWorldPos2D = initialWorldPos2D;
	float typeRandom = float(hash.z) * UINT_TO_FLOAT;

#if !defined(LOW_LOD) && !defined(FAR_LOD)
	// Tier dithering controls density, so distance culling begins at the fade endpoint.
	if (!baseCandidate && !cullsDisabled) {
		float cullDistance = lodFadeIn.w;
		float2 cullOffset = bladeWorldPos2D - grassLodOrigin;
		if (dot(cullOffset, cullOffset) >= cullDistance * cullDistance)
			return false;
	}
#endif

	// Fetch the grass type after culling to avoid the four-sample lookup for rejected blades.
	uint type;
	ComputeGrassType(type, packedGrassCell, mapSamplePos, typeRandom);
	if (type == 0u && !cullsDisabled)
		return false;
	type = max(type, 1u);

	GrassGeneratorType generatorType = generatorGrassType[type];
#if defined(FAR_LOD)
	if (!baseCandidate) {
		bladeWorldZ = TerrainHeightSlopeAt(terrainSlope, bladeWorldPos2D, quadWorldPos, quadrant, hasLand);
		terrainNormalZ = rsqrt(dot(terrainSlope, terrainSlope) + 1.0f);
	}
#endif
#if !defined(LOW_LOD) || defined(FAR_LOD)
	if (!cullsDisabled && (terrainNormalZ < generatorType.maxSlope || terrainNormalZ > generatorType.minSlope))
		return false;
#endif

#if defined(LOW_LOD) && !defined(FAR_LOD)
	// Clump density reaches zero at half a grid cell, bounding the inward fade's displacement.
	float clumpReach = voronoiGridSize * 0.1125f * abs(generatorType.clumpDistanceFactor);
	float innerCullRadius = max(lodFadeIn.x - clumpReach - 1.0f, 0.0f);
	float2 initialLodOffset = bladeWorldPos2D - grassLodOrigin;
	if (!cullsDisabled && lodFadeIn.y > 0.0f && dot(initialLodOffset, initialLodOffset) < innerCullRadius * innerCullRadius)
		return false;
#endif

	float3 worldPos;
	float3 viewPos;
	float objectClearance = 1.0e30f;
#if defined(FAR_LOD)
	worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	objectClearance = GetObjectClearance(worldPos, cullsDisabled);
	viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;

	if (!insideFrustum) {
		float widthExtent = generatorType.width * 2.5f * 1.3f * 32.0f * 1.6f;
		float geometryExtent = generatorType.height + widthExtent;
		float4 clip = mul(FrameBuffer::CameraViewProjUnjittered, float4(viewPos, 1.0f));
		bool outsideFrustum = clip.x + clip.w < -frustumPlaneExtent.x * geometryExtent ||
			clip.w - clip.x < -frustumPlaneExtent.y * geometryExtent ||
			clip.y + clip.w < -frustumPlaneExtent.z * geometryExtent ||
			clip.w - clip.y < -frustumPlaneExtent.w * geometryExtent;
		if (outsideFrustum && !cullsDisabled)
			return false;
	}

	if (!cullsDisabled && objectClearance <= occlusionParams.w)
		return false;
#endif

	// Delay the nine-cell clump search until after the inexpensive rejection tests.
	uint clumpRand;
	float clumpDist;
	float2 clumpDir;
	ComputeClump(clumpRand, clumpDist, clumpDir, bladeWorldPos2D, inverseVoronoiGridSize);
	float clumpDensity = 1.0f - smoothstep(0.15f, 0.50f, clumpDist);

	hash = Random::pcg3d(hash);
	float clumpDistRand = float(hash.x) * UINT_TO_FLOAT;
	float heightRand = float(hash.y) * UINT_TO_FLOAT;
	float angleRand = float(hash.z) * UINT_TO_FLOAT;

#if !defined(FAR_LOD)
	// Pull every near blade toward its Voronoi feature to hide the regular candidate lattice.
	float clumpPull = lerp(0.025f, 0.225f, clumpDistRand);
	float2 clumpDisplace = clumpDir * voronoiGridSize * clumpPull * generatorType.clumpDistanceFactor * clumpDensity;
	bladeWorldPos2D += clumpDisplace;

#if defined(LOW_LOD)
	if (!PassesBladeLOD(bladeWorldPos2D, cullsDisabled))
		return false;

	float2 displacedQuadPos = bladeWorldPos2D - quadWorldPos;
	if (hasLand && all(displacedQuadPos >= 0.0f) && all(displacedQuadPos < 2048.0f)) {
		bladeWorldZ = TerrainHeightSlopeAt(terrainSlope, bladeWorldPos2D, quadWorldPos, quadrant, true);
		terrainNormalZ = rsqrt(dot(terrainSlope, terrainSlope) + 1.0f);
	} else {
		bladeWorldZ += dot(terrainSlope, clumpDisplace);
	}

	if (!cullsDisabled && (terrainNormalZ < generatorType.maxSlope || terrainNormalZ > generatorType.minSlope))
		return false;
#else
	bladeWorldZ += dot(terrainSlope, clumpDisplace);
#endif
#endif
#if !defined(FAR_LOD)
	worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	objectClearance = GetObjectClearance(worldPos, cullsDisabled);
	viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;

	if (!insideFrustum) {
		// A root outside the frustum can still produce visible blade geometry near the edge.
		float widthExtent = generatorType.width * 2.5f * 1.3f;
#if defined(LOW_LOD)
		widthExtent *= 2.0f * (1.0f + miscParams.z);
#elif defined(MID_LOD)
		widthExtent *= 1.41421356f * (1.0f + miscParams.z);
#else
		widthExtent *= 1.0f + miscParams.z;
#endif
		float geometryExtent = generatorType.height + widthExtent;
		float4 clip = mul(FrameBuffer::CameraViewProjUnjittered, float4(viewPos, 1.0f));
		bool outsideFrustum = clip.x + clip.w < -frustumPlaneExtent.x * geometryExtent ||
			clip.w - clip.x < -frustumPlaneExtent.y * geometryExtent ||
			clip.y + clip.w < -frustumPlaneExtent.z * geometryExtent ||
			clip.w - clip.y < -frustumPlaneExtent.w * geometryExtent;
		if (outsideFrustum && !cullsDisabled)
			return false;
	}

#if !defined(LOW_LOD)
	if (!PassesBladeLOD(bladeWorldPos2D, cullsDisabled))
		return false;
#endif

	// Preserve grass beneath overhangs when there is still vertical room for part of the blade.
	if (!cullsDisabled && objectClearance <= occlusionParams.w)
		return false;
#endif

	// Height generation is deferred until after rejection because the frustum test uses type bounds.
	float clumpHeightRandom = float(clumpRand) * UINT_TO_FLOAT * clumpDensity;
	float unscaledHeight = (0.45f + heightRand * 0.55f) - clumpHeightRandom * generatorType.clumpHeightFactor;
	float randHeight = generatorType.height * unscaledHeight;
	if (objectClearance < 1.0e29f) {
		randHeight = min(randHeight, objectClearance - occlusionParams.w);
		unscaledHeight = randHeight / max(generatorType.height, 1.0e-4f);
	}

	// Store the authored width variation. High also bakes its stable distance widening below.
	float unscaledWidth = 1.0f;
	float widthRand = frac(heightRand * 1.618f + angleRand * 0.5f);
	unscaledWidth *= lerp(0.6f, 1.0f, widthRand);
	float scaledWidth = unscaledWidth * generatorType.width * 2.5f;

	hash = Random::pcg3d(hash);

	// Align the random facing toward the clump centre.
	float randAngle = angleRand * Math::TAU;

#if defined(FAR_LOD)
	float clumpedAngle = randAngle;
#else
	float clumpAngle = atan2(-clumpDir.y, -clumpDir.x);
	if (clumpAngle < 0.0f)
		clumpAngle += Math::TAU;
	float delta = clumpAngle - randAngle;

	if (delta < 0.0f)
		delta += Math::TAU;
	else if (delta >= Math::TAU)
		delta -= Math::TAU;

	float clumpedAngle = randAngle + delta * generatorType.clumpFacingFactor * clumpDensity;

	if (clumpedAngle < 0.0f)
		clumpedAngle += Math::TAU;
	else if (clumpedAngle >= Math::TAU)
		clumpedAngle -= Math::TAU;
#endif

	// Lean the blade downhill before applying wind.
	if (miscParams.y > 0.0f) {
		float slopeSteepness = sqrt(saturate(1.0f - terrainNormalZ * terrainNormalZ));
		if (slopeSteepness > 1e-4f) {
			float downhillAngle = atan2(-terrainSlope.y, -terrainSlope.x);

			if (downhillAngle < 0.0f)
				downhillAngle += Math::TAU;

			float slopeDiff = downhillAngle - clumpedAngle;
			if (slopeDiff > Math::PI)
				slopeDiff -= Math::TAU;
			else if (slopeDiff < -Math::PI)
				slopeDiff += Math::TAU;

			clumpedAngle += slopeDiff * miscParams.y * slopeSteepness;
			if (clumpedAngle < 0.0f)
				clumpedAngle += Math::TAU;
			else if (clumpedAngle >= Math::TAU)
				clumpedAngle -= Math::TAU;
		}
	}

	// Turn toward the wind without rotating beyond it.
	float windAdjustedAngle = CalculateWindAdjustedAngle(clumpedAngle, windAngle, windRotationScale, generatorType.rotationalStiffness, scaledWidth, randHeight);
#if defined(HIGH_LOD) || defined(MID_LOD)
	float windNoise = CalculateWindNoise(bladeWorldPos2D);
	float bladeWindPhase = (float(hash.x) * UINT_TO_FLOAT - 0.5f) * 0.45f;
	float bladeWindStrength = lerp(0.65f, 1.35f, float(hash.y) * UINT_TO_FLOAT);
	float windDisplacement = CalculateWindDisplacement(bladeWorldPos2D, SharedData::Timer, windSpeed, randHeight, windNoise, bladeWindPhase, bladeWindStrength);
#	if defined(HIGH_LOD)
	float previousWindDisplacement = CalculateWindDisplacement(bladeWorldPos2D, SharedData::Timer - miscParams.w, previousWindSpeed, randHeight, windNoise, bladeWindPhase, bladeWindStrength);
#	else
	float previousWindDisplacement = 0.0f;
#	endif
#else
	float windDisplacement = 0.0f;
	float previousWindDisplacement = 0.0f;
#endif

#if !defined(FAR_LOD)
	float facingSin, facingCos;
	sincos(windAdjustedAngle, facingSin, facingCos);
	float2 randFacing = float2(facingCos, facingSin);
#endif

	float storedWidth = unscaledWidth;

#if defined(HIGH_LOD)
	float appearanceDistance = ApproximateGrassDistance(bladeWorldPos2D - grassLodOrigin);
	outerGeometry = appearanceDistance >= 1024.0f;
	float distanceWidth = lerp(0.4f, 1.0f, saturate((appearanceDistance - 1024.0f) * (1.0f / 3072.0f)));
	storedWidth *= distanceWidth;
	uint packedWidth = (uint)round(saturate(storedWidth) * 255.0f);
#else
	uint packedWidth = (uint)(unscaledWidth * 255.0f);
#endif

	uint packedHeight = (uint)(unscaledHeight * 255.0f);
#if defined(LOW_LOD) && !defined(FAR_LOD)
	float lowDrawHeight = generatorType.height * float(packedHeight) * (1.0f / 255.0f);
	uint storedHeight = lowDrawHeight <= 45.0f;
#else
	uint storedHeight = packedHeight;
#endif
	b.posXY = f32tof16(viewPos.x) << 16 | f32tof16(viewPos.y);
	b.posZWidthHeight = f32tof16(viewPos.z) << 16 | packedWidth << 8 | storedHeight;

	// Keep the geometry hash independent of the camera-dependent view-thickening byte.
	uint stableBladeHash = (hash.z << 12) | ((clumpRand & 15u) << 8) | type;

#if defined(MID_LOD) || (defined(LOW_LOD) && !defined(FAR_LOD))
	float2 viewOffset = grassLodOrigin - bladeWorldPos2D;
	float2 viewDirection = viewOffset * rsqrt(max(dot(viewOffset, viewOffset), 1.0e-4f));
	float viewDotNormal = abs(dot(randFacing, viewDirection));
	float viewDotNormal2 = viewDotNormal * viewDotNormal;
	float viewThicken = 1.0f - viewDotNormal2 * viewDotNormal2;

	float2 rotatedFacing = float2(randFacing.x * 0.8660254f - randFacing.y * 0.5f, randFacing.x * 0.5f + randFacing.y * 0.8660254f);
	float rotatedViewDotNormal = abs(dot(rotatedFacing, viewDirection));
	float rotatedViewDotNormal2 = rotatedViewDotNormal * rotatedViewDotNormal;
	float rotatedViewThicken = 1.0f - rotatedViewDotNormal2 * rotatedViewDotNormal2;

	uint packedViewThicken = (uint)round(saturate(viewThicken) * 15.0f) | (uint)round(saturate(rotatedViewThicken) * 15.0f) << 4;
	uint packedClumpDensity = (uint)round(clumpDensity * 255.0f);
	uint hashClumpAndGrassType = type | (clumpRand & 0xFFu) << 8 | packedViewThicken << 16 | packedClumpDensity << 24;
#elif defined(HIGH_LOD)
	float2 viewOffset = grassLodOrigin - bladeWorldPos2D;
	float2 viewDirection = viewOffset * rsqrt(max(dot(viewOffset, viewOffset), 1.0e-4f));
	float viewDotNormal = saturate(dot(randFacing, viewDirection));
	float viewDotNormal2 = viewDotNormal * viewDotNormal;
	float viewThicken = (1.0f - viewDotNormal2 * viewDotNormal2) * smoothstep(0.0f, 0.2f, viewDotNormal);

	float2 rotatedFacing = float2(randFacing.x * 0.8660254f - randFacing.y * 0.5f, randFacing.x * 0.5f + randFacing.y * 0.8660254f);
	float rotatedViewDotNormal = saturate(dot(rotatedFacing, viewDirection));
	float rotatedViewDotNormal2 = rotatedViewDotNormal * rotatedViewDotNormal;
	float rotatedViewThicken = (1.0f - rotatedViewDotNormal2 * rotatedViewDotNormal2) * smoothstep(0.0f, 0.2f, rotatedViewDotNormal);

	uint packedClumpDensity = (uint)round(clumpDensity * 15.0f);
	uint packedViewThicken = (uint)round(saturate(max(viewThicken, rotatedViewThicken)) * 15.0f);
	uint hashClumpAndGrassType = type | (clumpRand & 0xFFu) << 8 | packedClumpDensity << 16 | packedViewThicken << 20;
#else
	uint hashClumpAndGrassType = stableBladeHash;
#endif

	// Precompute the blade tilt.
	uint2 tiltHash = Random::pcg2d(uint2(stableBladeHash, 0u));
	float randTilt = generatorType.tipWeight * (float(tiltHash.x) * UINT_TO_FLOAT * 1.4f + 0.30f);

#if defined(FAR_LOD)
	// Compute Far directions once per blade and pack them as eight-bit values.
	float facingSin, facingCos;
	float tiltSin, tiltCos;

	sincos(windAdjustedAngle, facingSin, facingCos);
	sincos(randTilt, tiltSin, tiltCos);

	uint4 packedDirections = (uint4)round(saturate(float4(facingCos, facingSin, tiltSin, tiltCos) * 0.5f + 0.5f) * 255.0f);
	b.facingTilt = packedDirections.x | packedDirections.y << 8 | packedDirections.z << 16 | packedDirections.w << 24;

	uint packedClumpDensity = (uint)round(clumpDensity * 255.0f);
	b.seedAndType = packedClumpDensity << 24 | (clumpRand & 0xFFFFu) << 8 | (type & 0xFFu);
#else
	uint packedRandBend = (uint)round(saturate(float(tiltHash.y) * UINT_TO_FLOAT) * 15.0f);

	// Only detailed materials consume the packed per-blade colour seed.
	uint packedBladeColor = 0u;
#if !defined(LOW_LOD)
#	if defined(HIGH_GEOMETRY_LOD)
	[branch] if (!outerGeometry)
#	endif
	{
		GrassType surfaceType = grassType[type];
		float bladeColorRand = float(tiltHash.x) * UINT_TO_FLOAT;
		float bladeValueRand = float(tiltHash.y) * UINT_TO_FLOAT;
		float3 hueTint = lerp(surfaceType.grassColorCool.rgb, surfaceType.grassColorWarm.rgb, bladeColorRand);
		float bladeValue = 1.0f + (bladeValueRand * 2.0f - 1.0f) * surfaceType.grassColorVar.y;
		float3 perBladeColor = lerp(1.0f, hueTint, surfaceType.grassColorVar.x) * bladeValue;

		float clumpColorRand = (float(clumpRand & 0xFFu) + 0.5f) * (1.0f / 256.0f);
		float clumpValueRand = (float((clumpRand >> 8) & 0xFFu) + 0.5f) * (1.0f / 256.0f);
		float3 clumpTint = lerp(surfaceType.grassColorCool.rgb, surfaceType.grassColorWarm.rgb, clumpColorRand);
		float clumpValue = 1.0f + (clumpValueRand * 2.0f - 1.0f) * surfaceType.grassColorVar.y * 0.75f;
		perBladeColor *= lerp(1.0f, clumpTint * clumpValue, surfaceType.clumpColorStrength * clumpDensity);

		// Pack blade-wide colour variation. The pixel shader evaluates spatial blotch and grain detail.
		uint3 packedColor = (uint3)round(saturate(perBladeColor * 0.5f) * 15.0f);
		packedBladeColor = packedColor.x | packedColor.y << 4u | packedColor.z << 8u;
	}
#endif
	uint packedBladeData = packedBladeColor | packedRandBend << 12u;

	float tiltSin, tiltCos;
	sincos(randTilt, tiltSin, tiltCos);

#if defined(LOW_LOD) && !defined(FAR_LOD)
	uint lowTiltX = f32tof16(tiltSin);
	uint lowTiltY = f32tof16(tiltCos);
	float2 lowTip = float2(f16tof32(lowTiltX), f16tof32(lowTiltY)) * lowDrawHeight;
	float lowRandBend = generatorType.stiffness * (0.25f + float(packedRandBend) * (1.6f / 15.0f));
	float2 lowMidPoint = lowTip * generatorType.mid + float2(-lowTip.y, lowTip.x) * lowRandBend;
	float lowWidthScale = float(packedWidth) * (1.0f / 255.0f);
	float lowRandWidth = generatorType.width * 5.0f * lerp(0.45f, 1.3f, lowWidthScale);
#endif

#if defined(HIGH_LOD)
	float2 densityUV = (bladeWorldPos2D - occlusionParams.xy) * occlusionInvExtent + 0.5f;
	float onMapDensity = 1.0f;
	float densityEdgeFade = 0.0f;
	if (all(densityUV >= 0.0f) && all(densityUV <= 1.0f)) {
		uint densityDimension = max((uint)grassAOParams.x, 1u);
		uint2 densityTexel = min(uint2(densityUV * densityDimension), densityDimension - 1u);
		float bladeCount = GrassDensityTexture[densityTexel];
		onMapDensity = saturate(bladeCount / max(grassAOParams.z, 1.0f));
		densityEdgeFade = saturate(min(min(densityUV.x, 1.0f - densityUV.x), min(densityUV.y, 1.0f - densityUV.y)) * 10.0f);
	}

	float canopyDensity = lerp(1.0f, onMapDensity, densityEdgeFade);
	float canopyAODensity = onMapDensity * densityEdgeFade;
	uint packedCanopy = (uint)round(canopyDensity * 15.0f) | (uint)round(canopyAODensity * 15.0f) << 4;
	hashClumpAndGrassType |= packedCanopy << 24;

	float worldShadow = 1.0f;
#	if defined(TERRAIN_SHADOWS)
	worldShadow *= TerrainShadows::GetTerrainShadow(worldPos, LinearSampler);
#	endif
#	if defined(CLOUD_SHADOWS)
	worldShadow *= CloudShadows::GetCloudShadowMult(viewPos, LinearSampler);
#	endif

	uint2 packedTilt = (uint2)round(saturate(float2(tiltSin, tiltCos) * 0.5f + 0.5f) * 255.0f);
	uint packedWorldShadow = (uint)round(saturate(worldShadow) * 255.0f);
	b.tipDir = packedTilt.x | packedTilt.y << 8 | packedWorldShadow << 16;
#elif defined(MID_LOD)
	uint2 packedTilt = (uint2)round(saturate(float2(tiltSin, tiltCos) * 0.5f + 0.5f) * 255.0f);
	float appearanceDistance = ApproximateGrassDistance(bladeWorldPos2D - grassLodOrigin);
	uint packedLodDistance = (uint)round(saturate(appearanceDistance * (1.0f / 6144.0f)) * 65535.0f);
	b.tipDir = packedTilt.x | packedTilt.y << 8 | packedLodDistance << 16;
#else
	b.tipDir = f32tof16(lowTip.x) << 16 | f32tof16(lowTip.y);
#endif
	b.hashClumpAndGrassType = hashClumpAndGrassType;

	int2 packedFacing = (int2)round(clamp(randFacing, -1.0f, 1.0f) * 127.0f);
#if defined(LOW_LOD) && !defined(FAR_LOD)
	b.facingAndWind = (uint)(packedFacing.x & 0xFF) | (uint)(packedFacing.y & 0xFF) << 8 | f32tof16(lowRandWidth) << 16;
	b.previousWind = f32tof16(lowMidPoint.x) << 16 | f32tof16(lowMidPoint.y);
#else
	b.facingAndWind = (uint)(packedFacing.x & 0xFF) | (uint)(packedFacing.y & 0xFF) << 8 | f32tof16(windDisplacement) << 16;
	b.previousWind = packedBladeData << 16 | f32tof16(previousWindDisplacement);
#endif

#if !defined(LOW_LOD)
#	if defined(SKYLIGHTING)
	float3 probeCell = round(FrameBuffer::CameraPosAdjust.xyz / Skylighting::CELL_SIZE);
	float3 probeOffset = probeCell * Skylighting::CELL_SIZE - FrameBuffer::CameraPosAdjust.xyz;
	uint3 probeArrayOrigin = (uint3)((int3)probeCell - (int3)(Skylighting::ARRAY_DIM / 2)) % Skylighting::ARRAY_DIM;
	float3 skylightingPosition = viewPos;
#		if defined(MID_LOD)
	float3 probeExtent = Skylighting::ARRAY_SIZE * 0.5f - Skylighting::CELL_SIZE;
	skylightingPosition = clamp(viewPos - probeOffset, -probeExtent, probeExtent) + probeOffset;
#		endif
	sh2 skylightingSH = Skylighting::SampleWithOrigin(skylightingPosition, float3(0.0f, 0.0f, 1.0f), probeOffset, probeArrayOrigin);

	b.skylightingSH0 = f32tof16(skylightingSH.x) << 16 | f32tof16(skylightingSH.y);
	b.skylightingSH1 = f32tof16(skylightingSH.z) << 16 | f32tof16(skylightingSH.w);
#	else
	b.skylightingSH0 = 0u;
	b.skylightingSH1 = 0u;
#	endif
#endif

#if defined(PGRASS_CACHED_COLLISION)
	// Cache one tip collision sample. The VS scales it smoothly from the anchored root.
	float2 collisionTip = float2(tiltSin, tiltCos) * randHeight;
	float3 collisionTipViewPos = viewPos + float3(randFacing * collisionTip.x, collisionTip.y);
	collisionTipViewPos.xy += windDir * windDisplacement;

	float3 collisionDisplacement;

#if defined(MID_LOD)
	float3 unusedPreviousCollisionDisplacement;
	GrassCollision::GetDisplacedPosition(collisionTipViewPos, viewPos, 1.0f, 2048.0f, true, 0.75f,
		collisionDisplacement, unusedPreviousCollisionDisplacement);
	b.collisionData = f32tof16(collisionDisplacement.x) << 16 | f32tof16(collisionDisplacement.y);
	b.previousWind = packedBladeData << 16 | f32tof16(collisionDisplacement.z);
#else
	float3 previousCollisionDisplacement;
	GrassCollision::GetDisplacedPosition(collisionTipViewPos, viewPos, 1.0f, 2048.0f, true, 0.75f,
		collisionDisplacement, previousCollisionDisplacement);

	b.collisionData.x = f32tof16(collisionDisplacement.x) << 16 | f32tof16(collisionDisplacement.y);
	b.collisionData.y = f32tof16(collisionDisplacement.z) << 16 | f32tof16(previousCollisionDisplacement.x);
	b.collisionData.z = f32tof16(previousCollisionDisplacement.y) << 16 | f32tof16(previousCollisionDisplacement.z);
#endif
#endif
#endif

	return true;
}

void GenerateThreadBlades(uint3 dispatch, uint groupIndex, out uint2 emittedBladeCounts)
{
	emittedBladeCounts = 0u;
	uint emittedBladeCount = 0u;

	uint patch = dispatch.x;
	uint bladeTask = VisibleBladeTasks[dispatch.z];
	uint bladeIndex = (bladeTask >> WORK_LANE_SHIFT) & 0xFu;
	uint quadrant = bladeTask & WORK_QUADRANT_MASK;

	bool hasLand = (bladeTask & WORK_HAS_LAND) != 0u;
	bool insideFrustum = (bladeTask & WORK_INSIDE_FRUSTUM) != 0u;
	bool allowSlopeExtras = (bladeTask & WORK_ALLOW_SLOPE_EXTRAS) != 0u;
	bool nearCovered = (bladeTask & WORK_NEAR_COVERED) != 0u;
	bool compactFar = (bladeTask & WORK_COMPACT_FAR) != 0u;
	bool occupiedTile = (bladeTask & WORK_OCCUPIED_TILE) != 0u;

	if (occupiedTile) {
		uint tile = (bladeTask >> WORK_TILE_SHIFT) & WORK_TILE_MASK;
		uint2 tilePos = uint2(tile & 15u, tile >> 4u);
		uint2 patchStart = uint2(tilePos.x * PATCHES_PER_ROW, tilePos.y * PATCH_ROWS) / OCCUPANCY_TILES_PER_AXIS;
		uint2 patchEnd = uint2((tilePos.x + 1u) * PATCHES_PER_ROW, (tilePos.y + 1u) * PATCH_ROWS) / OCCUPANCY_TILES_PER_AXIS;
		uint2 tilePatchDim = patchEnd - patchStart;
		uint2 localPatch = uint2(patch % MAX_TILE_PATCH_WIDTH, patch / MAX_TILE_PATCH_WIDTH);

		if (any(localPatch >= tilePatchDim))
			return;

		patch = (patchStart.y + localPatch.y) * PATCHES_PER_ROW + patchStart.x + localPatch.x;
		if (patch >= PATCHES_PER_QUADRANT)
			return;
	}

#if defined(FAR_LOD)
	if (compactFar) {
		uint activePatchCount = max(1u, (uint)ceil(PATCHES_PER_QUADRANT * saturate(farParams.w)));

		if (dispatch.x >= activePatchCount)
			return;

		// An odd permutation spreads the retained candidates over the entire quadrant.
		patch = (patch * 40501u + data[quadrant].quadrantHash) % PATCHES_PER_QUADRANT;
	}
#endif

	bool cullsDisabled = debugFlags.x > 0.5f;
	QuadrantData quadrantData = data[quadrant];
	uint2 patchPos = uint2(patch % PATCHES_PER_ROW, patch / PATCHES_PER_ROW);
	uint quadrantHash = quadrantData.quadrantHash;

	// Preserve the base blade slot's position and seed.
	uint patchHash = Random::iqint3(patchPos);
	uint bladeIndexRandomiser = (patchHash >> 16) & 3;
	uint randomBladeIndex = bladeIndex ^ bladeIndexRandomiser;
	uint2 pos = patchPos * 2u + uint2(randomBladeIndex >> 1u, randomBladeIndex & 1u);

	uint3 baseHash = Random::pcg3d(uint3(pos, quadrantHash));
	float2 baseJitter = float2(baseHash.xy) * UINT_TO_FLOAT;
#if defined(FAR_LOD)
	baseJitter *= 0.5f;
#endif

	float2 baseQuadPos2D = (float2(pos) + baseJitter) * BLADE_TO_WORLD;
	float2 baseWorldPos2D = baseQuadPos2D + quadrantData.quadWorldPos;
	float2 baseMapSamplePos = GrassMapSamplePos(baseQuadPos2D, baseHash);
	uint baseGrassCell = 0u;
	bool useBasePath = PassesEarlyFarLOD(baseWorldPos2D, nearCovered, compactFar, cullsDisabled);
#if !defined(LOW_LOD) && !defined(FAR_LOD)
	if (!cullsDisabled) {
		float2 baseLodXY = baseWorldPos2D - grassLodOrigin;
		float baseDistSq = dot(baseLodXY, baseLodXY);
		float baseCullDist = lodFadeIn.w;
		if (baseDistSq >= baseCullDist * baseCullDist)
			useBasePath = false;
	}
#endif

	if (useBasePath) {
		baseGrassCell = LoadGrassCell(baseMapSamplePos, quadrant);

		if (!cullsDisabled && baseGrassCell == 0u)
			useBasePath = false;
	}

#if defined(FAR_LOD)
	bool hasValidCandidate = useBasePath;
#if SLOPE_EXTRA_BLADES > 0
	if (!hasValidCandidate && allowSlopeExtras) {
		for (uint extraIndex = 0; extraIndex < SLOPE_EXTRA_BLADES; ++extraIndex) {
			if ((extraIndex % PATCH_BLADE_COUNT) != bladeIndex)
				continue;

			uint oldBladeIndex = SLOPE_EXTRA_SEED_BASE + extraIndex;
			uint3 extraHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float2 extraQuadPos = (float2(patchPos * 2u) + float2(extraHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
			float2 extraMapSamplePos = GrassMapSamplePos(extraQuadPos, extraHash);
			if (!PassesEarlyFarLOD(extraQuadPos + quadrantData.quadWorldPos, nearCovered, compactFar, cullsDisabled))
				continue;
			if (cullsDisabled || LoadGrassCell(extraMapSamplePos, quadrant) != 0u) {
				hasValidCandidate = true;
				break;
			}
		}
	}
#endif
	if (!hasValidCandidate)
		return;
#elif defined(LOW_LOD) && SLOPE_EXTRA_BLADES > 0
	if (!useBasePath && !cullsDisabled) {
		bool anyExtraGrass = false;
		[unroll] for (uint extraIndex = 0u; extraIndex < SLOPE_EXTRA_BLADES; ++extraIndex) {
			uint oldBladeIndex = SLOPE_EXTRA_SEED_BASE + extraIndex;
			uint3 extraHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float2 extraQuadPos = (float2(patchPos * 2u) + float2(extraHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
			if (LoadGrassCell(GrassMapSamplePos(extraQuadPos, extraHash), quadrant) != 0u) {
				anyExtraGrass = true;
				break;
			}
		}

		if (!anyExtraGrass)
			return;
	}
#else
#if SLOPE_EXTRA_BLADES > 0
	if (!useBasePath && bladeIndex >= SLOPE_EXTRA_BLADES)
		return;
#else
	if (!useBasePath)
		return;
#endif
#endif

	// One bilinear terrain sample establishes the plane for this path and its extras.
	float2 terrainSlope;
	float baseWorldZ = TerrainHeightSlopeAt(terrainSlope, baseWorldPos2D, quadrantData.quadWorldPos, quadrant, hasLand);
	if (useBasePath && IsPatchOccluded(baseWorldPos2D, baseWorldZ, cullsDisabled))
		return;
	float terrainNormalZ = rsqrt(dot(terrainSlope, terrainSlope) + 1.0f);

#if SLOPE_EXTRA_BLADES > 0
	// Reject slope extras before grass typing, clumping, LOD, occlusion, wind, and packing.
	float baseSlopeKeep = saturate(1.0f / max(terrainNormalZ, 0.05f) - 1.0f);
#if defined(LOW_LOD)
	// Fill distant hills more strongly without reserving more candidate slots.
	baseSlopeKeep = saturate(baseSlopeKeep * 2.0f);
#endif
	// Keep one emit path and let FXC choose the legal loop form for each permutation.
	for (uint candidateIndex = 0; candidateIndex < 1 + SLOPE_EXTRA_BLADES; ++candidateIndex) {
		bool isBase = candidateIndex == 0;
		uint3 candidateHash = baseHash;
		float2 candidateWorldPos = baseWorldPos2D;
		float2 candidateMapSamplePos = baseMapSamplePos;
		uint packedGrassCell = baseGrassCell;
		float candidateWorldZ = baseWorldZ;
		bool candidateValid = useBasePath;

		if (!isBase) {
			uint emitExtraIndex = candidateIndex - 1;
			uint oldBladeIndex = SLOPE_EXTRA_SEED_BASE + emitExtraIndex;
#if defined(FAR_LOD)
			if (!allowSlopeExtras)
				continue;

			candidateHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float2 extraQuadPos = (float2(patchPos * 2u) + float2(candidateHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
			candidateWorldPos = extraQuadPos + quadrantData.quadWorldPos;
			candidateMapSamplePos = GrassMapSamplePos(extraQuadPos, candidateHash);
			candidateValid = PassesEarlyFarLOD(candidateWorldPos, nearCovered, compactFar, cullsDisabled);
			if (!candidateValid)
				continue;
			packedGrassCell = LoadGrassCell(candidateMapSamplePos, quadrant);
			if (!cullsDisabled && packedGrassCell == 0u)
				continue;

			float slopeKeep = baseSlopeKeep;
			float extraKeep = max(saturate(lodFadeIn.z + slopeKeep), farParams.w);
			float keepRand = float(Random::pcg3d(uint3(asuint(candidateWorldPos), oldBladeIndex)).x) * UINT_TO_FLOAT;

			if (!cullsDisabled && keepRand > extraKeep)
				continue;
#else
			if ((emitExtraIndex % PATCH_BLADE_COUNT) != bladeIndex)
				continue;

			candidateHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
#if !defined(LOW_LOD)
			float slopeRoll = float(candidateHash.z) * UINT_TO_FLOAT;
			if (!cullsDisabled && slopeRoll > baseSlopeKeep)
				continue;
#endif

			float2 candidateQuadPos = (float2(patchPos * 2u) + float2(candidateHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
			candidateWorldPos = candidateQuadPos + quadrantData.quadWorldPos;
			candidateMapSamplePos = GrassMapSamplePos(candidateQuadPos, candidateHash);
			packedGrassCell = LoadGrassCell(candidateMapSamplePos, quadrant);
			if (!cullsDisabled && packedGrassCell == 0u)
				continue;
			candidateValid = true;
#endif
			candidateWorldZ = baseWorldZ + dot(terrainSlope, candidateWorldPos - baseWorldPos2D);
		}

		if (!candidateValid)
			continue;

		Blade blade;
		bool outerGeometry;
		if (BuildBlade(candidateHash, candidateMapSamplePos, candidateWorldPos, candidateWorldZ,
			terrainSlope, terrainNormalZ, quadrantData.quadWorldPos, quadrant, hasLand, packedGrassCell, cullsDisabled, insideFrustum, isBase, blade, outerGeometry)) {
#if defined(MID_LOD)
			uint outputIndex;
			IndirectArgs.InterlockedAdd(4u, 1u, outputIndex);
			BladeOutput[outputIndex] = blade;
#else
			GroupBlades[emittedBladeCount * THREADGROUP_SIZE + groupIndex] = blade;
#endif
#if defined(HIGH_GEOMETRY_LOD)
			GroupBladeOuter[emittedBladeCount * THREADGROUP_SIZE + groupIndex] = outerGeometry ? 1u : 0u;
#endif

			if (outerGeometry)
				emittedBladeCounts.y++;
			else
				emittedBladeCounts.x++;
			emittedBladeCount++;
		}
	}
#else
	if (useBasePath) {
		Blade blade;
		bool outerGeometry;
		if (BuildBlade(baseHash, baseMapSamplePos, baseWorldPos2D, baseWorldZ, terrainSlope, terrainNormalZ, quadrantData.quadWorldPos, quadrant, hasLand, baseGrassCell, cullsDisabled, insideFrustum, true, blade, outerGeometry)) {
#if defined(MID_LOD)
			uint outputIndex;
			IndirectArgs.InterlockedAdd(4u, 1u, outputIndex);
			BladeOutput[outputIndex] = blade;
#else
			GroupBlades[groupIndex] = blade;
#endif
#if defined(HIGH_GEOMETRY_LOD)
			GroupBladeOuter[groupIndex] = outerGeometry ? 1u : 0u;
#endif

			if (outerGeometry)
				emittedBladeCounts.y = 1u;
			else
				emittedBladeCounts.x = 1u;
		}
	}
#endif
}

[numthreads(TG_DIM_X, TG_DIM_Y, 1)] void main(uint3 dispatch : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
	uint2 emittedBladeCounts;
	GenerateThreadBlades(dispatch, groupIndex, emittedBladeCounts);

#if defined(HIGH_LOD) || defined(LOW_LOD)
#if defined(LOW_LOD) && !defined(FAR_LOD)
	GroupThreadOffset[groupIndex] = emittedBladeCounts.x;
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		uint groupBladeCount = 0u;
		[loop] for (uint i = 0u; i < THREADGROUP_SIZE; ++i) {
			uint threadBladeCount = GroupThreadOffset[i];
			GroupThreadOffset[i] = groupBladeCount;
			groupBladeCount += threadBladeCount;
		}

		GroupOutputBase = 0u;
		if (groupBladeCount != 0u)
			IndirectArgs.InterlockedAdd(4u, groupBladeCount, GroupOutputBase.x);
	}

	GroupMemoryBarrierWithGroupSync();

	uint emittedBladeCount = emittedBladeCounts.x;
	uint outputBase = GroupOutputBase.x + GroupThreadOffset[groupIndex];
	[loop] for (uint i = 0u; i < emittedBladeCount; ++i)
		BladeOutput[outputBase + i] = GroupBlades[i * THREADGROUP_SIZE + groupIndex];
#else
	if (groupIndex == 0u) {
		GroupInnerCount = 0u;
#if defined(HIGH_GEOMETRY_LOD)
		GroupOuterCount = 0u;
#endif
		GroupOutputBase = uint2(0u, 0u);
	}

	GroupMemoryBarrierWithGroupSync();

	uint2 threadOutputOffset = uint2(0u, 0u);
	if (emittedBladeCounts.x != 0u)
		InterlockedAdd(GroupInnerCount, emittedBladeCounts.x, threadOutputOffset.x);
#if defined(HIGH_GEOMETRY_LOD)
	if (emittedBladeCounts.y != 0u)
		InterlockedAdd(GroupOuterCount, emittedBladeCounts.y, threadOutputOffset.y);
#endif

	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		if (GroupInnerCount != 0u)
			IndirectArgs.InterlockedAdd(4u, GroupInnerCount, GroupOutputBase.x);
#if defined(HIGH_GEOMETRY_LOD)
		if (GroupOuterCount != 0u) {
			uint oldOuterEnd;
			IndirectArgs.InterlockedAdd(24u, GroupOuterCount, oldOuterEnd);
			IndirectArgs.InterlockedAdd(36u, 0u - GroupOuterCount, oldOuterEnd);
			GroupOutputBase.y = oldOuterEnd - GroupOuterCount;
		}
#endif
	}

	GroupMemoryBarrierWithGroupSync();

#if defined(HIGH_GEOMETRY_LOD)
	uint2 categoryOffset = 0u;
#endif
	uint emittedBladeCount = emittedBladeCounts.x + emittedBladeCounts.y;
	[loop] for (uint i = 0u; i < emittedBladeCount; ++i) {
#if defined(HIGH_GEOMETRY_LOD)
		uint outer = GroupBladeOuter[i * THREADGROUP_SIZE + groupIndex];
		uint outputIndex;
		if (outer != 0u)
			outputIndex = GroupOutputBase.y + threadOutputOffset.y + categoryOffset.y++;
		else
			outputIndex = GroupOutputBase.x + threadOutputOffset.x + categoryOffset.x++;
#else
		uint outputIndex = GroupOutputBase.x + threadOutputOffset.x + i;
#endif
		BladeOutput[outputIndex] = GroupBlades[i * THREADGROUP_SIZE + groupIndex];
	}
#endif
#endif
}
