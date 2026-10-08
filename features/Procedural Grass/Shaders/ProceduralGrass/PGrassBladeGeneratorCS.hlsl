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

// A second list at the buffer's tail holds simpler near-tier blades or Far's double-blade handoff fill.
#if defined(HIGH_GEOMETRY_LOD) || defined(MID_OUTER_GEOMETRY) || defined(LOW_OUTER_GEOMETRY) || defined(FAR_DOUBLE_GEOMETRY)
#	define PGRASS_OUTER_LIST
#endif

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
	float4 lodFadeIn;  // x: fade-in start, y: inverse range, z: unused, w: fade-out endpoint
	float4 lodFadeOut;
	QuadrantData data[QUADRANT_DATA_SIZE];
}

RWStructuredBuffer<Blade> BladeOutput : register(u0);
RWByteAddressBuffer IndirectArgs : register(u1);

// Far's handoff fill stages its candidates in rounds, so its group-shared staging does not limit occupancy.
#define FAR_STAGED_CANDIDATES 4
#if defined(FAR_LOD) && SLOPE_EXTRA_BLADES + 1 > FAR_STAGED_CANDIDATES
#	define PGRASS_STAGED_ROUNDS
static const uint MAX_BLADES_PER_THREAD = FAR_STAGED_CANDIDATES;
#else
static const uint MAX_BLADES_PER_THREAD = 1u + (SLOPE_EXTRA_BLADES + PATCH_BLADE_COUNT - 1u) / PATCH_BLADE_COUNT;
#endif

#if defined(HIGH_LOD) || defined(MID_LOD) || defined(LOW_LOD)
// Adjacent lanes occupy adjacent slots for each emission, reducing shared-memory bank conflicts.
groupshared Blade GroupBlades[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
#	if defined(PGRASS_OUTER_LIST)
groupshared uint GroupBladeOuter[THREADGROUP_SIZE * MAX_BLADES_PER_THREAD];
groupshared uint GroupOuterCount;
#	endif
groupshared uint GroupInnerCount;
groupshared uint2 GroupOutputBase;
#	if defined(PGRASS_STAGED_ROUNDS)
groupshared uint GroupCandidateRounds;
#	endif
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
// Terrain and object surfaces dispatch separate ranges of the same work list.
cbuffer GrassWorkRange : register(b0)
{
	uint workItemOffset;
	float workShare;  // Share of each compact work item's patches that this dispatch generates.
}

uint LoadBladeTask(uint taskIndex)
{
	taskIndex += workItemOffset;
	return VisibleBladeTasks[taskIndex];
}

StructuredBuffer<uint> QuadrantGrassCells : register(t6);  // Packed 2x2 LAND IDs for each quadrant cell.
StructuredBuffer<float2> TileHeightBounds : register(t11);
StructuredBuffer<uint> OccupancyRows : register(t12);

#if defined(HIGH_LOD)
Texture2D<uint> GrassDensityTexture : register(t7);
#endif

Texture2D<float> GrassHiZ : register(t8);  // Shared current-frame scene-depth pyramid.
#if defined(FAR_LOD)
#	include "ProceduralGrass/PGrassCanopy.hlsli"
#endif
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
static const uint WORK_OBJECT_SURFACE = 1u << 18u;
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

#if defined(FAR_LOD)
/**
 * @brief Returns a keyed pseudo-random permutation of index within [0, count), for count <= 2^20.
 * A four-round Feistel network permutes the next even power of two; indices it maps past count walk on until they land
 * inside it, which keeps the mapping one-to-one.
 */
uint PermutePatchIndex(uint index, uint count, uint key, bool inverse)
{
	uint halfBits = (firstbithigh(max(count - 1u, 1u)) + 2u) >> 1u;
	uint halfMask = (1u << halfBits) - 1u;
	uint permuted = index;
	[loop] for (uint walk = 0u; walk < 64u; ++walk)
	{
		uint left = permuted >> halfBits;
		uint right = permuted & halfMask;
		[unroll] for (uint step = 0u; step < 4u; ++step)
		{
			// The inverse runs the same rounds backwards, undoing each one.
			uint round = inverse ? 3u - step : step;
			uint kept = inverse ? left : right;
			uint mixed = (inverse ? right : left) ^ (Random::pcg2d(uint2(kept + round * 0x9E3779B9u, key)).x & halfMask);
			left = inverse ? mixed : kept;
			right = inverse ? kept : mixed;
		}
		permuted = left << halfBits | right;
		if (permuted < count)
			return permuted;
	}
	return index;
}

/** @brief The patches a Far work item draws from: its occupancy tile, or the whole quadrant, and the key that orders them. */
struct SharedPatchDomain
{
	uint2 patchStart;
	uint rowLength;
	uint patchCount;
	uint key;
};

SharedPatchDomain GetSharedPatchDomain(uint bladeTask, uint quadrantHash)
{
	SharedPatchDomain domain;
	domain.patchStart = 0u;
	domain.rowLength = PATCHES_PER_ROW;
	domain.patchCount = PATCHES_PER_QUADRANT;
	uint tile = (bladeTask >> WORK_TILE_SHIFT) & WORK_TILE_MASK;
	if ((bladeTask & WORK_OCCUPIED_TILE) != 0u) {
		uint2 tilePos = uint2(tile % OCCUPANCY_TILES_PER_AXIS, tile / OCCUPANCY_TILES_PER_AXIS);
		domain.patchStart = tilePos * uint2(PATCHES_PER_ROW, PATCH_ROWS) / OCCUPANCY_TILES_PER_AXIS;
		uint2 patchSize = (tilePos + 1u) * uint2(PATCHES_PER_ROW, PATCH_ROWS) / OCCUPANCY_TILES_PER_AXIS - domain.patchStart;
		domain.rowLength = patchSize.x;
		domain.patchCount = patchSize.x * patchSize.y;
	}
	domain.key = quadrantHash ^ (tile * 0x85EBCA6Bu);
	return domain;
}

/**
 * @brief Maps a compact work item's task index to one of the workShare of its patches, spread over the tile or quadrant.
 * A random permutation keeps the patches evenly spread without the lines a regular stride draws across the rows, and a
 * larger share keeps every patch of a smaller one. The task index is the patch's rank in that order. Returns false past
 * the share.
 */
bool ResolveSharedPatch(SharedPatchDomain domain, inout uint patch)
{
	if (domain.patchCount == 0u || patch >= max(1u, (uint)ceil(float(domain.patchCount) * workShare)))
		return false;

	uint local = PermutePatchIndex(patch, domain.patchCount, domain.key, false);
	patch = (domain.patchStart.y + local / domain.rowLength) * PATCHES_PER_ROW + domain.patchStart.x + local % domain.rowLength;
	return patch < PATCHES_PER_QUADRANT;
}

/** @brief Returns a resolved patch's rank in its work item's permutation: the task index a compact item gives it. */
uint GetSharedPatchRank(SharedPatchDomain domain, uint patch)
{
	uint2 local = uint2(patch % PATCHES_PER_ROW, patch / PATCHES_PER_ROW) - domain.patchStart;
	return PermutePatchIndex(local.y * domain.rowLength + local.x, domain.patchCount, domain.key, true);
}
#endif

#include "ProceduralGrass/PGrassCulling.hlsli"

#include "ProceduralGrass/PGrassPlacement.hlsli"

// Blade construction and generation build on placement.
#include "ProceduralGrass/PGrassBladeConstruction.hlsli"
#include "ProceduralGrass/PGrassGeneration.hlsli"

#if defined(HIGH_LOD) || defined(MID_LOD) || defined(LOW_LOD)
// Writes the group's staged blades with one output reservation per list. Every thread of the group must call it.
void FlushGroupBlades(uint groupIndex, uint2 emittedBladeCounts)
{
	if (groupIndex == 0u) {
		GroupInnerCount = 0u;
#	if defined(PGRASS_OUTER_LIST)
		GroupOuterCount = 0u;
#	endif
		GroupOutputBase = uint2(0u, 0u);
	}

	GroupMemoryBarrierWithGroupSync();

	uint2 threadOutputOffset = uint2(0u, 0u);
	if (emittedBladeCounts.x != 0u)
		InterlockedAdd(GroupInnerCount, emittedBladeCounts.x, threadOutputOffset.x);
#	if defined(PGRASS_OUTER_LIST)
	if (emittedBladeCounts.y != 0u)
		InterlockedAdd(GroupOuterCount, emittedBladeCounts.y, threadOutputOffset.y);
#	endif

	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0u) {
		if (GroupInnerCount != 0u)
			IndirectArgs.InterlockedAdd(4u, GroupInnerCount, GroupOutputBase.x);
#	if defined(PGRASS_OUTER_LIST)
		if (GroupOuterCount != 0u) {
			uint oldOuterEnd;
			IndirectArgs.InterlockedAdd(24u, GroupOuterCount, oldOuterEnd);
			IndirectArgs.InterlockedAdd(36u, 0u - GroupOuterCount, oldOuterEnd);
			GroupOutputBase.y = oldOuterEnd - GroupOuterCount;
		}
#	endif
	}

	GroupMemoryBarrierWithGroupSync();

#	if defined(PGRASS_OUTER_LIST)
	uint2 categoryOffset = 0u;
#	endif
	uint emittedBladeCount = emittedBladeCounts.x + emittedBladeCounts.y;
	[loop] for (uint i = 0u; i < emittedBladeCount; ++i)
	{
#	if defined(PGRASS_OUTER_LIST)
		uint outer = GroupBladeOuter[i * THREADGROUP_SIZE + groupIndex];
		uint outputIndex;
		if (outer != 0u)
			outputIndex = GroupOutputBase.y + threadOutputOffset.y + categoryOffset.y++;
		else
			outputIndex = GroupOutputBase.x + threadOutputOffset.x + categoryOffset.x++;
#	else
		uint outputIndex = GroupOutputBase.x + threadOutputOffset.x + i;
#	endif
		BladeOutput[outputIndex] = GroupBlades[i * THREADGROUP_SIZE + groupIndex];
	}
}
#endif

[numthreads(TG_DIM_X, TG_DIM_Y, 1)] void main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex) {
	uint tileTask = LoadBladeTask(dispatch.z);

	if (groupIndex == 0u) {
		GroupTileOccluded = 0u;
		if (grassHiZParams.w >= 1.0f && debugFlags.x <= 0.5f && (tileTask & WORK_OBJECT_SURFACE) == 0u) {
			if ((tileTask & (WORK_OCCUPIED_TILE | WORK_HAS_LAND)) == (WORK_OCCUPIED_TILE | WORK_HAS_LAND))
				GroupTileOccluded = IsOccupiedTileOccluded(tileTask) ? 1u : 0u;
#if defined(FAR_LOD)
			else if ((tileTask & WORK_HAS_LAND) != 0u)
				GroupTileOccluded = IsFarGroupOccluded(tileTask, groupID.x) ? 1u : 0u;
#endif
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (GroupTileOccluded != 0u)
		return;

#if defined(PGRASS_STAGED_ROUNDS)
	PatchCandidates candidates;
	bool hasCandidates = PreparePatchCandidates(dispatch, candidates);

	if (groupIndex == 0u)
		GroupCandidateRounds = 0u;
	GroupMemoryBarrierWithGroupSync();
	if (hasCandidates)
		InterlockedMax(GroupCandidateRounds, (candidates.candidateCount + FAR_STAGED_CANDIDATES - 1u) / FAR_STAGED_CANDIDATES);
	GroupMemoryBarrierWithGroupSync();

	uint candidateRounds = GroupCandidateRounds;
	for (uint candidateRound = 0u; candidateRound < candidateRounds; ++candidateRound) {
		uint2 emittedBladeCounts = 0u;
		uint candidateBegin = candidateRound * FAR_STAGED_CANDIDATES;
		if (hasCandidates)
			EmitPatchCandidates(candidates, candidateBegin, min(candidateBegin + FAR_STAGED_CANDIDATES, candidates.candidateCount), groupIndex, emittedBladeCounts);
		FlushGroupBlades(groupIndex, emittedBladeCounts);

		// The next round resets the group's counters, so every thread must first finish reading this round's output base.
		GroupMemoryBarrierWithGroupSync();
	}
#else
	uint2 emittedBladeCounts;
	GenerateThreadBlades(dispatch, groupIndex, emittedBladeCounts);
#	if defined(HIGH_LOD) || defined(MID_LOD) || defined(LOW_LOD)
	FlushGroupBlades(groupIndex, emittedBladeCounts);
#	endif
#endif
}
