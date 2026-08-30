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

// Fallbacks for Intellisense and undefined compile paths.
#if !defined(PATCH_BLADE_COUNT)
#	define PATCH_BLADE_COUNT 1
#endif
#if !defined(SLOPE_EXTRA_BLADES)
#	define SLOPE_EXTRA_BLADES 0
#endif

// Matches the cbuffer array to the renderer's quadrant capacity.
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

static const int TG_SIZE = TG_DIM_X * TG_DIM_Y;
static const uint PATCHES_PER_QUADRANT = BLADES_PER_ROW * BLADES_PER_ROW / 4;
static const float BLADES_PER_ROW_INV = 1.0f / BLADES_PER_ROW;
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
	float4 lodFadeIn;   // x: fade-in start dist (world), y: 1/range; per-tier LOD cross-fade
	float4 lodFadeOut;  // x: fade-out start dist (world), y: 1/range, z: min keep at the far edge
	QuadrantData data[QUADRANT_DATA_SIZE];
}

AppendStructuredBuffer<Blade> BladeAppendBuffer : register(u0);

Texture2D<float> gHeightTex : register(t0);

// One 17x17 grass-type grid per quadrant. 0 is bare and 1..N selects the grass type.
StructuredBuffer<uint> QuadrantGrass : register(t1);

static const uint QUADRANT_GRASS_PITCH = 17;
static const float QUADRANT_GRASS_SPACING = 2048.0f / 16.0f;

SamplerState gLinearSampler : register(s0);

// One 17x17 LAND height grid per quadrant. Loaded cells use these exact heights instead of the quantized heightmap.
StructuredBuffer<float> QuadrantHeights : register(t3);

// Compact blade-task list. The low 12 bits select QuadrantData and the remaining bits store the blade slot and culling flags.
StructuredBuffer<uint> VisibleBladeTasks : register(t5);
StructuredBuffer<uint> QuadrantGrassCells : register(t6);  // one packed 2x2 LAND-id cell per 16x16 quadrant cell

static const uint WORK_QUADRANT_MASK = 0xFFFu;
static const uint WORK_LANE_SHIFT = 12u;
static const uint WORK_HAS_LAND = 1u << 16u;
static const uint WORK_INSIDE_FRUSTUM = 1u << 17u;
static const uint WORK_ALLOW_SLOPE_EXTRAS = 1u << 18u;

static const float QUADRANT_NO_HEIGHT = -3.0e38f;

/** @brief Returns bilinear LAND height and slope from the same four corner loads. */
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

/** @brief Returns terrain height and slope. LAND uses cached heights and the fallback samples the heightmap. */
float TerrainHeightSlopeAt(out float2 slope, float2 world2D, float2 quadWorldPos, uint quadrant, bool hasLand)
{
	float h;
	if (SampleLandHeightSlope(h, slope, world2D - quadWorldPos, quadrant, hasLand))
		return h;

	h = lerp(heightMapZRange.x, heightMapZRange.y, gHeightTex.SampleLevel(gLinearSampler, world2D * heightMapScale + heightMapOffset, 0));
	float eps = QUADRANT_GRASS_SPACING;
	float hR = lerp(heightMapZRange.x, heightMapZRange.y, gHeightTex.SampleLevel(gLinearSampler, (world2D + float2(eps, 0.0f)) * heightMapScale + heightMapOffset, 0));
	float hU = lerp(heightMapZRange.x, heightMapZRange.y, gHeightTex.SampleLevel(gLinearSampler, (world2D + float2(0.0f, eps)) * heightMapScale + heightMapOffset, 0));
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

	// TopDownOcclusion pre-pads these maps. One centre gather still covers the blade's 2x2 texel footprint.
	float2 suv = saturate(uv);
    float4 hi = OcclusionMaskHigh.Gather(gLinearSampler, suv);
	float highest = max(max(hi.x, hi.y), max(hi.z, hi.w));  // empty texels hold -1e30 in the MAX map

	if (highest <= worldPos.z + occlusionParams.w)
		return false;

	float4 lo = OcclusionMaskLow.Gather(gLinearSampler, suv);
	float lowest = min(min(lo.x, lo.y), min(lo.z, lo.w));   // empty texels hold +1e30 in the MIN map
	return lowest < worldPos.z + occlusionParams.z;
}

void ComputeClump(out uint clumpRand, out float clumpDist, out float2 clumpDir, float2 worldPos, float gridSize, float inverseGridSize)
{
	float2 gridPos = worldPos * inverseGridSize;
	int2 gridCell = int2(gridPos);

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

	float invLen = rsqrt(clumpDist);
	clumpDist = clumpDist * invLen * gridSize;
	clumpDir *= invLen;
}

// Grass ids are packed as four bytes per uint.
uint LoadGrassId(uint sampleIndex)
{
	uint packed = QuadrantGrass[sampleIndex >> 2];
	return (packed >> ((sampleIndex & 3u) << 3u)) & 0xFFu;
}

void ComputeGrassType(out uint type, float2 quadLocalPos, uint quadrant, float typeRandom)
{
	float2 grassSample = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);

#if defined(FAR_LOD)
	// Far uses one nearest grass-type sample because type boundaries are sub-pixel at this distance.
	int2 nearest = int2(grassSample + 0.5f);
	uint id = LoadGrassId(quadrant * (QUADRANT_GRASS_PITCH * QUADRANT_GRASS_PITCH) + nearest.y * QUADRANT_GRASS_PITCH + nearest.x);
	type = id;
	return;
#else
	int2 baseSample = int2(grassSample);
	float2 frac = grassSample - float2(baseSample);

	uint packed = QuadrantGrassCells[quadrant * 256u + baseSample.y * 16u + baseSample.x];
	uint4 ids = uint4(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu, packed >> 24u);

	float4 weights;
	weights.x = (1 - frac.x) * (1 - frac.y);
	weights.y = frac.x * (1 - frac.y);
	weights.z = (1 - frac.x) * frac.y;
	weights.w = frac.x * frac.y;

	float weightSum = 0;
	type = ids[3];

	[unroll] for (int j = 0; j < 4; j++)
	{
		weightSum += weights[j];
		if (typeRandom < weightSum) {
			type = ids[j];
			break;
		}
	}
#endif
}

/** Far has no clump displacement, so its final XY LOD decision is known before any terrain or grass-map access. */
bool PassesEarlyFarLOD(float2 bladeWorldPos2D, bool cullsDisabled, out float lodDist)
{
	lodDist = -1.0f;
#if defined(FAR_LOD)
	if (!cullsDisabled) {
		lodDist = length(bladeWorldPos2D - FrameBuffer::CameraPosAdjust.xy);
		float inRamp = saturate((lodDist - lodFadeIn.x) * lodFadeIn.y);
		float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDist - lodFadeOut.x) * lodFadeOut.y));
		float keep = min(inRamp, outRamp);
		float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;

		return dither <= keep;
	}
#endif
	return true;
}

/** @brief Returns smooth, deterministic world-space variation from four inexpensive integer hashes. */
float CalculateWindNoise(float2 worldPosition)
{
	static const float WindNoiseCellSize = 512.0f;
	float2 cellPosition = worldPosition * (1.0f / WindNoiseCellSize);
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

/** Finish one base or slope-fill candidate after its terrain plane has been established. */
void EmitBlade(
	uint3 initialHash,
	float2 bladeQuadPos2D,
	float2 initialWorldPos2D,
	float bladeWorldZ,
	float2 terrainSlope,
	float3 terrainNormal,
	uint distIndex,
	uint quadrant,
	bool cullsDisabled,
	bool insideFrustum,
	float earlyFarLodDist,
	float preCulledDist)
{
	uint3 hash = initialHash;
	float2 bladeWorldPos2D = initialWorldPos2D;
	float typeRand = float(hash.z) * UINT_TO_FLOAT;
	float3 worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	float3 viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;

#if !defined(LOW_LOD)
	// Cull by XY distance before the matrix and frustum path. Main provides the base candidate's known distance.
	float dist = preCulledDist;
	static const float4 cullDists = float4(8192.0f, 4096.0f, 2048.0f, 1024.0f);
	float cullDist = 8192u >> distIndex;
	if (dist < 0.0f) {
		dist = length(viewPos.xy);
		if (dist >= cullDist && !cullsDisabled)
			return;
	}
#endif

	if (!insideFrustum) {
		static const float MAX_GRASS_HEIGHT = 150.0f;
		float4 clip = mul(FrameBuffer::CameraViewProj, float4(viewPos, 1));
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
	ComputeGrassType(type, mapSamplePos, quadrant, typeRand);
	if (type == 0 && !cullsDisabled)
		return;
	type = max(type, 1u);

	GrassGeneratorType grassType = generatorGrassType[type];
	if (!cullsDisabled && (terrainNormal.z < grassType.maxSlope || terrainNormal.z > grassType.minSlope))
		return;

	// Compute clumps after culling to avoid the nine-cell search for rejected blades.
	uint clumpRand;
	float clumpDist;
	float2 clumpDir;
#if defined(FAR_LOD)
	// Far skips invisible clump variation.
	clumpRand = 0u;
	clumpDist = 1.0f;  // -> clumpDensity 0
	clumpDir = float2(0.0f, 0.0f);  // -> zero clump displacement
#else
	ComputeClump(clumpRand, clumpDist, clumpDir, bladeWorldPos2D, voronoiGridSize, inverseVoronoiGridSize);
#endif
	float clumpDensity = saturate(1.0f - clumpDist);

	hash = Random::pcg3d(hash);
	float clumpDistRand = float(hash.x) * UINT_TO_FLOAT;
	float heightRand = float(hash.y) * UINT_TO_FLOAT;
	float angleRand = float(hash.z) * UINT_TO_FLOAT;

	// Shift toward the clump centre and keep the base on the terrain plane.
	float2 clumpDisplace = clumpDir * clumpDist * saturate(clumpDistRand - 0.5f) * grassType.clumpDistanceFactor;
	bladeWorldPos2D += clumpDisplace;
#if !defined(FAR_LOD)
	bladeWorldZ += dot(terrainSlope, clumpDisplace);
	worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;
#endif

#if !defined(FAR_LOD)
	if (!cullsDisabled) {
		float lodDist = length(viewPos.xy);
		float inRamp = saturate((lodDist - lodFadeIn.x) * lodFadeIn.y);
		float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDist - lodFadeOut.x) * lodFadeOut.y));
		float keep = min(inRamp, outRamp);
		float dither = float(Random::pcg3d(uint3(asuint(bladeWorldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;
		if (dither > keep)
			return;
	}
#endif

	if (!cullsDisabled && IsOccludedByObject(worldPos))
		return;

	// Generate height after culling. The frustum test uses the maximum blade height.
	float randClumpHeight = float(clumpRand) * UINT_TO_FLOAT;
	float unscaledHeight = (0.45f + heightRand * 0.55f) - randClumpHeight * grassType.clumpHeightFactor;
	float randHeight = grassType.height * unscaledHeight;

	// Blade width
#if defined(LOW_LOD)
	float unscaledWidth = 1.0f;
	float fadeOut = 1.0f;
#else
	float gain = saturate((dist - cullDists[3]) * rcp((cullDists[1] - cullDists[3])));

	float fadeOut = saturate((cullDist - dist) * (1.0f / 1024.0f));
	float unscaledWidth = lerp(0.4f, 1.0f, gain);
#endif

	float widthRand = frac(heightRand * 1.618f + angleRand * 0.5f);
	unscaledWidth *= lerp(0.6f, 1.0f, widthRand);
	float scaledWidth = unscaledWidth * grassType.width * 2.5f;

	hash = Random::pcg3d(hash);

	// Base facing and clump alignment
	float randAngle = angleRand * Math::TAU;

#if defined(FAR_LOD)
	float clumpedAngle = randAngle;  // no clump, so facing is just the blade's own random angle
#else
	float clumpAngle = atan2(-clumpDir.y, -clumpDir.x);
	if (clumpAngle < 0)
		clumpAngle += Math::TAU;
	float delta = clumpAngle - randAngle;

	if (delta < 0)
		delta += Math::TAU;
	else if (delta >= Math::TAU)
		delta -= Math::TAU;

	float clumpedAngle = randAngle + delta * grassType.clumpFacingFactor;

	if (clumpedAngle < 0)
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
	float windAdjustedAngle = CalculateWindAdjustedAngle(clumpedAngle, windDir, windAngle, windSpeed, grassType.rotationalStiffness, scaledWidth, randHeight);
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
	b.posZWidthHeight = f32tof16(viewPos.z) << 16 | (uint)(unscaledWidth * 255.0f) << 8 | (uint)(unscaledHeight * fadeOut * 255.0f);
	// Keep the geometry hash independent of the camera-dependent view-thickening byte.
	uint stableBladeHash = (hash.z << 12) | ((clumpRand & 15) << 8) | type;
#if defined(MID_LOD) || (defined(LOW_LOD) && !defined(FAR_LOD))
	// Pack view-thickening factors for both blade facings to avoid per-vertex evaluation.
	float2 viewDirection = normalize(-viewPos.xy);
	float viewDotNormal = saturate(dot(randFacing, viewDirection));
	float viewDotNormal2 = viewDotNormal * viewDotNormal;
	float viewThicken = (1.0f - viewDotNormal2 * viewDotNormal2) * smoothstep(0.0f, 0.2f, viewDotNormal);
	float2 rotatedFacing = float2(randFacing.x * 0.8660254f - randFacing.y * 0.5f, randFacing.x * 0.5f + randFacing.y * 0.8660254f);
	
	float rotatedViewDotNormal = saturate(dot(rotatedFacing, viewDirection));
	float rotatedViewDotNormal2 = rotatedViewDotNormal * rotatedViewDotNormal;
	float rotatedViewThicken = (1.0f - rotatedViewDotNormal2 * rotatedViewDotNormal2) * smoothstep(0.0f, 0.2f, rotatedViewDotNormal);
	uint packedViewThicken = (uint)round(saturate(viewThicken) * 15.0f) | (uint)round(saturate(rotatedViewThicken) * 15.0f) << 4;
	
	// Preserve the near blade layout while storing view thickening in the middle byte.
	uint hashClumpAndGrassType = (hash.z & 0xFFFu) << 20 | packedViewThicken << 12 | ((clumpRand & 15) << 8) | type;
#else
	uint hashClumpAndGrassType = stableBladeHash;
#endif

	// Precompute tilt
	uint2 tiltHash = Random::pcg2d(uint2(stableBladeHash, 0u));
	float randTilt = grassType.tipWeight * (float(tiltHash.x) * UINT_TO_FLOAT * 1.4f + 0.30f);

#if defined(FAR_LOD)
	// Compute Far directions once per blade and pack them as eight-bit values.
	float facingSin, facingCos;
	float tiltSin, tiltCos;
	
	sincos(windAdjustedAngle, facingSin, facingCos);
	sincos(randTilt, tiltSin, tiltCos);
	
	uint4 packedDirections = (uint4)round(saturate(float4(facingCos, facingSin, tiltSin, tiltCos) * 0.5f + 0.5f) * 255.0f);
	b.facingTilt = packedDirections.x | packedDirections.y << 8 | packedDirections.z << 16 | packedDirections.w << 24;
	
	// Store Far's widening ramp to avoid per-vertex length and square-root work.
	float farWidthDistance = earlyFarLodDist >= 0.0f ? earlyFarLodDist : length(viewPos.xy);
	uint farWidthByte = (uint)round(saturate((farWidthDistance - farParams.x) * farParams.y) * 255.0f);
	b.seedAndType = farWidthByte << 24 | (hash.z & 0xFFFFu) << 8 | (type & 0xFFu);
#else
	float randBend = grassType.stiffness * (float(tiltHash.y) * UINT_TO_FLOAT * 1.6f + 0.25f);
	uint packedDetailRandom = (tiltHash.x & 0xFFu) | (tiltHash.y & 0xFFu) << 8;
	float tiltSin, tiltCos;
	sincos(randTilt, tiltSin, tiltCos);
	b.tipDir = f32tof16(tiltSin) << 16 | f32tof16(tiltCos);
	b.hashClumpAndGrassType = hashClumpAndGrassType;
	
	// Store current facing as SNORM8 and animated tip displacement as f16.
	int2 packedFacing = (int2)round(clamp(randFacing, -1.0f, 1.0f) * 127.0f);
	b.facingAndWind = (uint)(packedFacing.x & 0xFF) | (uint)(packedFacing.y & 0xFF) << 8 | f32tof16(windDisplacement) << 16;
	b.previousWind = packedDetailRandom << 16 | f32tof16(previousWindDisplacement);
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

	if (patch >= PATCHES_PER_QUADRANT)
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
#if defined(LOW_LOD)
	baseJitter *= 0.5f;
#endif
	
	float2 baseQuadPos2D = (float2(pos) + baseJitter) * BLADE_TO_WORLD;
	float2 baseWorldPos2D = baseQuadPos2D + quadrantData.quadWorldPos;
	float baseFarLodDist;
	bool useBasePath = PassesEarlyFarLOD(baseWorldPos2D, cullsDisabled, baseFarLodDist);
	float basePreCulledDist = -1.0f;
	
#if !defined(LOW_LOD)
	if (!cullsDisabled) {
		float2 baseViewXY = baseWorldPos2D - FrameBuffer::CameraPosAdjust.xy;
		float baseDistSq = dot(baseViewXY, baseViewXY);
		float baseCullDist = 8192u >> bladeIndex;
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
	float extraFarLodDists[SLOPE_EXTRA_BLADES];
	bool extraValid[SLOPE_EXTRA_BLADES];

	if (allowSlopeExtras) {
		[unroll] for (uint extraIndex = 0; extraIndex < SLOPE_EXTRA_BLADES; ++extraIndex) {
			// Assign each extra slot to one base blade slot.
			bool owned = (extraIndex % PATCH_BLADE_COUNT) == bladeIndex;
			uint oldBladeIndex = PATCH_BLADE_COUNT + extraIndex;
			uint3 extraHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float2 extraQuadPos = (float2(patchPos * 2u) + float2(extraHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
	
			float extraFarLodDist;
	
			bool valid = owned && PassesEarlyFarLOD(extraQuadPos + quadrantData.quadWorldPos, cullsDisabled, extraFarLodDist);
			extraHashes[extraIndex] = extraHash;
			extraQuadPositions[extraIndex] = extraQuadPos;
			extraFarLodDists[extraIndex] = extraFarLodDist;
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
	float3 terrainNormal = normalize(float3(-terrainSlope.x, -terrainSlope.y, 1.0f));

#if SLOPE_EXTRA_BLADES > 0
	// Reject slope extras before grass typing, clumping, LOD, occlusion, wind, and packing.
	float baseSlopeKeep = saturate(1.0f / max(terrainNormal.z, 0.05f) - 1.0f);
	// Keep one emit path. Unrolling multiplies Low's DXBC sixfold.
	[loop] for (uint candidateIndex = 0; candidateIndex < 1 + SLOPE_EXTRA_BLADES; ++candidateIndex) {
		bool isBase = candidateIndex == 0;
		uint3 candidateHash = baseHash;
		float2 candidateQuadPos = baseQuadPos2D;
		float2 candidateWorldPos = baseWorldPos2D;
		float candidateWorldZ = baseWorldZ;
		uint candidateDistIndex = bladeIndex;
		bool candidateValid = useBasePath;
		float candidateFarLodDist = baseFarLodDist;
		float candidatePreCulledDist = basePreCulledDist;

		if (!isBase) {
			uint emitExtraIndex = candidateIndex - 1;
			uint oldBladeIndex = PATCH_BLADE_COUNT + emitExtraIndex;
#if defined(FAR_LOD)
			if (!allowSlopeExtras)
				continue;
			
			candidateValid = extraValid[emitExtraIndex];
			if (!candidateValid)
				continue;
			
			candidateHash = extraHashes[emitExtraIndex];
			candidateQuadPos = extraQuadPositions[emitExtraIndex];
			candidateWorldPos = candidateQuadPos + quadrantData.quadWorldPos;
			candidateFarLodDist = extraFarLodDists[emitExtraIndex];
			candidatePreCulledDist = -1.0f;
			
			// Continue Low's slope fill into Far with a fade.
			float slopeKeep = baseSlopeKeep * saturate((farParams.x + 4096.0f - candidateFarLodDist) * (1.0f / 4096.0f));
			float keepRand = float(Random::pcg3d(uint3(asuint(candidateWorldPos), oldBladeIndex)).x) * UINT_TO_FLOAT;
			
			if (!cullsDisabled && keepRand > slopeKeep)
				continue;
#else
			// Resolve the slope roll from the extra seed before constructing its position.
			if ((emitExtraIndex % PATCH_BLADE_COUNT) != bladeIndex)
				continue;
			
			candidateHash = Random::pcg3d(uint3(patchPos, oldBladeIndex + quadrantHash));
			float slopeRoll = float(candidateHash.z) * UINT_TO_FLOAT;
			if (!cullsDisabled && slopeRoll > baseSlopeKeep)
				continue;
			
			candidateQuadPos = (float2(patchPos * 2u) + float2(candidateHash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
			candidateWorldPos = candidateQuadPos + quadrantData.quadWorldPos;
#endif
			candidateWorldZ = baseWorldZ + dot(terrainSlope, candidateWorldPos - baseWorldPos2D);
			candidateDistIndex = 0u;
		}

		if (!candidateValid)
			continue;

		EmitBlade(candidateHash, candidateQuadPos, candidateWorldPos, candidateWorldZ,
			terrainSlope, terrainNormal, candidateDistIndex, quadrant, cullsDisabled, insideFrustum, candidateFarLodDist, candidatePreCulledDist);
	}
#else
	if (useBasePath)
		EmitBlade(baseHash, baseQuadPos2D, baseWorldPos2D, baseWorldZ, terrainSlope, terrainNormal, bladeIndex, quadrant, cullsDisabled, insideFrustum, baseFarLodDist, basePreCulledDist);
#endif
}
