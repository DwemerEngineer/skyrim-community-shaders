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

#if defined(SKYLIGHTING) && !defined(FAR_LOD)
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
	float4 lodFadeIn;   // x: fade-in start, y: inverse range, z: Far seam-fill retention
	float4 lodFadeOut;
	QuadrantData data[QUADRANT_DATA_SIZE];
}

RWStructuredBuffer<Blade> BladeOutput : register(u0);
RWByteAddressBuffer IndirectArgs : register(u1);

static const uint MAX_BLADES_PER_THREAD = 1u + (SLOPE_EXTRA_BLADES + PATCH_BLADE_COUNT - 1u) / PATCH_BLADE_COUNT;

groupshared Blade GroupBlades[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
groupshared uint GroupBladeOuter[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
groupshared uint GroupInnerCount;
groupshared uint GroupOuterCount;
groupshared uint2 GroupOutputBase;

Texture2D<float> TerrainHeightTexture : register(t0);

static const uint QUADRANT_GRASS_PITCH = 17;
static const float QUADRANT_GRASS_SPACING = 2048.0f / 16.0f;

SamplerState LinearSampler : register(s0);

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

// Conservatively reject Mid and Far patches against the shared max-depth pyramid.
bool IsPatchOccluded(float2 worldXY, float terrainZ, bool cullsDisabled)
{
#if defined(MID_LOD) || defined(FAR_LOD)
	if (cullsDisabled || grassHiZParams.w < 1.0f)
		return false;

	const float bladeHeight = max(grassAOParams.w, 64.0f);
	const float radius = max(bladeHeight * 0.65f, 96.0f);
	const float3 centre = float3(worldXY, terrainZ + bladeHeight * 0.5f) - FrameBuffer::CameraPosAdjust.xyz;
	const float distanceToCentre = max(length(centre), 1.0e-4f);
	// Large projected bounds make Hi-Z ineffective close to the camera.
	if (distanceToCentre < 4096.0f)
		return false;

	const float4 clipCentre = mul(FrameBuffer::CameraViewProj, float4(centre, 1.0f));
	if (clipCentre.w <= 0.0f)
		return false;

	const float2 uv = (clipCentre.xy / clipCentre.w) * float2(0.5f, -0.5f) + 0.5f;
	if (any(uv <= 0.0f) || any(uv >= 1.0f))
		return false;

	const float2 hiZSize = grassHiZParams.xy;
	const float2 screenSize = hiZSize * grassHiZParams.z;
	const float projectionScale = max(cameraViewRow0Sum, cameraViewRow1Sum);
	const float projectedRadiusPixels = radius * projectionScale * (0.5f * max(screenSize.x, screenSize.y)) / clipCentre.w;
	const float radiusTexels = projectedRadiusPixels / max(grassHiZParams.z, 1.0f);
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

// Return terrain height and slope from LAND data or the fallback heightmap.
float TerrainHeightSlopeAt(out float2 slope, float2 world2D, float2 quadWorldPos, uint quadrant, bool hasLand)
{
	float h;
	if (SampleLandHeightSlope(h, slope, world2D - quadWorldPos, quadrant, hasLand))
		return h;

	h = lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, world2D * heightMapScale + heightMapOffset, 0));
	float eps = QUADRANT_GRASS_SPACING;
	float hR = lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, (world2D + float2(eps, 0.0f)) * heightMapScale + heightMapOffset, 0));
	float hU = lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, (world2D + float2(0.0f, eps)) * heightMapScale + heightMapOffset, 0));
	slope = float2(hR - h, hU - h) * (1.0f / eps);

	return h;
}

Texture2D<float> OcclusionMaskHigh : register(t2);
Texture2D<float> OcclusionMaskLow : register(t4);

float GetObjectClearance(float3 worldPos)
{
	float2 uv = (worldPos.xy - occlusionParams.xy) * occlusionInvExtent + 0.5f;

	if (saturate(uv.x) != uv.x || saturate(uv.y) != uv.y)
		return 1.0e30f;

	// TopDownOcclusion includes world-space padding, so a centre sample needs no additional footprint.
	uint width, height;
	OcclusionMaskHigh.GetDimensions(width, height);
	uint2 texel = min(uint2(saturate(uv) * float2(width, height)), uint2(width - 1, height - 1));
	float highest = OcclusionMaskHigh.Load(int3(texel, 0));  // Empty texels hold -1e30 in the maximum map.

	if (highest <= worldPos.z + occlusionParams.w)
		return 1.0e30f;

	float lowest = OcclusionMaskLow.Load(int3(texel, 0));  // Empty texels hold +1e30 in the minimum map.
	float clearance = lowest - worldPos.z;
	return clearance < occlusionParams.z ? clearance : 1.0e30f;
}

void ComputeClump(out uint clumpRand, out float clumpDist, out float2 clumpDir, float2 worldPos, float gridSize, float inverseGridSize)
{
	float2 gridPos = worldPos * inverseGridSize;
	// Floor keeps the Voronoi grid continuous across negative world coordinates.
	int2 gridCell = int2(floor(gridPos));

	clumpDist = 1e30;
	clumpRand = 0;

	for (int y = gridCell.y - 1; y <= gridCell.y + 1; y++) {
		for (int x = gridCell.x - 1; x <= gridCell.x + 1; x++) {
			uint3 hash = Random::pcg3d(uint3(asuint(x), asuint(y), 0u));

			float2 jitter = float2(hash.xy) * UINT_TO_FLOAT;
			float2 featurePos = float2(x, y) + jitter;

			float2 offset = featurePos - gridPos;
			float distanceSquared = dot(offset, offset);

			if (distanceSquared < clumpDist) {
				clumpDist = distanceSquared;
				clumpDir = offset;
				clumpRand = hash.z;
			}
		}
	}

	float invLen = rsqrt(max(clumpDist, 1.0e-8f));
	clumpDist = clumpDist * invLen * gridSize;
	clumpDir *= invLen;
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
	int2 baseSample = int2(grassSample);
	float2 sampleFraction = grassSample - float2(baseSample);

	uint4 ids = uint4(packedGrassCell & 0xFFu, (packedGrassCell >> 8u) & 0xFFu, (packedGrassCell >> 16u) & 0xFFu, packedGrassCell >> 24u);

	float4 weights;
	weights.x = (1.0f - sampleFraction.x) * (1.0f - sampleFraction.y);
	weights.y = sampleFraction.x * (1.0f - sampleFraction.y);
	weights.z = (1.0f - sampleFraction.x) * sampleFraction.y;
	weights.w = sampleFraction.x * sampleFraction.y;

	float weightSum = 0.0f;
	type = ids[3];

	[unroll] for (int j = 0; j < 4; j++)
	{
		weightSum += weights[j];
		if (typeRandom < weightSum) {
			type = ids[j];
			break;
		}
	}
}

// Far can complete its LOD test before terrain and grass-map access.
bool PassesEarlyFarLOD(float2 bladeWorldPos2D, bool nearCovered, bool compactFar, bool cullsDisabled)
{
	bool passes = true;
#if defined(FAR_LOD)
	if (!cullsDisabled) {
		float2 lodOffset = abs(bladeWorldPos2D - grassLodOrigin);
		float lodDistance = length(lodOffset);
		float handoffDistance = max(lodOffset.x, lodOffset.y);
		float inRamp = 1.0f;
		if (nearCovered) {
			float handoffRamp = saturate((handoffDistance - lodFadeIn.x) * lodFadeIn.y);
			float fallbackRamp = saturate((handoffDistance - (lodFadeIn.x - 2.0f * rcp(lodFadeIn.y))) * (lodFadeIn.y * 0.5f));
			float fallbackKeep = lodFadeIn.z * 0.25f * fallbackRamp;
			inRamp = max(handoffRamp, fallbackKeep);
		}
		float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
		float unloadFadeStart = lodFadeOut.x + rcp(lodFadeOut.y);
		outRamp *= 1.0f - saturate((lodDistance - unloadFadeStart) * lodFadeOut.w);
		// Thin only after the Low/Far handoff. Widening in the VS loosely preserves coverage.
		float projectedKeep = GetFarPerformanceKeep(lodDistance, FrameBuffer::CameraProj._m00);
		float performanceKeep = compactFar ? saturate(projectedKeep / max(farParams.w, 1.0e-3f)) : projectedKeep;
		float keep = min(inRamp, outRamp) * performanceKeep;
		float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;

		passes = dither <= keep;
	}
#endif
	return passes;
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

float CalculateWindAdjustedAngle(float clumpedAngle, float2 direction, float angle, float speed, float rotationalStiffness, float scaledWidth, float bladeHeight)
{
	if (angle < 0.0f)
		angle += Math::TAU;

	float diff = angle - clumpedAngle;
	if (diff > Math::PI)
		diff -= Math::TAU;
	else if (diff < -Math::PI)
		diff += Math::TAU;

	float2 clumpedFacing = float2(cos(clumpedAngle), sin(clumpedAngle));
	float alignment = dot(direction, clumpedFacing) * 0.5f + 0.5f;
	float rotationFactor = lerp(0.2f, 1.0f, alignment * 0.5f);
	float totalRotation = rotationFactor * speed * speed * speed * 0.5f * scaledWidth * bladeHeight;
	float reducedRotation = totalRotation * rcp(rotationalStiffness * totalRotation + 1.0f);
	float clampedRotation = min(reducedRotation, abs(diff)) * sign(diff);
	return clumpedAngle + clampedRotation;
}

// Finish one base or slope-fill candidate after establishing its terrain plane.
bool BuildBlade(uint3 initialHash, float2 mapSamplePos, float2 initialWorldPos2D, float bladeWorldZ, float2 terrainSlope, float3 terrainNormal,
	float2 quadWorldPos, uint quadrant, bool hasLand, uint packedGrassCell, bool cullsDisabled, bool insideFrustum, float preCulledDist, out Blade b, out bool outerGeometry)
{
	b = (Blade)0;
	outerGeometry = false;

	uint3 hash = initialHash;
	float2 bladeWorldPos2D = initialWorldPos2D;
	float typeRandom = float(hash.z) * UINT_TO_FLOAT;
	float3 worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	float3 viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;

#if !defined(FAR_LOD)
	float lodDistance = preCulledDist;
#endif

#if !defined(LOW_LOD) && !defined(FAR_LOD)
	// Tier dithering controls density, so distance culling begins at the fade endpoint.
	float cullDistance = lodFadeOut.x + rcp(max(lodFadeOut.y, 1.0e-6f));
	if (lodDistance < 0.0f) {
		lodDistance = length(bladeWorldPos2D - grassLodOrigin);
		if (lodDistance >= cullDistance && !cullsDisabled)
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
#if !defined(LOW_LOD) || defined(FAR_LOD)
	if (!cullsDisabled && (terrainNormal.z < generatorType.maxSlope || terrainNormal.z > generatorType.minSlope))
		return false;
#endif

	// Delay the nine-cell clump search until after the inexpensive rejection tests.
	uint clumpRand;
	float clumpDist;
	float2 clumpDir;
	ComputeClump(clumpRand, clumpDist, clumpDir, bladeWorldPos2D, voronoiGridSize, inverseVoronoiGridSize);
	float clumpDistance01 = clumpDist * inverseVoronoiGridSize;
	float clumpDensity = 1.0f - smoothstep(0.15f, 0.50f, clumpDistance01);

	hash = Random::pcg3d(hash);
	float clumpDistRand = float(hash.x) * UINT_TO_FLOAT;
	float heightRand = float(hash.y) * UINT_TO_FLOAT;
	float angleRand = float(hash.z) * UINT_TO_FLOAT;

#if !defined(FAR_LOD)
	// Pull every near blade toward its Voronoi feature to hide the regular candidate lattice.
	float clumpPull = lerp(0.025f, 0.225f, clumpDistRand);
	float2 clumpDisplace = clumpDir * clumpDist * clumpPull * generatorType.clumpDistanceFactor * clumpDensity;
	bladeWorldPos2D += clumpDisplace;
#if defined(LOW_LOD)
	// The larger Low LOD displacement can cross enough terrain for the original tangent plane to become inaccurate.
	bladeWorldZ = TerrainHeightSlopeAt(terrainSlope, bladeWorldPos2D, quadWorldPos, quadrant, hasLand);
	terrainNormal = normalize(float3(-terrainSlope.x, -terrainSlope.y, 1.0f));
	if (!cullsDisabled && (terrainNormal.z < generatorType.maxSlope || terrainNormal.z > generatorType.minSlope))
		return false;
#else
	bladeWorldZ += dot(terrainSlope, clumpDisplace);
#endif
	worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;
#endif

	if (!insideFrustum) {
		// A root outside the frustum can still produce visible blade geometry near the edge.
		float widthExtent = generatorType.width * 2.5f * 1.3f;
#if defined(FAR_LOD)
		widthExtent *= 32.0f * 1.6f;
#elif defined(LOW_LOD)
		widthExtent *= 2.0f * (1.0f + miscParams.z);
#elif defined(MID_LOD)
		widthExtent *= 1.41421356f * (1.0f + miscParams.z);
#else
		widthExtent *= 1.0f + miscParams.z;
#endif
		float geometryExtent = generatorType.height + widthExtent;
		float4 clip = mul(FrameBuffer::CameraViewProjUnjittered, float4(viewPos, 1.0f));
		float padX = cameraViewRow0Sum * geometryExtent;
		float padY = cameraViewRow1Sum * geometryExtent;
		bool outsideFrustum = clip.x < -(clip.w + padX) || clip.x > clip.w + padX || clip.y < -(clip.w + padY) || clip.y > clip.w + padY;
		if (outsideFrustum && !cullsDisabled)
			return false;
	}

#if !defined(FAR_LOD)
	float2 lodOffset = abs(bladeWorldPos2D - grassLodOrigin);
	lodDistance = length(lodOffset);
#if defined(LOW_LOD)
	float lodFadeOutDistance = max(lodOffset.x, lodOffset.y);
#else
	float lodFadeOutDistance = lodDistance;
#endif
	if (!cullsDisabled) {
		float inRamp = saturate((lodDistance - lodFadeIn.x) * lodFadeIn.y);
		float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodFadeOutDistance - lodFadeOut.x) * lodFadeOut.y));
		float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;
#if defined(MID_LOD) || defined(LOW_LOD)
		if ((inRamp < 1.0f && dither <= 1.0f - inRamp) || dither > outRamp)
#elif defined(HIGH_LOD)
		if (dither > min(inRamp, outRamp))
#endif
			return false;
	}
#endif

	float objectClearance = 1.0e30f;
	if (!cullsDisabled) {
		objectClearance = GetObjectClearance(worldPos);
		// Preserve grass beneath overhangs when there is still vertical room for part of the blade.
		if (objectClearance <= occlusionParams.w)
			return false;
	}

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
	float2 downhill = terrainNormal.xy;
	float slopeSteepness = length(downhill);

	if (miscParams.y > 0.0f && slopeSteepness > 1e-4f) {
		float downhillAngle = atan2(downhill.y, downhill.x);

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

	// Turn toward the wind without rotating beyond it.
	float windAdjustedAngle = CalculateWindAdjustedAngle(clumpedAngle, windDir, windAngle, windSpeed, generatorType.rotationalStiffness, scaledWidth, randHeight);
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

	b.posXY = f32tof16(viewPos.x) << 16 | f32tof16(viewPos.y);
	b.posZWidthHeight = f32tof16(viewPos.z) << 16 | packedWidth << 8 | (uint)(unscaledHeight * 255.0f);

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

	// Evaluate broad colour variation once per emitted blade instead of once per vertex.
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
	uint packedBladeColor = packedColor.x | packedColor.y << 4u | packedColor.z << 8u;
	uint packedBladeData = packedBladeColor | packedRandBend << 12u;

	float tiltSin, tiltCos;
	sincos(randTilt, tiltSin, tiltCos);

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
	b.tipDir = f32tof16(tiltSin) << 16 | f32tof16(tiltCos);
#endif
	b.hashClumpAndGrassType = hashClumpAndGrassType;

	// Store current facing as SNORM8 and animated tip displacement as f16.
	int2 packedFacing = (int2)round(clamp(randFacing, -1.0f, 1.0f) * 127.0f);
	b.facingAndWind = (uint)(packedFacing.x & 0xFF) | (uint)(packedFacing.y & 0xFF) << 8 | f32tof16(windDisplacement) << 16;
	b.previousWind = packedBladeData << 16 | f32tof16(previousWindDisplacement);

#if !defined(FAR_LOD)
#	if defined(SKYLIGHTING)
	float3 probeCell = round(FrameBuffer::CameraPosAdjust.xyz / Skylighting::CELL_SIZE);
	float3 probeOffset = probeCell * Skylighting::CELL_SIZE - FrameBuffer::CameraPosAdjust.xyz;
	uint3 probeArrayOrigin = (uint3)((int3)probeCell - (int3)(Skylighting::ARRAY_DIM / 2)) % Skylighting::ARRAY_DIM;
	float3 skylightingPosition = viewPos;
#		if defined(MID_LOD) || defined(LOW_LOD)
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
	float basePreCulledDist = -1.0f;

#if !defined(LOW_LOD) && !defined(FAR_LOD)
	if (!cullsDisabled) {
		float2 baseLodXY = baseWorldPos2D - grassLodOrigin;
		float baseDistSq = dot(baseLodXY, baseLodXY);
		float baseCullDist = lodFadeOut.x + rcp(max(lodFadeOut.y, 1.0e-6f));
		if (baseDistSq >= baseCullDist * baseCullDist)
			useBasePath = false;
		else
			basePreCulledDist = sqrt(baseDistSq);
	}
#endif

	if (useBasePath) {
		baseGrassCell = LoadGrassCell(baseMapSamplePos, quadrant);
		if (!cullsDisabled && baseGrassCell == 0u)
			useBasePath = false;
	}

	bool hasValidCandidate = useBasePath;

#if SLOPE_EXTRA_BLADES > 0 && !defined(FAR_LOD)
	// High and Mid blade slot zero owns the slope-fill candidate, which may outlive its base candidate.
	hasValidCandidate = hasValidCandidate || bladeIndex == 0u;
	if (!useBasePath && bladeIndex == 0u && !cullsDisabled) {
		bool anyExtraGrass = false;
		[unroll] for (uint extraIndex = 0; extraIndex < SLOPE_EXTRA_BLADES; ++extraIndex) {
			if ((extraIndex % PATCH_BLADE_COUNT) != bladeIndex)
				continue;

			uint oldBladeIndex = SLOPE_EXTRA_SEED_BASE + extraIndex;
			uint3 extraHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float2 extraQuadPos = (float2(patchPos * 2u) + float2(extraHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
			anyExtraGrass = anyExtraGrass || LoadGrassCell(GrassMapSamplePos(extraQuadPos, extraHash), quadrant) != 0u;
		}
		if (!anyExtraGrass)
			return;
	}
#endif

#if SLOPE_EXTRA_BLADES > 0 && defined(FAR_LOD)
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

	// One bilinear terrain sample establishes the plane for this path and its extras.
	float2 terrainSlope;
	float baseWorldZ = TerrainHeightSlopeAt(terrainSlope, baseWorldPos2D, quadrantData.quadWorldPos, quadrant, hasLand);
	if (IsPatchOccluded(baseWorldPos2D, baseWorldZ, cullsDisabled))
		return;
	float3 terrainNormal = normalize(float3(-terrainSlope.x, -terrainSlope.y, 1.0f));

#if SLOPE_EXTRA_BLADES > 0
	// Reject slope extras before grass typing, clumping, LOD, occlusion, wind, and packing.
	float baseSlopeKeep = saturate(1.0f / max(terrainNormal.z, 0.05f) - 1.0f);
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
		float candidatePreCulledDist = basePreCulledDist;

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
			candidatePreCulledDist = -1.0f;

			// The reserved candidates cover both the Low/Far seam and steep terrain, but must respect Far's density floor.
			float seamKeep = lodFadeIn.z;
			float slopeKeep = baseSlopeKeep;
			float extraKeep = max(saturate(seamKeep + slopeKeep), farParams.w);
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
#endif
			candidateWorldZ = baseWorldZ + dot(terrainSlope, candidateWorldPos - baseWorldPos2D);
		}

		if (!candidateValid)
			continue;

		if (!cullsDisabled && packedGrassCell == 0u)
			continue;

		Blade blade;
		bool outerGeometry;
		if (BuildBlade(candidateHash, candidateMapSamplePos, candidateWorldPos, candidateWorldZ,
			terrainSlope, terrainNormal, quadrantData.quadWorldPos, quadrant, hasLand, packedGrassCell, cullsDisabled, insideFrustum, candidatePreCulledDist, blade, outerGeometry)) {
			GroupBlades[groupIndex * MAX_BLADES_PER_THREAD + emittedBladeCount] = blade;
			GroupBladeOuter[groupIndex * MAX_BLADES_PER_THREAD + emittedBladeCount] = outerGeometry ? 1u : 0u;

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
		if (BuildBlade(baseHash, baseMapSamplePos, baseWorldPos2D, baseWorldZ, terrainSlope, terrainNormal, quadrantData.quadWorldPos, quadrant, hasLand, baseGrassCell, cullsDisabled, insideFrustum, basePreCulledDist, blade, outerGeometry)) {
			GroupBlades[groupIndex * MAX_BLADES_PER_THREAD] = blade;
			GroupBladeOuter[groupIndex * MAX_BLADES_PER_THREAD] = outerGeometry ? 1u : 0u;

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

	if (groupIndex == 0u) {
		GroupInnerCount = 0u;
		GroupOuterCount = 0u;
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

	uint2 categoryOffset = 0u;
	uint emittedBladeCount = emittedBladeCounts.x + emittedBladeCounts.y;
	[loop] for (uint i = 0u; i < emittedBladeCount; ++i) {
		uint outer = GroupBladeOuter[groupIndex * MAX_BLADES_PER_THREAD + i];
		uint outputIndex;
		if (outer != 0u)
			outputIndex = GroupOutputBase.y + threadOutputOffset.y + categoryOffset.y++;
		else
			outputIndex = GroupOutputBase.x + threadOutputOffset.x + categoryOffset.x++;
		BladeOutput[outputIndex] = GroupBlades[groupIndex * MAX_BLADES_PER_THREAD + i];
	}
}
