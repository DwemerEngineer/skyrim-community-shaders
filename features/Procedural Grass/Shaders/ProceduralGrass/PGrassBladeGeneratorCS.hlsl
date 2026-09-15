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
#	include "GrassCollision/GrassCollision.hlsli"
#endif

#if defined(HIGH_LOD) && defined(SKYLIGHTING)
#	define SKYLIGHTING_PROBE_REGISTER t50
#	include "Skylighting/Skylighting.hlsli"
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
	float4 lodFadeOut;  // x: fade-out start, y: inverse range, z: minimum retention
	QuadrantData data[QUADRANT_DATA_SIZE];
}

AppendStructuredBuffer<Blade> BladeAppendBuffer : register(u0);

Texture2D<float> TerrainHeightTexture : register(t0);

static const uint QUADRANT_GRASS_PITCH = 17;
static const float QUADRANT_GRASS_SPACING = 2048.0f / 16.0f;

SamplerState LinearSampler : register(s0);

// Loaded quadrants use exact 17x17 LAND heights instead of the quantized heightmap.
StructuredBuffer<float> QuadrantHeights : register(t3);

// The low 12 bits select QuadrantData. The remaining bits store the blade slot and flags.
StructuredBuffer<uint> VisibleBladeTasks : register(t5);
StructuredBuffer<uint> QuadrantGrassCells : register(t6);  // Packed 2x2 LAND IDs for each quadrant cell.
Texture2D<float> GrassHiZ : register(t8);                   // Shared current-frame scene-depth pyramid.

static const uint WORK_QUADRANT_MASK = 0xFFFu;
static const uint WORK_LANE_SHIFT = 12u;
static const uint WORK_HAS_LAND = 1u << 16u;
static const uint WORK_INSIDE_FRUSTUM = 1u << 17u;
static const uint WORK_ALLOW_SLOPE_EXTRAS = 1u << 18u;
static const uint WORK_NEAR_COVERED = 1u << 19u;
static const uint WORK_COMPACT_FAR = 1u << 20u;

// Start slope-fill seeds after High's four base slots to keep their positions consistent across tiers.
static const uint SLOPE_EXTRA_SEED_BASE = 4u;

// Conservatively reject Far patches against the shared max-depth pyramid.
bool IsFarPatchOccluded(float2 worldXY, float terrainZ, bool cullsDisabled)
{
#if defined(FAR_LOD)
	if (cullsDisabled || grassHiZParams.w < 1.0f)
		return false;

	const float bladeHeight = max(grassAOParams.w, 64.0f);
	const float radius = max(bladeHeight * 0.65f, 96.0f);
	const float3 centre = float3(worldXY, terrainZ + bladeHeight * 0.5f) - FrameBuffer::CameraPosAdjust.xyz;
	const float distanceToCentre = max(length(centre), 1.0e-4f);
	// Loaded tiers cover this range, where large projected bounds also make Hi-Z ineffective.
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

bool IsOccludedByObject(float3 worldPos)
{
	float2 uv = (worldPos.xy - occlusionParams.xy) * occlusionInvExtent + 0.5f;

	if (saturate(uv.x) != uv.x || saturate(uv.y) != uv.y)
		return false;

	// TopDownOcclusion includes world-space padding, so a centre sample needs no additional footprint.
	uint width, height;
	OcclusionMaskHigh.GetDimensions(width, height);
	uint2 texel = min(uint2(saturate(uv) * float2(width, height)), uint2(width - 1, height - 1));
	float highest = OcclusionMaskHigh.Load(int3(texel, 0));  // Empty texels hold -1e30 in the maximum map.

	if (highest <= worldPos.z + occlusionParams.w)
		return false;

	float lowest = OcclusionMaskLow.Load(int3(texel, 0));  // Empty texels hold +1e30 in the minimum map.
	return lowest < worldPos.z + occlusionParams.z;
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

void ComputeGrassType(out uint type, float2 quadLocalPos, uint quadrant, float typeRandom)
{
	float2 grassSample = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);

	int2 baseSample = int2(grassSample);
	float2 sampleFraction = grassSample - float2(baseSample);

	uint packed = QuadrantGrassCells[quadrant * 256u + baseSample.y * 16u + baseSample.x];
	uint4 ids = uint4(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu, packed >> 24u);

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
bool PassesEarlyFarLOD(float2 bladeWorldPos2D, bool nearCovered, bool compactFar, bool cullsDisabled, out float lodDistance)
{
	lodDistance = -1.0f;
	bool passes = true;
#if defined(FAR_LOD)
	if (!cullsDisabled) {
		lodDistance = length(bladeWorldPos2D - grassLodOrigin);
		// Cross-fade only where a loaded tier supplies complementary blades. Retain sparse Far coverage
		// through Mid's outer range so candidate rejection cannot expose a gap.
		float inRamp = 1.0f;
		if (nearCovered) {
			float handoffRamp = saturate((lodDistance - lodFadeIn.x) * lodFadeIn.y);
			float fallbackRamp = saturate((lodDistance - (lodFadeIn.x - 2.0f * rcp(lodFadeIn.y))) * (lodFadeIn.y * 0.5f));
			float fallbackKeep = min(lodFadeIn.z * 0.25f, 0.25f) * fallbackRamp;
			inRamp = max(handoffRamp, fallbackKeep);
		}
		float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
		// Thin only after the Low/Far handoff. Widening in the VS loosely preserves coverage.
		float performanceKeep = compactFar ? 1.0f : lerp(1.0f, farParams.w, saturate((lodDistance - farParams.x) * farParams.y));
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
void EmitBlade(
	uint3 initialHash,
	float2 bladeQuadPos2D,
	float2 initialWorldPos2D,
	float bladeWorldZ,
	float2 terrainSlope,
	float3 terrainNormal,
	uint quadrant,
	bool cullsDisabled,
	bool insideFrustum,
	float preCulledDist)
{
	uint3 hash = initialHash;
	float2 bladeWorldPos2D = initialWorldPos2D;
	float typeRandom = float(hash.z) * UINT_TO_FLOAT;
	float3 worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	float3 viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;

#if !defined(LOW_LOD) && !defined(FAR_LOD)
	// Tier dithering controls density, so distance culling begins at the fade endpoint.
	float lodDistance = preCulledDist;
	float cullDistance = lodFadeOut.x + rcp(max(lodFadeOut.y, 1.0e-6f));
	if (lodDistance < 0.0f) {
		lodDistance = length(bladeWorldPos2D - grassLodOrigin);
		if (lodDistance >= cullDistance && !cullsDisabled)
			return;
	}
#endif

	if (!insideFrustum) {
		static const float MAX_GRASS_HEIGHT = 150.0f;
		float4 clip = mul(FrameBuffer::CameraViewProjUnjittered, float4(viewPos, 1));
		float extraHeight = MAX_GRASS_HEIGHT * 2.0f;
		float padX = cameraViewRow0Sum * extraHeight;
		float padY = cameraViewRow1Sum * extraHeight;
		bool outsideFrustum = clip.x < -(clip.w + padX) || clip.x > clip.w + padX || clip.y < -(clip.w + padY) || clip.y > clip.w + padY;
		if (outsideFrustum && !cullsDisabled)
			return;
	}

	// Fetch the grass type after culling to avoid the four-sample lookup for rejected blades.
	uint type;

	float2 mapSamplePos = bladeQuadPos2D + (float2(hash.xy) * UINT_TO_FLOAT * 2.0f - 1.0f) * miscParams.x;
	ComputeGrassType(type, mapSamplePos, quadrant, typeRandom);
	if (type == 0u && !cullsDisabled)
		return;
	type = max(type, 1u);

	GrassGeneratorType generatorType = generatorGrassType[type];
	if (!cullsDisabled && (terrainNormal.z < generatorType.maxSlope || terrainNormal.z > generatorType.minSlope))
		return;

	// Delay the nine-cell clump search until after the inexpensive rejection tests.
	uint clumpRand;
	float clumpDist;
	float2 clumpDir;
	ComputeClump(clumpRand, clumpDist, clumpDir, bladeWorldPos2D, voronoiGridSize, inverseVoronoiGridSize);
	float clumpDensity = saturate(1.0f - clumpDist);

	hash = Random::pcg3d(hash);
	float clumpDistRand = float(hash.x) * UINT_TO_FLOAT;
	float heightRand = float(hash.y) * UINT_TO_FLOAT;
	float angleRand = float(hash.z) * UINT_TO_FLOAT;

#if !defined(FAR_LOD)
	// Pull every near blade toward its Voronoi feature to hide the regular candidate lattice.
	float clumpPull = lerp(0.025f, 0.225f, clumpDistRand);
	float2 clumpDisplace = clumpDir * clumpDist * clumpPull * generatorType.clumpDistanceFactor;
	bladeWorldPos2D += clumpDisplace;
	bladeWorldZ += dot(terrainSlope, clumpDisplace);
	worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;
#endif

#if !defined(FAR_LOD)
	if (!cullsDisabled) {
		float lodDistance = length(bladeWorldPos2D - grassLodOrigin);
		float inRamp = saturate((lodDistance - lodFadeIn.x) * lodFadeIn.y);
		float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
		float keep = min(inRamp, outRamp);
		float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;
#if defined(MID_LOD)
		// High keeps the lower random values. Mid takes the rest during their shared transition.
		if ((inRamp < 1.0f && dither <= 1.0f - inRamp) || dither > outRamp)
#elif defined(HIGH_LOD)
		if (keep <= 0.0f || dither > keep)
#else
		// Mid keeps the lower values as it fades out. Low takes the adjacent range without overlap.
		float lowRangeStart = 1.0f - inRamp;
		if (keep <= 0.0f || dither <= lowRangeStart || dither > lowRangeStart + keep)
#endif
			return;
	}
#endif

	if (!cullsDisabled && IsOccludedByObject(worldPos))
		return;

	// Generate height after culling. The frustum test uses the maximum blade height.
	float clumpHeightRandom = float(clumpRand) * UINT_TO_FLOAT;
	float unscaledHeight = (0.45f + heightRand * 0.55f) - clumpHeightRandom * generatorType.clumpHeightFactor;
	float randHeight = generatorType.height * unscaledHeight;

	// Store only camera-independent width variation. The VS applies continuous LOD widening.
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

	float clumpedAngle = randAngle + delta * generatorType.clumpFacingFactor;

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
	float previousWindDisplacement = CalculateWindDisplacement(bladeWorldPos2D, SharedData::Timer - miscParams.w, previousWindSpeed, randHeight, windNoise, bladeWindPhase, bladeWindStrength);
#else
	float windDisplacement = 0.0f;
	float previousWindDisplacement = 0.0f;
#endif

#if !defined(FAR_LOD)
	float facingSin, facingCos;
	sincos(windAdjustedAngle, facingSin, facingCos);
	float2 randFacing = float2(facingCos, facingSin);
#endif

	Blade b;
	b.posXY = f32tof16(viewPos.x) << 16 | f32tof16(viewPos.y);
	b.posZWidthHeight = f32tof16(viewPos.z) << 16 | (uint)(unscaledWidth * 255.0f) << 8 | (uint)(unscaledHeight * 255.0f);
	// Keep the geometry hash independent of the camera-dependent view-thickening byte.
	uint stableBladeHash = (hash.z << 12) | ((clumpRand & 15u) << 8) | type;
#if defined(MID_LOD) || (defined(LOW_LOD) && !defined(FAR_LOD))
	// Pack both view-thickening factors using the stable LOD origin.
	float2 viewOffset = grassLodOrigin - bladeWorldPos2D;
	float2 viewDirection = viewOffset * rsqrt(max(dot(viewOffset, viewOffset), 1.0e-4f));
	float viewDotNormal = saturate(dot(randFacing, viewDirection));
	float viewDotNormal2 = viewDotNormal * viewDotNormal;
	float viewThicken = (1.0f - viewDotNormal2 * viewDotNormal2) * smoothstep(0.0f, 0.2f, viewDotNormal);
	float2 rotatedFacing = float2(randFacing.x * 0.8660254f - randFacing.y * 0.5f, randFacing.x * 0.5f + randFacing.y * 0.8660254f);
	
	float rotatedViewDotNormal = saturate(dot(rotatedFacing, viewDirection));
	float rotatedViewDotNormal2 = rotatedViewDotNormal * rotatedViewDotNormal;
	float rotatedViewThicken = (1.0f - rotatedViewDotNormal2 * rotatedViewDotNormal2) * smoothstep(0.0f, 0.2f, rotatedViewDotNormal);
	uint packedViewThicken = (uint)round(saturate(viewThicken) * 15.0f) | (uint)round(saturate(rotatedViewThicken) * 15.0f) << 4;
	
	// Split the 16-bit clump seed around the view-thickening byte.
	uint packedClumpSeed = (clumpRand & 0xFFF0u) << 16 | (clumpRand & 0xFu) << 8;
	uint hashClumpAndGrassType = packedClumpSeed | packedViewThicken << 12 | type;
#elif defined(HIGH_LOD)
	// High does not need a view-thickening byte, so retain a full 24-bit cell seed.
	uint hashClumpAndGrassType = (clumpRand & 0xFFFFFFu) << 8 | type;
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
	float randBend = generatorType.stiffness * (float(tiltHash.y) * UINT_TO_FLOAT * 1.6f + 0.25f);

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
	perBladeColor *= lerp(1.0f, clumpTint * clumpValue, surfaceType.clumpColorStrength);
	// Pack blade-wide colour variation. The pixel shader evaluates spatial blotch and grain detail.
	uint3 packedColor = (uint3)round(saturate(perBladeColor * 0.5f) * uint3(31u, 63u, 31u));
	uint packedBladeColor = packedColor.x | packedColor.y << 5u | packedColor.z << 11u;
	float tiltSin, tiltCos;
	sincos(randTilt, tiltSin, tiltCos);
	b.tipDir = f32tof16(tiltSin) << 16 | f32tof16(tiltCos);
	b.hashClumpAndGrassType = hashClumpAndGrassType;
	
	// Store current facing as SNORM8 and animated tip displacement as f16.
	int2 packedFacing = (int2)round(clamp(randFacing, -1.0f, 1.0f) * 127.0f);
	b.facingAndWind = (uint)(packedFacing.x & 0xFF) | (uint)(packedFacing.y & 0xFF) << 8 | f32tof16(windDisplacement) << 16;
	b.previousWind = packedBladeColor << 16 | f32tof16(previousWindDisplacement);
	b.clumpDensity = f32tof16(randBend) << 16 | f32tof16(clumpDensity);
#if defined(HIGH_LOD)
	// One root-position probe sample covers the blade. Use UNIT_SH outside the detail range.
#	if defined(SKYLIGHTING)
	static const float DETAIL_FADE_END = 3072.0f;
	sh2 skylightingSH = Skylighting::UNIT_SH;
	if (mul(FrameBuffer::CameraViewProj, float4(viewPos, 1.0f)).w < DETAIL_FADE_END)
		skylightingSH = Skylighting::Sample(viewPos, float3(0.0f, 0.0f, 1.0f));
	
	b.skylightingSH0 = f32tof16(skylightingSH.x) << 16 | f32tof16(skylightingSH.y);
	b.skylightingSH1 = f32tof16(skylightingSH.z) << 16 | f32tof16(skylightingSH.w);
#	else
	// Preserve the 32-byte High append stride when Skylighting is disabled.
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
	float3 previousCollisionDisplacement;
	GrassCollision::GetDisplacedPosition(collisionTipViewPos, viewPos, 1.0f, 2048.0f, true, 0.75f,
		collisionDisplacement, previousCollisionDisplacement);

	b.collisionData.x = f32tof16(collisionDisplacement.x) << 16 | f32tof16(collisionDisplacement.y);
	b.collisionData.y = f32tof16(collisionDisplacement.z) << 16 | f32tof16(previousCollisionDisplacement.x);
	b.collisionData.z = f32tof16(previousCollisionDisplacement.y) << 16 | f32tof16(previousCollisionDisplacement.z);
#endif
#endif

	BladeAppendBuffer.Append(b);
}

[numthreads(TG_DIM_X, TG_DIM_Y, 1)] void main(uint3 dispatch : SV_DispatchThreadID)
{
	uint patch = dispatch.x;
	uint bladeTask = VisibleBladeTasks[dispatch.z];
	uint bladeIndex = (bladeTask >> WORK_LANE_SHIFT) & 0xFu;
	uint quadrant = bladeTask & WORK_QUADRANT_MASK;
	bool hasLand = (bladeTask & WORK_HAS_LAND) != 0u;
	bool insideFrustum = (bladeTask & WORK_INSIDE_FRUSTUM) != 0u;
	bool allowSlopeExtras = (bladeTask & WORK_ALLOW_SLOPE_EXTRAS) != 0u;
	bool nearCovered = (bladeTask & WORK_NEAR_COVERED) != 0u;
	bool compactFar = (bladeTask & WORK_COMPACT_FAR) != 0u;

	uint activePatchCount = PATCHES_PER_QUADRANT;
#if defined(FAR_LOD)
	if (compactFar) {
		activePatchCount = max(1u, (uint)ceil(PATCHES_PER_QUADRANT * saturate(farParams.w)));
		// An odd permutation spreads the retained candidates over the entire quadrant.
		patch = (patch * 40501u + data[quadrant].quadrantHash) % PATCHES_PER_QUADRANT;
	}
#endif
	if (dispatch.x >= activePatchCount)
		return;

	bool cullsDisabled = debugFlags.x > 0.5f;
	QuadrantData quadrantData = data[quadrant];
	uint2 patchPos = uint2(patch % (BLADES_PER_ROW / 2), patch / (BLADES_PER_ROW / 2));
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
	float baseFarLodDistance;
	bool useBasePath = PassesEarlyFarLOD(baseWorldPos2D, nearCovered, compactFar, cullsDisabled, baseFarLodDistance);
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
	
	bool hasValidCandidate = useBasePath;
#if SLOPE_EXTRA_BLADES > 0 && !defined(FAR_LOD)
	// High and Mid blade slot zero owns the slope-fill candidate, which may outlive its base candidate.
	hasValidCandidate = hasValidCandidate || bladeIndex == 0u;
#endif

#if SLOPE_EXTRA_BLADES > 0 && defined(FAR_LOD)
	uint3 extraHashes[SLOPE_EXTRA_BLADES];
	float2 extraQuadPositions[SLOPE_EXTRA_BLADES];
	float extraFarLodDistances[SLOPE_EXTRA_BLADES];
	bool extraValid[SLOPE_EXTRA_BLADES];

	if (allowSlopeExtras) {
		[unroll] for (uint extraIndex = 0; extraIndex < SLOPE_EXTRA_BLADES; ++extraIndex) {
			// Assign each extra slot to one base blade slot.
			bool owned = (extraIndex % PATCH_BLADE_COUNT) == bladeIndex;
			uint oldBladeIndex = SLOPE_EXTRA_SEED_BASE + extraIndex;
			uint3 extraHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float2 extraQuadPos = (float2(patchPos * 2u) + float2(extraHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
	
			float extraFarLodDistance = -1.0f;

			bool valid = false;
			if (owned)
				valid = PassesEarlyFarLOD(extraQuadPos + quadrantData.quadWorldPos, nearCovered, compactFar, cullsDisabled, extraFarLodDistance);
			extraHashes[extraIndex] = extraHash;
			extraQuadPositions[extraIndex] = extraQuadPos;
			extraFarLodDistances[extraIndex] = extraFarLodDistance;
			extraValid[extraIndex] = valid;
			hasValidCandidate = hasValidCandidate || valid;
		}
	}
#endif

	if (!hasValidCandidate)
		return;

	// One bilinear terrain sample establishes the plane for this path and its extras.
	float2 terrainSlope;
	float baseWorldZ = TerrainHeightSlopeAt(terrainSlope, baseWorldPos2D, quadrantData.quadWorldPos, quadrant, hasLand);
	if (IsFarPatchOccluded(baseWorldPos2D, baseWorldZ, cullsDisabled))
		return;
	float3 terrainNormal = normalize(float3(-terrainSlope.x, -terrainSlope.y, 1.0f));

#if SLOPE_EXTRA_BLADES > 0
	// Reject slope extras before grass typing, clumping, LOD, occlusion, wind, and packing.
	float baseSlopeKeep = saturate(1.0f / max(terrainNormal.z, 0.05f) - 1.0f);
#if defined(LOW_LOD)
	// Fill distant hills more strongly without reserving more candidate slots.
	baseSlopeKeep = saturate(baseSlopeKeep * 2.0f);
#endif
	// Keep one emit path. Unrolling multiplies Low's DXBC sixfold.
	[loop] for (uint candidateIndex = 0; candidateIndex < 1 + SLOPE_EXTRA_BLADES; ++candidateIndex) {
		bool isBase = candidateIndex == 0;
		uint3 candidateHash = baseHash;
		float2 candidateWorldPos = baseWorldPos2D;
		float candidateWorldZ = baseWorldZ;
		bool candidateValid = useBasePath;
		float candidateFarLodDistance = baseFarLodDistance;
		float candidatePreCulledDist = basePreCulledDist;

		if (!isBase) {
			uint emitExtraIndex = candidateIndex - 1;
			uint oldBladeIndex = SLOPE_EXTRA_SEED_BASE + emitExtraIndex;
#if defined(FAR_LOD)
			if (!allowSlopeExtras)
				continue;
			
			candidateValid = extraValid[emitExtraIndex];
			if (!candidateValid)
				continue;
			
			candidateHash = extraHashes[emitExtraIndex];
			candidateWorldPos = extraQuadPositions[emitExtraIndex] + quadrantData.quadWorldPos;
			candidateFarLodDistance = extraFarLodDistances[emitExtraIndex];
			candidatePreCulledDist = -1.0f;
			
			// Use Far's reserved candidates to keep the Low/Far handoff density-neutral, then
			// retire that seam fill smoothly. Steep terrain can retain candidates farther out.
			float seamKeep = lodFadeIn.z * saturate((farParams.x + 4096.0f - candidateFarLodDistance) * (1.0f / 4096.0f));
			float slopeKeep = baseSlopeKeep * saturate((farParams.x + 4096.0f - candidateFarLodDistance) * (1.0f / 4096.0f));
			float extraKeep = saturate(seamKeep + slopeKeep);
			float keepRand = float(Random::pcg3d(uint3(asuint(candidateWorldPos), oldBladeIndex)).x) * UINT_TO_FLOAT;
			
			if (!cullsDisabled && keepRand > extraKeep)
				continue;
#else
			// Resolve the slope roll from the extra seed before constructing its position.
			if ((emitExtraIndex % PATCH_BLADE_COUNT) != bladeIndex)
				continue;
			
			candidateHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float slopeRoll = float(candidateHash.z) * UINT_TO_FLOAT;
			if (!cullsDisabled && slopeRoll > baseSlopeKeep)
				continue;
			
			candidateWorldPos = (float2(patchPos * 2u) + float2(candidateHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD + quadrantData.quadWorldPos;
#endif
			candidateWorldZ = baseWorldZ + dot(terrainSlope, candidateWorldPos - baseWorldPos2D);
		}

		if (!candidateValid)
			continue;

		// Recompute the local map coordinate because FXC does not reliably preserve it through this loop.
		EmitBlade(candidateHash, candidateWorldPos - quadrantData.quadWorldPos, candidateWorldPos, candidateWorldZ,
			terrainSlope, terrainNormal, quadrant, cullsDisabled, insideFrustum, candidatePreCulledDist);
	}
#else
	if (useBasePath)
		EmitBlade(baseHash, baseQuadPos2D, baseWorldPos2D, baseWorldZ, terrainSlope, terrainNormal, quadrant, cullsDisabled, insideFrustum, basePreCulledDist);
#endif
}
