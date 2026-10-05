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

#if defined(SKYLIGHTING) && defined(HIGH_LOD)
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

uint2 BaseGridPosition(uint2 patchPos, uint bladeIndex)
{
	uint patchHash = Random::iqint3(patchPos);
	uint bladeIndexRandomiser = (patchHash >> 16) & 3u;
	uint randomBladeIndex = bladeIndex ^ bladeIndexRandomiser;
	return patchPos * 2u + uint2(randomBladeIndex >> 1u, randomBladeIndex & 1u);
}

float2 BaseQuadrantPosition(uint2 pos, uint3 hash)
{
	float2 jitter = float2(hash.xy) * UINT_TO_FLOAT;
#if defined(FAR_LOD)
	jitter *= 0.5f;
#endif
	return (float2(pos) + jitter) * BLADE_TO_WORLD;
}

static const uint WORK_HAS_LAND = 1u << 16u;

struct QuadrantData
{
	float2 quadWorldPos;
	uint quadrantHash;
	uint flags;
};

cbuffer QuadrantData : register(b7)
{
	float4 lodFadeIn;  // x: fade-in start, y: inverse range, z: sparse Far extra retention, w: fade-out endpoint
	float4 lodFadeOut;
	QuadrantData data[QUADRANT_DATA_SIZE];
}

RWStructuredBuffer<Blade> BladeOutput : register(u0);
RWByteAddressBuffer IndirectArgs : register(u1);

static const uint MAX_BLADES_PER_THREAD = 1u + (SLOPE_EXTRA_BLADES + PATCH_BLADE_COUNT - 1u) / PATCH_BLADE_COUNT;
#if defined(LOW_LOD) && !defined(FAR_LOD) && SLOPE_EXTRA_BLADES > 0
#	define LOW_PATCHES_PER_GROUP THREADGROUP_SIZE

struct LowPatchSetup
{
	float2 baseWorldPos2D;
	float2 terrainSlope;
	float baseWorldZ;
	uint patch;
};
#endif

#if defined(HIGH_LOD) || defined(MID_LOD) || defined(LOW_LOD)
// Adjacent lanes occupy adjacent slots for each emission, reducing shared-memory bank conflicts.
#	if defined(LOW_LOD) && !defined(FAR_LOD) && SLOPE_EXTRA_BLADES > 0
groupshared Blade GroupBlades[THREADGROUP_SIZE * (1 + SLOPE_EXTRA_BLADES)];
groupshared LowPatchSetup LowPatchSetups[LOW_PATCHES_PER_GROUP];
groupshared uint LowActiveCount;
groupshared uint LowInnerCount;
groupshared uint LowOuterCount;
#	else
groupshared Blade GroupBlades[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
#	endif
#	if defined(HIGH_GEOMETRY_LOD)
groupshared uint GroupBladeOuter[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
groupshared uint GroupOuterCount;
#	endif
#	if !defined(LOW_LOD) || defined(FAR_LOD) || SLOPE_EXTRA_BLADES == 0
groupshared uint GroupInnerCount;
#	endif
groupshared uint2 GroupOutputBase;
#endif

Texture2D<float> TerrainHeightTexture : register(t0);
SamplerState LinearSampler : register(s0);

static const uint QUADRANT_GRASS_PITCH = 17;
static const float QUADRANT_GRASS_SPACING = 2048.0f / 16.0f;

StructuredBuffer<float> QuadrantHeights : register(t3);

// Return bilinear LAND height and slope from the same four corners.
bool SampleLandHeightSlope(out float height, out float2 slope, float2 quadLocalPos, uint quadrant, bool hasLand)
{
	height = 0.0f;
	slope = float2(0.0f, 0.0f);

	if (!hasLand)
		return false;

	float2 gridPosition = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);
	int2 baseSample = int2(gridPosition);
	float2 sampleFraction = gridPosition - baseSample;

	uint quadrantBase = quadrant * (QUADRANT_GRASS_PITCH * QUADRANT_GRASS_PITCH);
	uint lowerLeftIndex = quadrantBase + baseSample.y * QUADRANT_GRASS_PITCH + baseSample.x;
	float heightLowerLeft = QuadrantHeights[lowerLeftIndex];
	float heightLowerRight = QuadrantHeights[lowerLeftIndex + 1];
	float heightUpperLeft = QuadrantHeights[lowerLeftIndex + QUADRANT_GRASS_PITCH];
	float heightUpperRight = QuadrantHeights[lowerLeftIndex + QUADRANT_GRASS_PITCH + 1];

	height = lerp(lerp(heightLowerLeft, heightLowerRight, sampleFraction.x), lerp(heightUpperLeft, heightUpperRight, sampleFraction.x), sampleFraction.y);
	slope = float2(
				lerp(heightLowerRight - heightLowerLeft, heightUpperRight - heightUpperLeft, sampleFraction.y),
				lerp(heightUpperLeft - heightLowerLeft, heightUpperRight - heightLowerRight, sampleFraction.x)) *
	        (1.0f / QUADRANT_GRASS_SPACING);

	return true;
}

// The low 12 bits select QuadrantData. The remaining bits store the blade slot and flags.
StructuredBuffer<uint> VisibleBladeTasks : register(t5);
StructuredBuffer<uint> QuadrantGrassCells : register(t6);  // Packed 2x2 LAND IDs for each quadrant cell.
StructuredBuffer<float2> TileHeightBounds : register(t11);
StructuredBuffer<uint> OccupancyRows : register(t12);

#if defined(HIGH_LOD)
Texture2D<uint> GrassDensityTexture : register(t7);
#endif

Texture2D<float> GrassHiZ : register(t8);  // Shared current-frame scene-depth pyramid.
#if defined(LOW_LOD)
Texture2D<float> TerrainSurfaceLift : register(t9);
Texture2D<float> TerrainSurfaceHeight : register(t10);

/** @brief Returns how far generated roots can be raised around a box reaching `reach` from `world2D`. */
float GetTerrainLiftReach(float2 world2D, float reach)
{
	float2 offset = abs(world2D - grassLodOrigin);
	return TerrainLiftMax * GetTerrainLiftBlend(max(offset.x, offset.y) + reach);
}
#endif

static const uint WORK_QUADRANT_MASK = 0xFFFu;
static const uint WORK_LANE_SHIFT = 12u;
static const uint WORK_INSIDE_FRUSTUM = 1u << 17u;
static const uint WORK_ALLOW_SLOPE_EXTRAS = 1u << 18u;
static const uint WORK_NEAR_COVERED = 1u << 19u;
static const uint WORK_COMPACT_FAR = 1u << 20u;
static const uint WORK_OCCUPIED_TILE = 1u << 21u;
static const uint WORK_TILE_SHIFT = 22u;
static const uint WORK_TILE_MASK = 0xFFu;
static const uint WORK_FULL_GRASS = 1u << 30u;

static const uint PATCHES_PER_ROW = BLADES_PER_ROW / 2u;
static const uint PATCH_ROWS = (PATCHES_PER_QUADRANT + PATCHES_PER_ROW - 1u) / PATCHES_PER_ROW;
static const uint OCCUPANCY_TILES_PER_AXIS = QUADRANT_GRASS_PITCH - 1u;
static const uint MAX_TILE_PATCH_WIDTH = (PATCHES_PER_ROW + OCCUPANCY_TILES_PER_AXIS - 1u) / OCCUPANCY_TILES_PER_AXIS;

// Start slope-fill seeds after High's four base slots to keep their positions consistent across tiers.
static const uint SLOPE_EXTRA_SEED_BASE = 4u;

uint3 ExtraCandidateHash(uint2 patchPos, uint extraIndex, uint quadrantHash)
{
	return Random::pcg3d(uint3(patchPos, SLOPE_EXTRA_SEED_BASE + extraIndex + quadrantHash));
}

float2 ExtraCandidateQuadPos(uint2 patchPos, uint3 hash)
{
	return (float2(patchPos * 2u) + float2(hash.xy) * UINT_TO_FLOAT * 2.0f) * BLADE_TO_WORLD;
}

// Stable per-position dither shared by every tier's LOD fades.
float LodDither(float2 worldPos2D)
{
	return float(Random::pcg3d(uint3(asuint(worldPos2D), 0x9E3779B9u)).z) * UINT_TO_FLOAT;
}

// Maps a tile-local task index to its quadrant patch. Returns false outside the tile or past the last patch.
bool ResolveTilePatch(uint bladeTask, inout uint patch)
{
	if ((bladeTask & WORK_OCCUPIED_TILE) != 0u) {
		uint tile = (bladeTask >> WORK_TILE_SHIFT) & WORK_TILE_MASK;
		uint2 tilePos = uint2(tile % OCCUPANCY_TILES_PER_AXIS, tile / OCCUPANCY_TILES_PER_AXIS);
		uint2 patchStart = tilePos * uint2(PATCHES_PER_ROW, PATCH_ROWS) / OCCUPANCY_TILES_PER_AXIS;
		uint2 patchEnd = (tilePos + 1u) * uint2(PATCHES_PER_ROW, PATCH_ROWS) / OCCUPANCY_TILES_PER_AXIS;
		uint2 localPatch = uint2(patch % MAX_TILE_PATCH_WIDTH, patch / MAX_TILE_PATCH_WIDTH);
		if (any(localPatch >= patchEnd - patchStart))
			return false;
		patch = (patchStart.y + localPatch.y) * PATCHES_PER_ROW + patchStart.x + localPatch.x;
	}
	return patch < PATCHES_PER_QUADRANT;
}

// Rejects a root whose blade envelope lies wholly outside a side plane of the unjittered frustum.
bool IsOutsideFrustum(float3 viewPos, float geometryExtent)
{
	float4 clip = mul(FrameBuffer::CameraViewProjUnjittered, float4(viewPos, 1.0f));
	return clip.x + clip.w < -frustumPlaneExtent.x * geometryExtent ||
	       clip.w - clip.x < -frustumPlaneExtent.y * geometryExtent ||
	       clip.y + clip.w < -frustumPlaneExtent.z * geometryExtent ||
	       clip.w - clip.y < -frustumPlaneExtent.w * geometryExtent;
}
groupshared uint GroupTileOccluded;

// The Hi-Z depth bounds assume the game's forward perspective projection.
bool HasForwardPerspective()
{
	return FrameBuffer::CameraProj._m20 == 0.0f && FrameBuffer::CameraProj._m21 == 0.0f &&
	       FrameBuffer::CameraProj._m30 == 0.0f && FrameBuffer::CameraProj._m31 == 0.0f &&
	       FrameBuffer::CameraProj._m32 == 1.0f && FrameBuffer::CameraProj._m33 == 0.0f && FrameBuffer::CameraProj._m23 < 0.0f;
}

// Farthest Hi-Z depth over at most 3x3 texels of one mip.
float LoadHiZMax3x3(int2 sampleMin, int2 sampleMax, int mip)
{
	float tileMax = 0.0f;
	[unroll] for (int y = 0; y < 3; ++y)
	{
		[unroll] for (int x = 0; x < 3; ++x)
		{
			const int2 texel = sampleMin + int2(x, y);
			if (all(texel <= sampleMax))
				tileMax = max(tileMax, GrassHiZ.Load(int3(texel, mip)));
		}
	}
	return tileMax;
}

bool IsVolumeOccluded(float3 centre, float radius, float minDistance, bool cullsDisabled)
{
	if (cullsDisabled || grassHiZParams.w < 1.0f)
		return false;
	const float distanceSq = dot(centre, centre);
	if (distanceSq < minDistance * minDistance)
		return false;
	if (!HasForwardPerspective())
		return false;

	// Include half-packed root rounding and a small silhouette margin in both bounds.
	radius += length(max(abs(centre) + radius, 1.0f) * (1.0f / 1024.0f)) + 8.0f;
	const float4 clipCentre = mul(FrameBuffer::CameraViewProj, float4(centre, 1.0f));
	const float3 clipWAxis = FrameBuffer::CameraViewProj[3].xyz;
	const float minClipW = clipCentre.w - radius * length(clipWAxis);
	const float terrainDepthMargin = 128.0f;
	const float minRenderedW = minClipW - terrainDepthMargin;
	if (minRenderedW <= 1.0f)
		return false;

	const float2 hiZSize = grassHiZParams.xy;
	const float2 ndc = clipCentre.xy / clipCentre.w;
	const float2 uv = ndc * float2(0.5f, -0.5f) + 0.5f;
	const float2 screenRadius = radius * float2(length(FrameBuffer::CameraViewProj[0].xyz - ndc.x * clipWAxis), length(FrameBuffer::CameraViewProj[1].xyz - ndc.y * clipWAxis)) * (0.5f / minClipW);
	float2 uvMin = uv - screenRadius - rcp(hiZSize);
	float2 uvMax = uv + screenRadius + rcp(hiZSize);
	if (any(uvMax <= 0.0f) || any(uvMin >= 1.0f))
		return false;
	uvMin = max(uvMin, 0.0f);
	uvMax = min(uvMax, 1.0f);
	const float2 spanTexels = (uvMax - uvMin) * hiZSize;
	const float wantedMip = ceil(log2(max(max(spanTexels.x, spanTexels.y), 1.0f)));
	// A failed reduction leaves only the base texture; avoid scanning it for large bounds.
	if (wantedMip > 0.0f && grassHiZParams.w < 2.0f)
		return false;
	const int mip = min((int)wantedMip, (int)grassHiZParams.w - 1);
	const float mipScale = exp2((float)mip);
	const int2 minTexel = int2(floor(uvMin * hiZSize / mipScale));
	const int2 maxTexel = int2(floor(uvMax * hiZSize / mipScale));
	const int2 mipSize = max(int2(ceil(hiZSize / mipScale)), int2(1, 1));
	const int2 sampleMin = clamp(minTexel, int2(0, 0), mipSize - 1);
	const int2 sampleMax = clamp(maxTexel, int2(0, 0), mipSize - 1);

	float tileMax = 0.0f;
	if (wantedMip <= grassHiZParams.w - 1.0f) {
		tileMax = LoadHiZMax3x3(sampleMin, sampleMax, mip);
	} else {
		// Large bounds scan the covered coarsest mip instead of bypassing the shallow pyramid.
		[loop] for (int y = sampleMin.y; y <= sampleMax.y; ++y)
		{
			[loop] for (int x = sampleMin.x; x <= sampleMax.x; ++x)
				tileMax = max(tileMax, GrassHiZ.Load(int3(int2(x, y), mip)));
		}
	}

	// Bound the actual biased draw depth; a fixed NDC tolerance grows too large in the distance.
	const float nearestDepth = FrameBuffer::CameraProj._m22 + FrameBuffer::CameraProj._m23 / minRenderedW;
	return nearestDepth > tileMax + 2.0e-6f;
}

#if defined(FAR_LOD)
/** @brief Reconstructs the camera-relative position of one Hi-Z texel; w is zero when the texel is sky or offscreen. */
float4 GetHiZScenePosition(int2 texel)
{
	if (any(texel < 0) || any(texel >= int2(grassHiZParams.xy)))
		return 0.0f;
	float depth = GrassHiZ.Load(int3(texel, 0));
	if (depth >= 1.0f)
		return 0.0f;
	float2 ndc = (float2(texel) + 0.5f) / grassHiZParams.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, depth, 1.0f));
	if (abs(position.w) < 1.0e-8f)
		return 0.0f;
	return float4(position.xyz / position.w, 1.0f);
}

/**
 * @brief Detects a Far root covered from close by a steep or tall surface.
 * Distant LOD rocks and ledges are missing from the top-down occlusion map, so blades rooted under them would pierce
 * their tops. Shallow surfaces near the ground are terrain and leave the blade alone.
 */
bool IsRootUnderObject(float3 rootView, float2 terrainSlope)
{
	if (grassHiZParams.w < 1.0f || !HasForwardPerspective())
		return false;

	float4 rootClip = mul(FrameBuffer::CameraViewProj, float4(rootView, 1.0f));
	if (rootClip.w <= 1.0f)
		return false;
	float2 uv = rootClip.xy / rootClip.w * float2(0.5f, -0.5f) + 0.5f;
	if (any(uv < 0.0f) || any(uv >= 1.0f))
		return false;
	int2 depthTexel = int2(uv * grassHiZParams.xy);
	if (GrassHiZ.Load(int3(depthTexel, 0)) >= rootClip.z / rootClip.w)
		return false;

	float4 scenePosition = GetHiZScenePosition(depthTexel);
	if (scenePosition.w == 0.0f)
		return false;
	float2 sceneRootOffset = scenePosition.xy - rootView.xy;
	float sceneGroundHeight = scenePosition.z - (rootView.z + dot(terrainSlope, sceneRootOffset));
	if (sceneGroundHeight <= 16.0f || dot(sceneRootOffset, sceneRootOffset) >= 768.0f * 768.0f)
		return false;
	if (sceneGroundHeight > 64.0f)
		return true;

	// Rock, ledge, and trunk faces are steep; low shallow surfaces are terrain.
	float4 sceneRight = GetHiZScenePosition(depthTexel + int2(1, 0));
	float4 sceneDown = GetHiZScenePosition(depthTexel + int2(0, 1));
	if (sceneRight.w == 0.0f || sceneDown.w == 0.0f)
		return true;
	float3 sceneNormal = cross(sceneRight.xyz - scenePosition.xyz, sceneDown.xyz - scenePosition.xyz);
	return abs(sceneNormal.z) < 0.7f * length(sceneNormal);
}
#endif

#if defined(LOW_LOD)
// Rejects one finished blade whose projected root-to-tip rectangle is entirely behind Hi-Z.
// Patch and tile spheres overlap nearby silhouettes, so this catches distant blades hidden behind nearer grass and terrain.
bool IsBladeOccluded(float3 rootView, float3 tipView, float radius, bool cullsDisabled)
{
	if (cullsDisabled || grassHiZParams.w < 1.0f || !HasForwardPerspective())
		return false;

	// Leave a small margin around silhouette edges and half-packed roots.
	radius += 8.0f;
	const float4 clipRoot = mul(FrameBuffer::CameraViewProj, float4(rootView, 1.0f));
	const float4 clipTip = mul(FrameBuffer::CameraViewProj, float4(tipView, 1.0f));
	const float3 clipWAxis = FrameBuffer::CameraViewProj[3].xyz;
	const float minClipW = min(clipRoot.w, clipTip.w) - radius * length(clipWAxis);
	if (minClipW <= 1.0f)
		return false;

	// Bound each endpoint's width sphere, then take the rectangle covering the whole segment.
	const float2 ndcRoot = clipRoot.xy / clipRoot.w;
	const float2 ndcTip = clipTip.xy / clipTip.w;
	const float radiusScale = radius / minClipW;
	const float2 extentRoot = radiusScale * float2(length(FrameBuffer::CameraViewProj[0].xyz - ndcRoot.x * clipWAxis), length(FrameBuffer::CameraViewProj[1].xyz - ndcRoot.y * clipWAxis));
	const float2 extentTip = radiusScale * float2(length(FrameBuffer::CameraViewProj[0].xyz - ndcTip.x * clipWAxis), length(FrameBuffer::CameraViewProj[1].xyz - ndcTip.y * clipWAxis));
	const float2 ndcMin = min(ndcRoot - extentRoot, ndcTip - extentTip);
	const float2 ndcMax = max(ndcRoot + extentRoot, ndcTip + extentTip);

	// One Hi-Z texel covers TAA jitter and small changes at silhouette edges.
	const float2 hiZSize = grassHiZParams.xy;
	float2 uvMin = float2(ndcMin.x, -ndcMax.y) * 0.5f + 0.5f - 1.0f / hiZSize;
	float2 uvMax = float2(ndcMax.x, -ndcMin.y) * 0.5f + 0.5f + 1.0f / hiZSize;
	if (any(uvMax <= 0.0f) || any(uvMin >= 1.0f))
		return false;
	uvMin = max(uvMin, 0.0f);
	uvMax = min(uvMax, 1.0f);

	// Pick the finest mip where the rectangle spans at most three texels on each axis.
	const float2 spanTexels = (uvMax - uvMin) * hiZSize;
	const int mip = (int)max(ceil(log2(max(max(spanTexels.x, spanTexels.y), 1.0f) * 0.5f)), 0.0f);
	if (mip > (int)grassHiZParams.w - 1)
		return false;
	const float mipScale = exp2((float)mip);
	const int2 mipSize = max(int2(ceil(hiZSize / mipScale)), int2(1, 1));
	const int2 sampleMin = clamp(int2(floor(uvMin * hiZSize / mipScale)), int2(0, 0), mipSize - 1);
	const int2 sampleMax = clamp(int2(floor(uvMax * hiZSize / mipScale)), int2(0, 0), mipSize - 1);

	const float nearestDepth = FrameBuffer::CameraProj._m22 + FrameBuffer::CameraProj._m23 / minClipW;
	return nearestDepth > LoadHiZMax3x3(sampleMin, sampleMax, mip) + 2.0e-6f;
}
#endif

bool ResolveTileHeightBounds(uint quadrant, uint tile, out float2 heightBounds)
{
	heightBounds = TileHeightBounds[quadrant * OCCUPANCY_TILES_PER_AXIS * OCCUPANCY_TILES_PER_AXIS + tile];
	return heightBounds.x > -1.0e30f && heightBounds.y >= heightBounds.x;
}

bool IsPatchOccluded(float2 worldXY, float terrainZ, float2 terrainSlope, uint quadrant, bool hasLand, bool cullsDisabled)
{
#if defined(MID_LOD) || (defined(LOW_LOD) && !defined(FAR_LOD))
	if (cullsDisabled || !hasLand || grassHiZParams.w < 1.0f)
		return false;
	const float2 tilePosition = (worldXY - data[quadrant].quadWorldPos) / QUADRANT_GRASS_SPACING;
	if (any(tilePosition < 0.0f) || any(tilePosition >= float(OCCUPANCY_TILES_PER_AXIS)))
		return false;
	const uint2 tileXY = uint2(tilePosition);
	float2 heightBounds;
	if (!ResolveTileHeightBounds(quadrant, tileXY.y * OCCUPANCY_TILES_PER_AXIS + tileXY.x, heightBounds))
		return false;
	const float bladeHeight = max(grassAOParams.w, 64.0f);
	float radius = max(grassHiZParams.z, 96.0f);
#	if defined(LOW_LOD)
	const float patchReach = 2.8284272f * BLADE_TO_WORLD;
	radius += patchReach + (patchReach + grassHiZBounds.y) * length(terrainSlope);
#	else
	radius += grassHiZBounds.y * length(terrainSlope) + grassHiZBounds.z;
#	endif
	// Expanded LAND bounds cover displaced roots and extras across changes in terrain slope.
	const float minHeight = min(terrainZ, heightBounds.x);
#	if defined(LOW_LOD)
	const float maxHeight = max(terrainZ, heightBounds.y) + GetTerrainLiftReach(worldXY, radius);
#	else
	const float maxHeight = max(terrainZ, heightBounds.y);
#	endif
	const float verticalReach = (maxHeight - minHeight) * 0.5f + radius;
	radius = length(float2(radius, verticalReach));
	const float3 centre = float3(worldXY, (minHeight + maxHeight + bladeHeight) * 0.5f) - FrameBuffer::CameraPosAdjust.xyz;
	const float minDistance = max(512.0f, radius * 1.5f);
	return IsVolumeOccluded(centre, radius, minDistance, cullsDisabled);
#else
	return false;
#endif
}

// Tests a quadrant-local box of blade roots, padded by the blade geometry radius, against Hi-Z.
bool IsRootBoxOccluded(uint quadrant, float2 localMin, float2 localMax, float2 heightBounds, float geometryRadius, float minDistanceFloor, float minDistanceScale, bool cullsDisabled)
{
#if defined(LOW_LOD)
	float2 boxReach = (localMax - localMin) * 0.5f + geometryRadius;
	heightBounds.y += GetTerrainLiftReach(data[quadrant].quadWorldPos + (localMin + localMax) * 0.5f, max(boxReach.x, boxReach.y));
#endif
	float3 reach = float3((localMax - localMin) * 0.5f + geometryRadius, (heightBounds.y - heightBounds.x) * 0.5f + geometryRadius);
	float radius = length(reach);
	float bladeHeight = max(grassAOParams.w, 64.0f);
	float3 centre = float3(data[quadrant].quadWorldPos + (localMin + localMax) * 0.5f, (heightBounds.x + heightBounds.y + bladeHeight) * 0.5f) - FrameBuffer::CameraPosAdjust.xyz;
	return IsVolumeOccluded(centre, radius, max(minDistanceFloor, radius * minDistanceScale), cullsDisabled);
}

bool IsOccupiedTileOccluded(uint bladeTask)
{
	if ((bladeTask & (WORK_OCCUPIED_TILE | WORK_HAS_LAND)) != (WORK_OCCUPIED_TILE | WORK_HAS_LAND))
		return false;
	uint quadrant = bladeTask & WORK_QUADRANT_MASK;
	uint tile = (bladeTask >> WORK_TILE_SHIFT) & WORK_TILE_MASK;
	float2 heightBounds;
	if (!ResolveTileHeightBounds(quadrant, tile, heightBounds))
		return false;
	float geometryRadius =
#if defined(FAR_LOD)
		max(grassHiZBounds.x, 96.0f);
#elif defined(HIGH_LOD) || defined(MID_LOD)
		max(grassHiZParams.z, 96.0f) + grassHiZBounds.z;
#else
		max(grassHiZParams.z, 96.0f);
#endif
	// Roots jitter up to one patch beyond the tile.
	float2 tileMin = float2(tile % OCCUPANCY_TILES_PER_AXIS, tile / OCCUPANCY_TILES_PER_AXIS) * QUADRANT_GRASS_SPACING - 2.0f * BLADE_TO_WORLD;
	float2 tileMax = tileMin + QUADRANT_GRASS_SPACING + 4.0f * BLADE_TO_WORLD;
	return IsRootBoxOccluded(quadrant, tileMin, tileMax, heightBounds, geometryRadius, 768.0f, 2.0f, debugFlags.x > 0.5f);
}

#if defined(FAR_LOD)
bool IsFarPatchBoundsOccluded(uint2 patchPos, uint quadrant, bool hasLand, bool cullsDisabled)
{
	if (cullsDisabled || !hasLand || grassHiZParams.w < 1.0f)
		return false;

	const float patchWidth = 2.0f * BLADE_TO_WORLD;
	const float2 patchMin = float2(patchPos) * patchWidth;
	const int2 tileXY = clamp(int2((patchMin + 0.5f * patchWidth) / QUADRANT_GRASS_SPACING), int2(0, 0), int2(OCCUPANCY_TILES_PER_AXIS - 1, OCCUPANCY_TILES_PER_AXIS - 1));
	float2 heightBounds;
	if (!ResolveTileHeightBounds(quadrant, tileXY.y * OCCUPANCY_TILES_PER_AXIS + tileXY.x, heightBounds))
		return false;
	return IsRootBoxOccluded(quadrant, patchMin, patchMin + patchWidth, heightBounds, max(grassHiZBounds.x, 96.0f), 512.0f, 1.5f, cullsDisabled);
}

bool IsFarGroupOccluded(uint bladeTask, uint groupX)
{
	if ((bladeTask & WORK_HAS_LAND) == 0u || (bladeTask & (WORK_OCCUPIED_TILE | WORK_COMPACT_FAR)) != 0u)
		return false;

	const uint firstPatch = groupX * THREADGROUP_SIZE;
	if (firstPatch >= PATCHES_PER_QUADRANT)
		return true;
	const uint lastPatch = min(firstPatch + THREADGROUP_SIZE - 1u, PATCHES_PER_QUADRANT - 1u);
	const uint2 firstPos = uint2(firstPatch % PATCHES_PER_ROW, firstPatch / PATCHES_PER_ROW);
	const uint2 lastPos = uint2(lastPatch % PATCHES_PER_ROW, lastPatch / PATCHES_PER_ROW);
	const bool wrapsRow = firstPos.y != lastPos.y;
	const uint2 minPatch = uint2(wrapsRow ? 0u : firstPos.x, firstPos.y);
	const uint2 maxPatch = uint2(wrapsRow ? PATCHES_PER_ROW - 1u : lastPos.x, lastPos.y) + 1u;

	const float patchWidth = 2.0f * BLADE_TO_WORLD;
	const uint2 minTile = min(uint2(float2(minPatch) * patchWidth / QUADRANT_GRASS_SPACING), OCCUPANCY_TILES_PER_AXIS - 1u);
	const uint2 maxTile = min(uint2(float2(maxPatch) * patchWidth / QUADRANT_GRASS_SPACING), OCCUPANCY_TILES_PER_AXIS - 1u);
	const uint quadrant = bladeTask & WORK_QUADRANT_MASK;

	float2 heightBounds = float2(3.402823466e+38f, -3.402823466e+38f);
	[loop] for (uint tileY = minTile.y; tileY <= maxTile.y; ++tileY)
	{
		[loop] for (uint tileX = minTile.x; tileX <= maxTile.x; ++tileX)
		{
			float2 tileBounds;
			if (!ResolveTileHeightBounds(quadrant, tileY * OCCUPANCY_TILES_PER_AXIS + tileX, tileBounds))
				return false;
			heightBounds = float2(min(heightBounds.x, tileBounds.x), max(heightBounds.y, tileBounds.y));
		}
	}
	return IsRootBoxOccluded(quadrant, float2(minPatch) * patchWidth, float2(maxPatch) * patchWidth, heightBounds, max(grassHiZBounds.x, 96.0f), 768.0f, 1.5f, false);
}
#endif

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

// Keep the nearest Voronoi feature, skipping cells whose closest point cannot beat the current best.
void TryClumpCell(inout uint clumpRand, inout float clumpDistSq, inout float2 clumpDir, int2 cell, float2 gridPos)
{
	float2 cellMin = float2(cell);
	float2 cellOffset = clamp(gridPos, cellMin, cellMin + 1.0f) - gridPos;
	if (dot(cellOffset, cellOffset) >= clumpDistSq)
		return;

	uint3 hash = Random::pcg3d(uint3(asuint(cell), 0u));
	float2 offset = cellMin + float2(hash.xy) * UINT_TO_FLOAT - gridPos;
	float distanceSquared = dot(offset, offset);
	if (distanceSquared < clumpDistSq) {
		clumpDistSq = distanceSquared;
		clumpDir = offset;
		clumpRand = hash.z;
	}
}

// Every tier searches all nine cells, because cell-wide clump traits must match between tiers.
void ComputeClump(out uint clumpRand, out float clumpDist, out float2 clumpDir, float2 worldPos, float inverseGridSize)
{
	float2 gridPos = worldPos * inverseGridSize;
	// Floor keeps the Voronoi grid continuous across negative world coordinates.
	int2 gridCell = int2(floor(gridPos));
	clumpRand = 0u;
	clumpDist = 1.0e30f;
	clumpDir = float2(0.0f, 0.0f);
	TryClumpCell(clumpRand, clumpDist, clumpDir, gridCell, gridPos);
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
		{
			if (x != 0 || y != 0)
				TryClumpCell(clumpRand, clumpDist, clumpDir, gridCell + int2(x, y), gridPos);
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
		// Use the complement of Low's fade-out throughout their shared band.
		inRamp = smoothstep(0.0f, 1.0f, (handoffDistance - lodFadeIn.x) * lodFadeIn.y);
	}

	float fullKeepRadius = min(lodFadeOut.x, farParams.x);
	// Low fades on square distance, so retain Far through the same handoff band.
	if (handoffDistance <= fullKeepRadius) {
		if (!compactFar && inRamp >= 1.0f)
			return true;

		float keep = compactFar ? saturate(inRamp / max(farParams.w, 1.0e-3f)) : inRamp;
		float dither = LodDither(bladeWorldPos2D);
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
	float dither = LodDither(bladeWorldPos2D);

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
							Random::iqint3(asuint(baseCell + int2(1, 0)))) *
	                    UINT_TO_FLOAT;
	float2 noiseUpper = float2(
							Random::iqint3(asuint(baseCell + int2(0, 1))),
							Random::iqint3(asuint(baseCell + int2(1, 1)))) *
	                    UINT_TO_FLOAT;
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

/** @brief Turns an angle toward a target along the shorter arc and wraps the result to [0, TAU). */
float TurnAngleToward(float angle, float target, float weight)
{
	float difference = target - angle;
	difference -= Math::TAU * round(difference * (1.0f / Math::TAU));
	float turned = angle + difference * weight;
	return turned - Math::TAU * floor(turned * (1.0f / Math::TAU));
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

#	if defined(MID_LOD)
	float lodDistance = length(lodOffset);
	float inRamp = saturate((lodDistance - lodFadeIn.x) * lodFadeIn.y);
	float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
	float dither = LodDither(bladeWorldPos2D);
	return !((inRamp < 1.0f && dither <= 1.0f - inRamp) || dither > outRamp);
#	else
	float lodDistanceSq = dot(lodOffset, lodOffset);
#		if defined(LOW_LOD)
	float lodFadeOutDistance = max(lodOffset.x, lodOffset.y);
#		else
	float lodFadeOutDistanceSq = lodDistanceSq;
#		endif
	float lodFadeInEnd = lodFadeIn.x + rcp(max(lodFadeIn.y, 1.0e-6f));
	float lodFadeInStartSq = lodFadeIn.x * lodFadeIn.x;
	float lodFadeInEndSq = lodFadeInEnd * lodFadeInEnd;
#		if defined(LOW_LOD)
	bool beforeFadeOut = lodFadeOutDistance <= lodFadeOut.x;
	bool afterFadeOut = lodFadeOutDistance >= lodFadeIn.w;
#		else
	float lodFadeOutStartSq = lodFadeOut.x * lodFadeOut.x;
	float lodFadeOutEndSq = lodFadeIn.w * lodFadeIn.w;
	bool beforeFadeOut = lodFadeOutDistanceSq <= lodFadeOutStartSq;
	bool afterFadeOut = lodFadeOutDistanceSq >= lodFadeOutEndSq;
#		endif

#		if defined(LOW_LOD)
	// Mid is only generated in the loaded cells. Beyond them Low has no tier to hand off to, so it stays whole
	// rather than thinning toward a Mid that is not there.
	if (any(bladeWorldPos2D < loadedLandBounds.xy) || any(bladeWorldPos2D > loadedLandBounds.zw)) {
		lodFadeInStartSq = 0.0f;
		lodFadeInEndSq = 0.0f;
	}
	if (lodDistanceSq <= lodFadeInStartSq)
		return false;
#		endif

	if (lodDistanceSq >= lodFadeInEndSq && beforeFadeOut)
		return true;

	float dither = LodDither(bladeWorldPos2D);
	if (lodDistanceSq >= lodFadeInEndSq && afterFadeOut)
		return dither <= lodFadeOut.z;

	float lodDistance = sqrt(lodDistanceSq);
	float inRamp = lodDistanceSq >= lodFadeInEndSq ? 1.0f : saturate((lodDistance - lodFadeIn.x) * lodFadeIn.y);
#		if defined(LOW_LOD)
	float outRamp = lerp(1.0f, lodFadeOut.z, smoothstep(0.0f, 1.0f, (lodFadeOutDistance - lodFadeOut.x) * lodFadeOut.y));
#		else
	float outRamp = lerp(1.0f, lodFadeOut.z, saturate((lodDistance - lodFadeOut.x) * lodFadeOut.y));
#		endif
#		if defined(LOW_LOD)
	if ((inRamp < 1.0f && dither <= 1.0f - inRamp) || dither > outRamp)
#		elif defined(HIGH_LOD)
	if (dither > min(inRamp, outRamp))
#		endif
		return false;
	return true;
#	endif
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
	float clumpReach = generatorType.clumpGridSize * 0.1125f * abs(generatorType.clumpDistanceFactor);
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
		float widthExtent = generatorType.width * 2.5f * 1.3f * 32.0f * 2.0f;
		float geometryExtent = generatorType.height + widthExtent + GetTerrainLiftReach(bladeWorldPos2D, 0.0f);
		if (!cullsDisabled && IsOutsideFrustum(viewPos, geometryExtent))
			return false;
	}

	if (!cullsDisabled && objectClearance <= occlusionParams.w)
		return false;
#endif

	// Delay the nine-cell clump search until after the inexpensive rejection tests.
	uint clumpRand;
	float clumpDist;
	float2 clumpDir;
	ComputeClump(clumpRand, clumpDist, clumpDir, bladeWorldPos2D, generatorType.inverseClumpGridSize);
	// Height, facing, lean, and colour belong to the whole Voronoi cell. Only the pull and base AO fall off with distance.
	float clumpDensity = 1.0f - smoothstep(0.15f, 0.50f, clumpDist);

	hash = Random::pcg3d(hash);
	float clumpDistRand = float(hash.x) * UINT_TO_FLOAT;
	float heightRand = float(hash.y) * UINT_TO_FLOAT;
	float angleRand = float(hash.z) * UINT_TO_FLOAT;

#if !defined(FAR_LOD)
	// Pull every near blade toward its Voronoi feature to hide the regular candidate lattice.
	float clumpPull = lerp(0.025f, 0.225f, clumpDistRand);
	float2 clumpDisplace = clumpDir * generatorType.clumpGridSize * clumpPull * generatorType.clumpDistanceFactor * clumpDensity;
	bladeWorldPos2D += clumpDisplace;

#	if defined(LOW_LOD)
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
#	else
	bladeWorldZ += dot(terrainSlope, clumpDisplace);
#	endif
#endif
#if !defined(FAR_LOD)
	worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	objectClearance = GetObjectClearance(worldPos, cullsDisabled);
	viewPos = worldPos - FrameBuffer::CameraPosAdjust.xyz;

	if (!insideFrustum) {
		// A root outside the frustum can still produce visible blade geometry near the edge.
		float widthExtent = generatorType.width * 2.5f * 1.3f;
#	if defined(LOW_LOD)
		widthExtent *= 2.0f * (1.0f + miscParams.z);
#	elif defined(MID_LOD)
		widthExtent *= 1.41421356f * (1.0f + miscParams.z);
#	else
		widthExtent *= 1.0f + miscParams.z;
#	endif
		float geometryExtent = generatorType.height + widthExtent;
#	if defined(LOW_LOD)
		geometryExtent += GetTerrainLiftReach(bladeWorldPos2D, 0.0f);
#	endif
		if (!cullsDisabled && IsOutsideFrustum(viewPos, geometryExtent))
			return false;
	}

#	if !defined(LOW_LOD)
	if (!PassesBladeLOD(bladeWorldPos2D, cullsDisabled))
		return false;
#	endif

	// Preserve grass beneath overhangs when there is still vertical room for part of the blade.
	if (!cullsDisabled && objectClearance <= occlusionParams.w)
		return false;
#endif

#if defined(LOW_LOD)
	if (!cullsDisabled) {
		viewPos.z += GetTerrainLift(bladeWorldPos2D, bladeWorldZ);
#	if defined(FAR_LOD)
		// Test the raised root: terrain LOD that the lift accounts for must not count as an object above it.
		if (IsRootUnderObject(viewPos, terrainSlope))
			return false;
#	endif
	}
#endif

	// Height generation is deferred until after rejection because the frustum test uses type bounds.
	// Blades take a share of their clump's height, so neighbouring clumps stand at different heights.
	float clumpHeightRandom = float(clumpRand) * UINT_TO_FLOAT;
	float unscaledHeight = 0.45f + lerp(heightRand, clumpHeightRandom, generatorType.clumpHeightFactor) * 0.55f;
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
	float clumpedAngle = angleRand * Math::TAU;
#if !defined(FAR_LOD)
	// Positive factors splay a clump away from its centre; negative factors turn it inward.
	float2 clumpFacingDir = clumpDir * -sign(generatorType.clumpFacingFactor);
	clumpedAngle = TurnAngleToward(clumpedAngle, atan2(clumpFacingDir.y, clumpFacingDir.x), abs(generatorType.clumpFacingFactor));
#endif
	// Every tier shares the per-clump lean, since a common direction changes how distant clumps shade.
	[branch] if (generatorType.clumpLeanFactor > 0.0f)
	{
		uint leanState = clumpRand;
		float clumpLeanAngle = float(Random::pcg(leanState)) * UINT_TO_FLOAT * Math::TAU;
		clumpedAngle = TurnAngleToward(clumpedAngle, clumpLeanAngle, generatorType.clumpLeanFactor);
	}
	if (miscParams.y > 0.0f) {
		float steepness = sqrt(saturate(1.0f - terrainNormalZ * terrainNormalZ));
		if (steepness > 1e-4f)
			clumpedAngle = TurnAngleToward(clumpedAngle, atan2(-terrainSlope.y, -terrainSlope.x), miscParams.y * steepness);
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
	float detailFade = 1.0f - smoothstep(512.0f, 1536.0f, appearanceDistance);
	outerGeometry = float(hash.z) * UINT_TO_FLOAT >= detailFade;
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
#	if defined(LOW_OUTER_GEOMETRY)
	// Switch single blades after the Mid handoff, only when their tip is narrower than half a pixel.
	float outerStart = lodFadeIn.x + rcp(max(lodFadeIn.y, 1.0e-6f));
	float outerKeep = saturate((length(bladeWorldPos2D - grassLodOrigin) - outerStart) * (1.0f / 1024.0f));
	float lowTipWidth = generatorType.width * 5.0f * lerp(0.45f, 1.3f, float(packedWidth) * (1.0f / 255.0f)) * 0.06f;
	float tipViewDepth = mul(FrameBuffer::CameraViewProjUnjittered, float4(viewPos, 1.0f)).w - generatorType.height;
	float projectedTipWidth = lowTipWidth * max(cameraViewRow0Sum + abs(FrameBuffer::CameraProj._m00) * miscParams.z, cameraViewRow1Sum) /
	                          (max(tipViewDepth, 1.0f) * min(dynamicResolutionInverted.x, dynamicResolutionInverted.y));
	outerKeep *= 1.0f - smoothstep(0.25f, 0.5f, projectedTipWidth);
	outerGeometry = storedHeight == 0u && float(hash.z) * UINT_TO_FLOAT < outerKeep;
#	endif
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
	uint packedRandBend = (uint)round(saturate(float(tiltHash.y) * UINT_TO_FLOAT) * 15.0f);
	b.seedAndType = packedClumpDensity << 24 | packedRandBend << 20 | (clumpRand & 0xFFu) << 8 | (type & 0xFFu);

	// Mirror the Far VS: packed root and directions, distance widening, and coverage compensation.
	float3 packedRoot = float3(f16tof32(b.posXY >> 16), f16tof32(b.posXY), f16tof32(b.posZWidthHeight >> 16));
	float4 packedDirectionValues = float4(packedDirections) * (2.0f / 255.0f) - 1.0f;
	float2 farTip = packedDirectionValues.zw * (generatorType.height * float(packedHeight) * (1.0f / 255.0f));
	float3 packedTip = packedRoot + float3(packedDirectionValues.xy * farTip.x, farTip.y);
	float2 farRootOffset = packedRoot.xy + FrameBuffer::CameraPosAdjust.xy - grassLodOrigin;
	float2 farCoverage = GetFarCoverage(farRootOffset, FrameBuffer::CameraProj._m00);
	float farWidth = generatorType.width * 2.5f * lerp(0.45f, 1.3f, float(packedWidth) * (1.0f / 255.0f)) *
	                 lerp(2.0f, 32.0f, farCoverage.x) * farCoverage.y;
	if (IsBladeOccluded(packedRoot, packedTip, farWidth + 1.0f, cullsDisabled))
		return false;
#else
	uint packedRandBend = (uint)round(saturate(float(tiltHash.y) * UINT_TO_FLOAT) * 15.0f);

#	if !defined(LOW_LOD)
	// Only detailed materials consume the packed per-blade colour seed.
	uint packedBladeColor = 0u;
#		if defined(HIGH_GEOMETRY_LOD)
	[branch] if (!outerGeometry)
#		endif
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
		perBladeColor *= lerp(1.0f, clumpTint * clumpValue, surfaceType.clumpColorStrength);

		// Pack blade-wide colour variation. The pixel shader evaluates spatial blotch and grain detail.
		uint3 packedColor = (uint3)round(saturate(perBladeColor * 0.5f) * 15.0f);
		packedBladeColor = packedColor.x | packedColor.y << 4u | packedColor.z << 8u;
	}
	uint packedBladeData = packedBladeColor | packedRandBend << 12u;
#	endif

	float tiltSin, tiltCos;
	sincos(randTilt, tiltSin, tiltCos);
	int2 packedFacing = (int2)round(clamp(randFacing, -1.0f, 1.0f) * 127.0f);

#	if defined(LOW_LOD) && !defined(FAR_LOD)
	uint lowTiltX = f32tof16(tiltSin);
	uint lowTiltY = f32tof16(tiltCos);
	float2 lowTip = float2(f16tof32(lowTiltX), f16tof32(lowTiltY)) * lowDrawHeight;
	float lowWidthScale = float(packedWidth) * (1.0f / 255.0f);
	float lowRandWidth = generatorType.width * 5.0f * lerp(0.45f, 1.3f, lowWidthScale);
	float2 packedFacingValue = float2(packedFacing) * (1.0f / 127.0f);
	float packedWidthValue = f16tof32(f32tof16(lowRandWidth));
	float2 lowBaseAxis = float2(-packedFacingValue.y, packedFacingValue.x) * packedWidthValue;
	b.posZWidthHeight = f32tof16(viewPos.z) << 16 | packedRandBend << 1 | storedHeight;
	float3 packedRoot = float3(f16tof32(b.posXY >> 16), f16tof32(b.posXY), f16tof32(b.posZWidthHeight >> 16));

	// Test the geometry the VS will draw: packed root, tip along the facing, and view-thickened width.
	float3 packedTip = packedRoot + float3(packedFacingValue * lowTip.x, lowTip.y);
	float bladeRadius = packedWidthValue * (1.0f + miscParams.z) + 1.0f;
	if (IsBladeOccluded(packedRoot, packedTip, bladeRadius, cullsDisabled))
		return false;
#	endif

#	if defined(HIGH_LOD)
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
#		if defined(TERRAIN_SHADOWS)
	worldShadow *= TerrainShadows::GetTerrainShadow(worldPos, LinearSampler);
#		endif
#		if defined(CLOUD_SHADOWS)
	worldShadow *= CloudShadows::GetCloudShadowMult(viewPos, LinearSampler);
#		endif

	uint2 packedTilt = (uint2)round(saturate(float2(tiltSin, tiltCos) * 0.5f + 0.5f) * 255.0f);
	uint packedWorldShadow = (uint)round(saturate(worldShadow) * 255.0f);
	uint packedDetailFade = (uint)round(detailFade * 255.0f);
	b.tipDir = packedTilt.x | packedTilt.y << 8 | packedWorldShadow << 16 | packedDetailFade << 24;
#	elif defined(MID_LOD)
	uint2 packedTilt = (uint2)round(saturate(float2(tiltSin, tiltCos) * 0.5f + 0.5f) * 255.0f);
	float appearanceDistance = ApproximateGrassDistance(bladeWorldPos2D - grassLodOrigin);
	uint packedLodDistance = (uint)round(saturate(appearanceDistance * (1.0f / 6144.0f)) * 65535.0f);
	b.tipDir = packedTilt.x | packedTilt.y << 8 | packedLodDistance << 16;
#	else
	b.tipDir = f32tof16(lowTip.x) << 16 | f32tof16(lowTip.y);
#	endif
	b.hashClumpAndGrassType = hashClumpAndGrassType;

#	if defined(LOW_LOD) && !defined(FAR_LOD)
	b.facingAndWind = (uint)(packedFacing.x & 0xFF) | (uint)(packedFacing.y & 0xFF) << 8 | f32tof16(lowRandWidth) << 16;
	b.previousWind = f32tof16(lowBaseAxis.x) << 16 | f32tof16(lowBaseAxis.y);
#	else
	b.facingAndWind = (uint)(packedFacing.x & 0xFF) | (uint)(packedFacing.y & 0xFF) << 8 | f32tof16(windDisplacement) << 16;
	b.previousWind = packedBladeData << 16 | f32tof16(previousWindDisplacement);
#	endif

#	if defined(MID_LOD)
	b.skylightingRoot = viewPos;
#	elif !defined(LOW_LOD)
#		if defined(SKYLIGHTING)
	float3 probeCell = round(FrameBuffer::CameraPosAdjust.xyz / Skylighting::CELL_SIZE);
	float3 probeOffset = probeCell * Skylighting::CELL_SIZE - FrameBuffer::CameraPosAdjust.xyz;
	uint3 probeArrayOrigin = (uint3)((int3)probeCell - (int3)(Skylighting::ARRAY_DIM / 2)) % Skylighting::ARRAY_DIM;
	float3 skylightingPosition = viewPos;
	sh2 skylightingSH = Skylighting::SampleWithOrigin(skylightingPosition, float3(0.0f, 0.0f, 1.0f), probeOffset, probeArrayOrigin);

	b.skylightingSH0 = f32tof16(skylightingSH.x) << 16 | f32tof16(skylightingSH.y);
	b.skylightingSH1 = f32tof16(skylightingSH.z) << 16 | f32tof16(skylightingSH.w);
#		else
	b.skylightingSH0 = 0u;
	b.skylightingSH1 = 0u;
#		endif
#	endif

#	if defined(PGRASS_CACHED_COLLISION)
	// Cache one tip collision sample. The VS scales it smoothly from the anchored root.
	float2 collisionTip = float2(tiltSin, tiltCos) * randHeight;
	float3 collisionTipViewPos = viewPos + float3(randFacing * collisionTip.x, collisionTip.y);
	collisionTipViewPos.xy += windDir * windDisplacement;

	float3 collisionDisplacement;

#		if defined(MID_LOD)
	float3 unusedPreviousCollisionDisplacement;
	GrassCollision::GetDisplacedPosition(collisionTipViewPos, viewPos, 1.0f, 2048.0f, true, 0.75f,
		collisionDisplacement, unusedPreviousCollisionDisplacement);
	b.collisionData = f32tof16(collisionDisplacement.x) << 16 | f32tof16(collisionDisplacement.y);
	b.previousWind = packedBladeData << 16 | f32tof16(collisionDisplacement.z);
#		else
	float3 previousCollisionDisplacement;
	GrassCollision::GetDisplacedPosition(collisionTipViewPos, viewPos, 1.0f, 2048.0f, true, 0.75f,
		collisionDisplacement, previousCollisionDisplacement);

	b.collisionData.x = f32tof16(collisionDisplacement.x) << 16 | f32tof16(collisionDisplacement.y);
	b.collisionData.y = f32tof16(collisionDisplacement.z) << 16 | f32tof16(previousCollisionDisplacement.x);
	b.collisionData.z = f32tof16(previousCollisionDisplacement.y) << 16 | f32tof16(previousCollisionDisplacement.z);
#		endif
#	endif
#endif

	return true;
}

#if defined(LOW_LOD) && !defined(FAR_LOD) && SLOPE_EXTRA_BLADES > 0
/**
 * @brief Returns how many extra blades a patch needs to match Mid's two base blades and slope fill.
 * Slots below the count are always kept and the next one in proportion to the remainder, so the slot count only has
 * to cover the largest count rather than dilute it.
 */
float GetLowExtraCount(float terrainNormalZ)
{
	float slopeKeep = saturate(rcp(max(terrainNormalZ, 0.05f)) - 1.0f);
	float densityRatio = BLADE_TO_WORLD / max(midCandidateSpacing, 1.0f);
	densityRatio *= densityRatio;
	return clamp(distantFill * (densityRatio * (2.0f + slopeKeep) - 1.0f), 0.0f, SLOPE_EXTRA_BLADES);
}

void AppendLowBlade(Blade blade, bool outerGeometry)
{
	uint slot;
#	if defined(LOW_OUTER_GEOMETRY)
	if (outerGeometry) {
		InterlockedAdd(LowOuterCount, 1u, slot);
		GroupBlades[THREADGROUP_SIZE * (1 + SLOPE_EXTRA_BLADES) - 1u - slot] = blade;
		return;
	}
#	endif
	InterlockedAdd(LowInnerCount, 1u, slot);
	GroupBlades[slot] = blade;
}

void GenerateLowExtra(uint3 dispatch, uint extraTask)
{
	uint activePatch = extraTask / SLOPE_EXTRA_BLADES;
	uint extraIndex = extraTask % SLOPE_EXTRA_BLADES;
	LowPatchSetup setup = LowPatchSetups[activePatch];
	uint bladeTask = VisibleBladeTasks[dispatch.z];
	uint quadrant = bladeTask & WORK_QUADRANT_MASK;
	bool hasLand = (bladeTask & WORK_HAS_LAND) != 0u;
	bool insideFrustum = (bladeTask & WORK_INSIDE_FRUSTUM) != 0u;
	bool cullsDisabled = debugFlags.x > 0.5f;
	QuadrantData quadrantData = data[quadrant];
	uint2 patchPos = uint2(setup.patch % PATCHES_PER_ROW, setup.patch / PATCHES_PER_ROW);
	uint3 candidateHash = ExtraCandidateHash(patchPos, extraIndex, quadrantData.quadrantHash);
	float2 candidateQuadPos = ExtraCandidateQuadPos(patchPos, candidateHash);
	float2 candidateWorldPos = candidateQuadPos + quadrantData.quadWorldPos;
	float terrainNormalZ = rsqrt(dot(setup.terrainSlope, setup.terrainSlope) + 1.0f);
	if (!cullsDisabled && float(candidateHash.z) * UINT_TO_FLOAT > GetLowExtraCount(terrainNormalZ) - float(extraIndex))
		return;
	float2 candidateMapSamplePos = GrassMapSamplePos(candidateQuadPos, candidateHash);
	uint packedGrassCell = LoadGrassCell(candidateMapSamplePos, quadrant);
	if (!cullsDisabled && packedGrassCell == 0u)
		return;
	float candidateWorldZ = setup.baseWorldZ + dot(setup.terrainSlope, candidateWorldPos - setup.baseWorldPos2D);
	Blade blade;
	bool outerGeometry;
	if (BuildBlade(candidateHash, candidateMapSamplePos, candidateWorldPos, candidateWorldZ,
			setup.terrainSlope, terrainNormalZ, quadrantData.quadWorldPos, quadrant, hasLand, packedGrassCell, cullsDisabled, insideFrustum, false, blade, outerGeometry))
		AppendLowBlade(blade, outerGeometry);
}
#endif

#if defined(FAR_LOD) && SLOPE_EXTRA_BLADES > 0
/** @brief Retains matching candidate counts until Low is gone, then gradually returns to sparse Far fill. */
float GetFarExtraCount(float2 world2D, float terrainNormalZ)
{
	float slopeKeep = saturate(rcp(max(terrainNormalZ, 0.05f)) - 1.0f);
	float densityRatio = BLADE_TO_WORLD / max(midCandidateSpacing, 1.0f);
	densityRatio *= densityRatio;
	float seamExtras = clamp(distantFill * (densityRatio * (2.0f + slopeKeep) - 1.0f), 0.0f, SLOPE_EXTRA_BLADES);
	float distantExtras = min(2.0f * distantFill * max(saturate(lodFadeIn.z + 2.0f * slopeKeep), farParams.w), SLOPE_EXTRA_BLADES);
	float2 offset = abs(world2D - grassLodOrigin);
	float squareDistance = max(offset.x, offset.y);
	float handoffEnd = lodFadeOut.x;
	float distantBlend = smoothstep(handoffEnd, handoffEnd + 4096.0f, squareDistance);
	float extraFade = 1.0f - smoothstep(handoffEnd + 4096.0f, handoffEnd + 6144.0f, squareDistance);
	return lerp(seamExtras, distantExtras, distantBlend) * extraFade;
}
#endif

void GenerateThreadBlades(uint3 dispatch, uint groupIndex, out uint2 emittedBladeCounts)
{
	emittedBladeCounts = 0u;

#if defined(LOW_LOD) && !defined(FAR_LOD) && SLOPE_EXTRA_BLADES > 0
	uint bladeTask = VisibleBladeTasks[dispatch.z];
	uint patchSlot = groupIndex;
	bool laneHasPatch = patchSlot < LOW_PATCHES_PER_GROUP;

	if (laneHasPatch) {
		uint dispatchGroup = dispatch.x / THREADGROUP_SIZE;
		uint patch = dispatchGroup * LOW_PATCHES_PER_GROUP + patchSlot;
		bool validPatch = ResolveTilePatch(bladeTask, patch);
		if (validPatch && debugFlags.x <= 0.5f && (bladeTask & WORK_FULL_GRASS) == 0u && !PatchHasGrass(uint2(patch % PATCHES_PER_ROW, patch / PATCHES_PER_ROW), bladeTask & WORK_QUADRANT_MASK))
			validPatch = false;

		if (validPatch) {
			uint quadrant = bladeTask & WORK_QUADRANT_MASK;
			bool hasLand = (bladeTask & WORK_HAS_LAND) != 0u;
			bool nearCovered = (bladeTask & WORK_NEAR_COVERED) != 0u;
			bool compactFar = (bladeTask & WORK_COMPACT_FAR) != 0u;
			bool cullsDisabled = debugFlags.x > 0.5f;
			QuadrantData quadrantData = data[quadrant];
			uint2 patchPos = uint2(patch % PATCHES_PER_ROW, patch / PATCHES_PER_ROW);
			uint quadrantHash = quadrantData.quadrantHash;
			uint bladeIndex = (bladeTask >> WORK_LANE_SHIFT) & 0xFu;

			uint3 baseHash;
			uint2 gridPos = BaseGridPosition(patchPos, bladeIndex);
			baseHash = Random::pcg3d(uint3(gridPos, quadrantHash));
			float2 baseQuadPos2D = BaseQuadrantPosition(gridPos, baseHash);

			float2 baseWorldPos2D = baseQuadPos2D + quadrantData.quadWorldPos;
			float2 baseMapSamplePos = GrassMapSamplePos(baseQuadPos2D, baseHash);
			bool useBasePath = PassesEarlyFarLOD(baseWorldPos2D, nearCovered, compactFar, cullsDisabled);
			uint baseGrassCell = 0u;

			if (useBasePath) {
				baseGrassCell = LoadGrassCell(baseMapSamplePos, quadrant);
				if (!cullsDisabled && baseGrassCell == 0u)
					useBasePath = false;
			}

			float2 terrainSlope;
			float baseWorldZ;
			baseWorldZ = TerrainHeightSlopeAt(terrainSlope, baseWorldPos2D, quadrantData.quadWorldPos, quadrant, hasLand);

			if (!(useBasePath && IsPatchOccluded(baseWorldPos2D, baseWorldZ, terrainSlope, quadrant, hasLand, cullsDisabled))) {
				// Share only the terrain plane needed by extra candidates. Base inputs stay in this lane.
				LowPatchSetup setup;
				setup.baseWorldPos2D = baseWorldPos2D;
				setup.terrainSlope = terrainSlope;
				setup.baseWorldZ = baseWorldZ;
				setup.patch = patch;

				// Only accepted patches contribute extra candidates to the group queue.
				uint activeSlot;
				InterlockedAdd(LowActiveCount, 1u, activeSlot);
				LowPatchSetups[activeSlot] = setup;

				if (useBasePath) {
					float terrainNormalZ = rsqrt(dot(terrainSlope, terrainSlope) + 1.0f);
					Blade blade;
					bool outerGeometry;
					bool insideFrustum = (bladeTask & WORK_INSIDE_FRUSTUM) != 0u;
					if (BuildBlade(baseHash, baseMapSamplePos, baseWorldPos2D, baseWorldZ,
							terrainSlope, terrainNormalZ, quadrantData.quadWorldPos, quadrant, hasLand, baseGrassCell, cullsDisabled, insideFrustum, true, blade, outerGeometry))
						AppendLowBlade(blade, outerGeometry);
				}
			}
		}
	}
	return;
#else
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

	// Full-quadrant dispatches round up to whole groups, so also reject the tail past the last patch.
	if (!ResolveTilePatch(bladeTask, patch))
		return;

#	if defined(FAR_LOD)
	if (compactFar) {
		uint activePatchCount = max(1u, (uint)ceil(PATCHES_PER_QUADRANT * saturate(farParams.w)));

		if (dispatch.x >= activePatchCount)
			return;

		// An odd permutation spreads the retained candidates over the entire quadrant.
		patch = (patch * 40501u + data[quadrant].quadrantHash) % PATCHES_PER_QUADRANT;
	}
#	endif

	bool cullsDisabled = debugFlags.x > 0.5f;
	QuadrantData quadrantData = data[quadrant];
	uint2 patchPos = uint2(patch % PATCHES_PER_ROW, patch / PATCHES_PER_ROW);
	uint quadrantHash = quadrantData.quadrantHash;
	if (!cullsDisabled && (bladeTask & WORK_FULL_GRASS) == 0u && !PatchHasGrass(patchPos, quadrant))
		return;

	// Preserve the base blade slot's position and seed.
	uint3 baseHash;
	uint2 gridPos = BaseGridPosition(patchPos, bladeIndex);
	baseHash = Random::pcg3d(uint3(gridPos, quadrantHash));
	float2 baseQuadPos2D = BaseQuadrantPosition(gridPos, baseHash);
	float2 baseWorldPos2D = baseQuadPos2D + quadrantData.quadWorldPos;
	float2 baseMapSamplePos = GrassMapSamplePos(baseQuadPos2D, baseHash);
	uint baseGrassCell = 0u;
	bool useBasePath = PassesEarlyFarLOD(baseWorldPos2D, nearCovered, compactFar, cullsDisabled);
#	if !defined(LOW_LOD) && !defined(FAR_LOD)
	if (!cullsDisabled) {
		float2 baseLodXY = baseWorldPos2D - grassLodOrigin;
		float baseDistSq = dot(baseLodXY, baseLodXY);
		float baseCullDist = lodFadeIn.w;
		if (baseDistSq >= baseCullDist * baseCullDist)
			useBasePath = false;
	}
#	endif

	if (useBasePath) {
		baseGrassCell = LoadGrassCell(baseMapSamplePos, quadrant);

		if (!cullsDisabled && baseGrassCell == 0u)
			useBasePath = false;
	}

#	if defined(FAR_LOD)
	// The patch already passed the coarse grass check, and the candidate loop tests every extra itself, so a patch
	// whose base failed is not scanned for a valid extra first.
#		if SLOPE_EXTRA_BLADES > 0
	if (!useBasePath && !allowSlopeExtras)
		return;
#		else
	if (!useBasePath)
		return;
#		endif
	if (IsFarPatchBoundsOccluded(patchPos, quadrant, hasLand, cullsDisabled))
		return;
#	elif defined(LOW_LOD) && SLOPE_EXTRA_BLADES > 0
	if (!useBasePath && !cullsDisabled) {
		bool anyExtraGrass = false;
		[unroll] for (uint extraIndex = 0u; extraIndex < SLOPE_EXTRA_BLADES; ++extraIndex)
		{
			uint3 extraHash = ExtraCandidateHash(patchPos, extraIndex, quadrantHash);
			float2 extraQuadPos = ExtraCandidateQuadPos(patchPos, extraHash);
			if (LoadGrassCell(GrassMapSamplePos(extraQuadPos, extraHash), quadrant) != 0u) {
				anyExtraGrass = true;
				break;
			}
		}

		if (!anyExtraGrass)
			return;
	}
#	else
#		if SLOPE_EXTRA_BLADES > 0
	if (!useBasePath && bladeIndex >= SLOPE_EXTRA_BLADES)
		return;
#		else
	if (!useBasePath)
		return;
#		endif
#	endif

	// One bilinear terrain sample establishes the plane for this path and its extras.
	float2 terrainSlope;
	float baseWorldZ;
	baseWorldZ = TerrainHeightSlopeAt(terrainSlope, baseWorldPos2D, quadrantData.quadWorldPos, quadrant, hasLand);
	if (useBasePath && IsPatchOccluded(baseWorldPos2D, baseWorldZ, terrainSlope, quadrant, hasLand, cullsDisabled))
		return;
	float terrainNormalZ = rsqrt(dot(terrainSlope, terrainSlope) + 1.0f);

#	if SLOPE_EXTRA_BLADES > 0
	// Reject slope extras before grass typing, clumping, LOD, occlusion, wind, and packing.
#		if defined(FAR_LOD)
	float farExtraCount = allowSlopeExtras ? GetFarExtraCount(baseWorldPos2D, terrainNormalZ) : 0.0f;
#		else
	float baseSlopeKeep = saturate(1.0f / max(terrainNormalZ, 0.05f) - 1.0f);
#		endif
	// Keep one emit path and let FXC choose the legal loop form for each permutation.
	uint candidateCount = 1u + SLOPE_EXTRA_BLADES;
#		if defined(FAR_LOD)
	if (!cullsDisabled)
		candidateCount = 1u + uint(ceil(farExtraCount));
#		endif
	for (uint candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
		bool isBase = candidateIndex == 0;
		uint3 candidateHash = baseHash;
		float2 candidateWorldPos = baseWorldPos2D;
		float2 candidateMapSamplePos = baseMapSamplePos;
		uint packedGrassCell = baseGrassCell;
		float candidateWorldZ = baseWorldZ;
		bool candidateValid = useBasePath;

		if (!isBase) {
			uint emitExtraIndex = candidateIndex - 1;
#		if defined(FAR_LOD)
			if (!allowSlopeExtras)
				continue;

			candidateHash = ExtraCandidateHash(patchPos, emitExtraIndex, quadrantHash);
			float2 extraQuadPos = ExtraCandidateQuadPos(patchPos, candidateHash);
			candidateWorldPos = extraQuadPos + quadrantData.quadWorldPos;
			candidateMapSamplePos = GrassMapSamplePos(extraQuadPos, candidateHash);
			candidateValid = PassesEarlyFarLOD(candidateWorldPos, nearCovered, compactFar, cullsDisabled);
			if (!candidateValid)
				continue;

			float extraKeep = saturate(farExtraCount - float(emitExtraIndex));
			float keepRand = float(Random::pcg3d(uint3(asuint(candidateWorldPos), SLOPE_EXTRA_SEED_BASE + emitExtraIndex)).x) * UINT_TO_FLOAT;

			if (!cullsDisabled && keepRand > extraKeep)
				continue;
			packedGrassCell = LoadGrassCell(candidateMapSamplePos, quadrant);
			if (!cullsDisabled && packedGrassCell == 0u)
				continue;
#		else
			if ((emitExtraIndex % PATCH_BLADE_COUNT) != bladeIndex)
				continue;

			candidateHash = ExtraCandidateHash(patchPos, emitExtraIndex, quadrantHash);
#			if !defined(LOW_LOD)
			float slopeRoll = float(candidateHash.z) * UINT_TO_FLOAT;
			if (!cullsDisabled && slopeRoll > baseSlopeKeep)
				continue;
#			endif

			float2 candidateQuadPos = ExtraCandidateQuadPos(patchPos, candidateHash);
			candidateWorldPos = candidateQuadPos + quadrantData.quadWorldPos;
			candidateMapSamplePos = GrassMapSamplePos(candidateQuadPos, candidateHash);
			packedGrassCell = LoadGrassCell(candidateMapSamplePos, quadrant);
			if (!cullsDisabled && packedGrassCell == 0u)
				continue;
			candidateValid = true;
#		endif
			candidateWorldZ = baseWorldZ + dot(terrainSlope, candidateWorldPos - baseWorldPos2D);
		}

		if (!candidateValid)
			continue;

		Blade blade;
		bool outerGeometry;
		if (BuildBlade(candidateHash, candidateMapSamplePos, candidateWorldPos, candidateWorldZ,
				terrainSlope, terrainNormalZ, quadrantData.quadWorldPos, quadrant, hasLand, packedGrassCell, cullsDisabled, insideFrustum, isBase, blade, outerGeometry)) {
			GroupBlades[emittedBladeCount * THREADGROUP_SIZE + groupIndex] = blade;
#		if defined(HIGH_GEOMETRY_LOD)
			GroupBladeOuter[emittedBladeCount * THREADGROUP_SIZE + groupIndex] = outerGeometry ? 1u : 0u;
#		endif

			if (outerGeometry)
				emittedBladeCounts.y++;
			else
				emittedBladeCounts.x++;
			emittedBladeCount++;
		}
	}
#	else
	if (useBasePath) {
		Blade blade;
		bool outerGeometry;
		if (BuildBlade(baseHash, baseMapSamplePos, baseWorldPos2D, baseWorldZ, terrainSlope, terrainNormalZ, quadrantData.quadWorldPos, quadrant, hasLand, baseGrassCell, cullsDisabled, insideFrustum, true, blade, outerGeometry)) {
			GroupBlades[groupIndex] = blade;
#		if defined(HIGH_GEOMETRY_LOD)
			GroupBladeOuter[groupIndex] = outerGeometry ? 1u : 0u;
#		endif

			if (outerGeometry)
				emittedBladeCounts.y = 1u;
			else
				emittedBladeCounts.x = 1u;
		}
	}
#	endif

#endif
}

[numthreads(TG_DIM_X, TG_DIM_Y, 1)] void main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex) {
	uint tileTask = VisibleBladeTasks[dispatch.z];
	if (groupIndex == 0u) {
		GroupTileOccluded = 0u;
		if (grassHiZParams.w >= 1.0f && debugFlags.x <= 0.5f) {
			if ((tileTask & (WORK_OCCUPIED_TILE | WORK_HAS_LAND)) == (WORK_OCCUPIED_TILE | WORK_HAS_LAND))
				GroupTileOccluded = IsOccupiedTileOccluded(tileTask) ? 1u : 0u;
#if defined(FAR_LOD)
			else if ((tileTask & WORK_HAS_LAND) != 0u)
				GroupTileOccluded = IsFarGroupOccluded(tileTask, groupID.x) ? 1u : 0u;
#endif
		}
#if defined(LOW_LOD) && !defined(FAR_LOD) && SLOPE_EXTRA_BLADES > 0
		LowActiveCount = 0u;
		LowInnerCount = 0u;
		LowOuterCount = 0u;
		GroupOutputBase = 0u;
#endif
	}
	GroupMemoryBarrierWithGroupSync();
	if (GroupTileOccluded != 0u)
		return;
	uint2 emittedBladeCounts;
	GenerateThreadBlades(dispatch, groupIndex, emittedBladeCounts);

#if defined(HIGH_LOD) || defined(MID_LOD) || defined(LOW_LOD)
#	if defined(LOW_LOD) && !defined(FAR_LOD) && SLOPE_EXTRA_BLADES > 0
	GroupMemoryBarrierWithGroupSync();
	uint extraTaskCount = LowActiveCount * SLOPE_EXTRA_BLADES;
	[loop] for (uint extraTask = groupIndex; extraTask < extraTaskCount; extraTask += THREADGROUP_SIZE)
		GenerateLowExtra(dispatch, extraTask);
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		if (LowInnerCount != 0u)
			IndirectArgs.InterlockedAdd(4u, LowInnerCount, GroupOutputBase.x);
		if (LowOuterCount != 0u) {
			uint oldOuterEnd;
			IndirectArgs.InterlockedAdd(24u, LowOuterCount, oldOuterEnd);
			IndirectArgs.InterlockedAdd(36u, 0u - LowOuterCount, oldOuterEnd);
			GroupOutputBase.y = oldOuterEnd - LowOuterCount;
		}
	}

	GroupMemoryBarrierWithGroupSync();
	[loop] for (uint innerSlot = groupIndex; innerSlot < LowInnerCount; innerSlot += THREADGROUP_SIZE)
		BladeOutput[GroupOutputBase.x + innerSlot] = GroupBlades[innerSlot];
	[loop] for (uint outerSlot = groupIndex; outerSlot < LowOuterCount; outerSlot += THREADGROUP_SIZE)
	{
		BladeOutput[GroupOutputBase.y + outerSlot] = GroupBlades[THREADGROUP_SIZE * (1 + SLOPE_EXTRA_BLADES) - 1u - outerSlot];
	}
#	else
	if (groupIndex == 0u) {
		GroupInnerCount = 0u;
#		if defined(HIGH_GEOMETRY_LOD)
		GroupOuterCount = 0u;
#		endif
		GroupOutputBase = uint2(0u, 0u);
	}

	GroupMemoryBarrierWithGroupSync();

	uint2 threadOutputOffset = uint2(0u, 0u);
	if (emittedBladeCounts.x != 0u)
		InterlockedAdd(GroupInnerCount, emittedBladeCounts.x, threadOutputOffset.x);
#		if defined(HIGH_GEOMETRY_LOD)
	if (emittedBladeCounts.y != 0u)
		InterlockedAdd(GroupOuterCount, emittedBladeCounts.y, threadOutputOffset.y);
#		endif

	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		if (GroupInnerCount != 0u)
			IndirectArgs.InterlockedAdd(4u, GroupInnerCount, GroupOutputBase.x);
#		if defined(HIGH_GEOMETRY_LOD)
		if (GroupOuterCount != 0u) {
			uint oldOuterEnd;
			IndirectArgs.InterlockedAdd(24u, GroupOuterCount, oldOuterEnd);
			IndirectArgs.InterlockedAdd(36u, 0u - GroupOuterCount, oldOuterEnd);
			GroupOutputBase.y = oldOuterEnd - GroupOuterCount;
		}
#		endif
	}

	GroupMemoryBarrierWithGroupSync();

#		if defined(HIGH_GEOMETRY_LOD)
	uint2 categoryOffset = 0u;
#		endif
	uint emittedBladeCount = emittedBladeCounts.x + emittedBladeCounts.y;
	[loop] for (uint i = 0u; i < emittedBladeCount; ++i)
	{
#		if defined(HIGH_GEOMETRY_LOD)
		uint outer = GroupBladeOuter[i * THREADGROUP_SIZE + groupIndex];
		uint outputIndex;
		if (outer != 0u)
			outputIndex = GroupOutputBase.y + threadOutputOffset.y + categoryOffset.y++;
		else
			outputIndex = GroupOutputBase.x + threadOutputOffset.x + categoryOffset.x++;
#		else
		uint outputIndex = GroupOutputBase.x + threadOutputOffset.x + i;
#		endif
		BladeOutput[outputIndex] = GroupBlades[i * THREADGROUP_SIZE + groupIndex];
	}
#	endif
#endif
}
