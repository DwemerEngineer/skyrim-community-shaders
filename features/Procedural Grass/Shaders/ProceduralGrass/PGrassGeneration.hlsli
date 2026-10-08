#ifndef __PGRASS_GENERATION_HLSLI__
#define __PGRASS_GENERATION_HLSLI__

#if defined(FAR_LOD)
// Far's handoff fill and its full view-facing keep fade out over this distance past Low's band. Keep in sync with
// PGrassRenderer.h.
static const float FarHandoffFillFade = 4096.0f;

/** @brief Returns the smoothstep from Low's handoff end to FarHandoffFillFade beyond it. */
float GetFarHandoffFade(float squareDistance)
{
	float handoffEnd = lodFadeIn.x + rcp(max(lodFadeIn.y, 1.0e-6f));
	return smoothstep(handoffEnd, handoffEnd + FarHandoffFillFade, squareDistance);
}

/**
 * @brief Returns the share of candidates Far keeps for its view-facing blades. Where Far meets Low it keeps them all,
 * since Low's edge-on blades are view-thickened and cover more than their random facing alone; it eases to
 * FarViewFacingKeep as the handoff fill fades.
 */
float GetFarViewFacingKeep(float2 lodOffset)
{
	return lerp(1.0f, FarViewFacingKeep, GetFarHandoffFade(max(abs(lodOffset.x), abs(lodOffset.y))));
}
#endif

#if defined(FAR_LOD) && defined(PGRASS_FAR_HANDOFF)
/**
 * @brief Returns the fill candidates a Far patch adds beyond its slope slot to match Low's density where they meet.
 * The fill is full through Low's fade-out band and fades over the next FarHandoffFillFade units, never covering more
 * ground than the seam as Far's blades widen. Beyond it Far keeps the density its own setting gives.
 */
float GetFarHandoffFill(float2 lodOffset, float slopeKeep)
{
	float seamBlades = (1.0f + slopeKeep) * farHandoffDensityRatio;
	float fade = 1.0f - GetFarHandoffFade(max(abs(lodOffset.x), abs(lodOffset.y)));
	float coverageBlades = seamBlades * (DistantWidthScale / GetFarWidthScale(lodOffset));
	return max(min(seamBlades * fade, coverageBlades) - 1.0f - slopeKeep, 0.0f);
}
#endif

// One thread's patch after its LOD, grass-map, terrain, and occlusion tests, ready to emit its candidates.
struct PatchCandidates
{
	uint2 patchPos;
	uint quadrant;
	uint quadrantHash;
	float2 quadWorldPos;
	uint bladeIndex;
	// Flags are 0 or 1: bool members here compiled wrongly at /O3 on AMD, adding blades to Far's compact quadrants.
	uint hasLand;
	uint objectSurface;
	uint insideFrustum;
	uint cullsDisabled;
	uint useBasePath;
	uint3 baseHash;
	float2 baseWorldPos2D;
	float2 baseMapSamplePos;
	uint baseGrassCell;
	float2 terrainSlope;
	float baseWorldZ;
	float terrainNormalZ;
	float extraKeep;  // Near tiers: the slope fill's keep share. Far: the extra candidates before its LOD keep.
	uint candidateCount;
#if defined(FAR_LOD)
	float farLODKeep;
	float2 farPatchRank;  // x: the patch's rank in its work item's permutation, y: 1 / the item's patch count
#endif
};

/** @brief Resolves a thread's patch and its shared terrain plane. Returns false when the patch emits nothing. */
bool PreparePatchCandidates(uint3 dispatch, out PatchCandidates candidates)
{
	candidates = (PatchCandidates)0;

	uint patch = dispatch.x;
	uint bladeTask = LoadBladeTask(dispatch.z);
	uint bladeIndex = (bladeTask >> WORK_LANE_SHIFT) & 0xFu;
	uint quadrant = bladeTask & WORK_QUADRANT_MASK;

	bool hasLand = (bladeTask & WORK_HAS_LAND) != 0u;
	bool objectSurface = (bladeTask & WORK_OBJECT_SURFACE) != 0u;
	bool insideFrustum = (bladeTask & WORK_INSIDE_FRUSTUM) != 0u;
	bool nearCovered = (bladeTask & WORK_NEAR_COVERED) != 0u;
	bool compactFar = (bladeTask & WORK_COMPACT_FAR) != 0u;

	// Full-quadrant dispatches round up to whole groups, so also reject the tail past the last patch. Far's compact work
	// generates only its share of the tile's or quadrant's patches, spread over them.
#if defined(FAR_LOD)
	SharedPatchDomain patchDomain = GetSharedPatchDomain(bladeTask, data[quadrant].quadrantHash);
	uint patchRank = patch;
	bool patchResolved = compactFar ? ResolveSharedPatch(patchDomain, patch) : ResolveTilePatch(bladeTask, patch);
	// Full items keep their contiguous groups for group culling and recover the rank a compact item would give the patch.
	if (patchResolved && !compactFar)
		patchRank = GetSharedPatchRank(patchDomain, patch);
#else
	bool patchResolved = ResolveTilePatch(bladeTask, patch);
#endif
	if (!patchResolved)
		return false;

	bool cullsDisabled = debugFlags.x > 0.5f;
	QuadrantData quadrantData = data[quadrant];
	uint2 patchPos = uint2(patch % PATCHES_PER_ROW, patch / PATCHES_PER_ROW);
	uint quadrantHash = quadrantData.quadrantHash;
	if (!cullsDisabled && (bladeTask & WORK_FULL_GRASS) == 0u && !PatchHasGrass(patchPos, quadrant))
		return false;

	// Preserve the base blade slot's position and seed.
	uint3 baseHash;
	uint2 gridPos = BaseGridPosition(patchPos, bladeIndex);
	baseHash = Random::pcg3d(uint3(gridPos, quadrantHash));
	float2 baseQuadPos2D = BaseQuadrantPosition(gridPos, baseHash);
	float2 baseWorldPos2D = baseQuadPos2D + quadrantData.quadWorldPos;
	float2 baseMapSamplePos = GrassMapSamplePos(baseQuadPos2D, baseWorldPos2D, baseHash);
	uint baseGrassCell = 0u;
#if defined(FAR_LOD)
	// Every candidate dithers against Far's LOD fades and view-facing share by its patch's rank in the item's permutation.
	// A compact item generates the first ranks, and the CPU bounds its share above every keep in it, so a share change
	// never changes which candidates survive; moving the camera only dissolves them one at a time as their keep changes.
	float farLODKeep = cullsDisabled ? 1.0f : GetFarLODKeep(baseWorldPos2D, nearCovered) * GetFarViewFacingKeep(baseWorldPos2D - grassLodOrigin);
	float inversePatchCount = rcp(float(max(patchDomain.patchCount, 1u)));
	if (farLODKeep <= float(patchRank) * inversePatchCount)
		return false;
	bool useBasePath = farLODKeep >= 1.0f || (float(patchRank) + LodDither(baseWorldPos2D)) * inversePatchCount <= farLODKeep;
#else
	bool useBasePath = true;
#endif
#if !defined(LOW_LOD) && !defined(FAR_LOD)
	if (!cullsDisabled) {
		float2 baseLodXY = baseWorldPos2D - grassLodOrigin;
		float baseDistSq = dot(baseLodXY, baseLodXY);
		float baseCullDist = lodFadeIn.w;
		if (baseDistSq >= baseCullDist * baseCullDist)
			useBasePath = false;
	}
#endif

	if (useBasePath && !objectSurface) {
		baseGrassCell = LoadGrassCell(baseMapSamplePos, quadrant);

		if (!cullsDisabled && baseGrassCell == 0u)
			useBasePath = false;
	}

#if SLOPE_EXTRA_BLADES > 0
	if (!useBasePath && bladeIndex >= SLOPE_EXTRA_BLADES)
		return false;
#else
	if (!useBasePath)
		return false;
#endif

	// Share the sampled surface plane when deciding how many slope extras to generate.
	float2 terrainSlope;
	float baseWorldZ;
	if (objectSurface) {
		uint surfaceType;
		float surfaceDensity;
		LoadGrassObjectSurface(baseWorldPos2D, baseWorldZ, terrainSlope, surfaceType, surfaceDensity);
	} else {
		baseWorldZ = TerrainHeightSlopeAt(terrainSlope, baseWorldPos2D, quadrantData.quadWorldPos, quadrant, hasLand);
	}
	if (!objectSurface && useBasePath && IsPatchOccluded(baseWorldPos2D, baseWorldZ, terrainSlope, quadrant, hasLand, cullsDisabled))
		return false;

	candidates.patchPos = patchPos;
	candidates.quadrant = quadrant;
	candidates.quadrantHash = quadrantHash;
	candidates.quadWorldPos = quadrantData.quadWorldPos;
	candidates.bladeIndex = bladeIndex;
	candidates.hasLand = hasLand ? 1u : 0u;
	candidates.objectSurface = objectSurface ? 1u : 0u;
	candidates.insideFrustum = insideFrustum ? 1u : 0u;
	candidates.cullsDisabled = cullsDisabled ? 1u : 0u;
	candidates.useBasePath = useBasePath ? 1u : 0u;
	candidates.baseHash = baseHash;
	candidates.baseWorldPos2D = baseWorldPos2D;
	candidates.baseMapSamplePos = baseMapSamplePos;
	candidates.baseGrassCell = baseGrassCell;
	candidates.terrainSlope = terrainSlope;
	candidates.baseWorldZ = baseWorldZ;
	candidates.terrainNormalZ = rsqrt(dot(terrainSlope, terrainSlope) + 1.0f);
	candidates.candidateCount = 1u + SLOPE_EXTRA_BLADES;

#if SLOPE_EXTRA_BLADES > 0
	// Reject slope extras before grass typing, clumping, LOD, occlusion, wind, and packing.
	float baseSlopeKeep = GetSlopeFillKeep(candidates.terrainNormalZ);
	candidates.extraKeep = baseSlopeKeep;
#	if defined(FAR_LOD)
	// Far's slope slot and, near Low, its handoff fill share one count: slot k is kept for its share of what remains.
#		if defined(PGRASS_FAR_HANDOFF)
	candidates.extraKeep += GetFarHandoffFill(baseWorldPos2D - grassLodOrigin, baseSlopeKeep);
#			if defined(FAR_DOUBLE_GEOMETRY)
	// Each handoff extra draws a double blade, so half as many records give the same blades.
	candidates.extraKeep *= 0.5f;
#			endif
#		endif
	if (!cullsDisabled)
		candidates.candidateCount = min(candidates.candidateCount, 1u + uint(ceil(candidates.extraKeep)));
	candidates.farLODKeep = farLODKeep;
	candidates.farPatchRank = float2(patchRank, inversePatchCount);
#	endif
#endif
	return true;
}

/** @brief Builds one candidate: the patch's base blade at index 0, or one of its extras. Returns false when it is rejected. */
bool BuildPatchCandidate(PatchCandidates candidates, uint candidateIndex, out Blade blade, out bool outerGeometry)
{
	blade = (Blade)0;
	outerGeometry = false;

	bool isBase = candidateIndex == 0;
	uint3 candidateHash = candidates.baseHash;
	float2 candidateWorldPos = candidates.baseWorldPos2D;
	float2 candidateMapSamplePos = candidates.baseMapSamplePos;
	uint packedGrassCell = candidates.baseGrassCell;
	float candidateWorldZ = candidates.baseWorldZ;
	bool cullsDisabled = candidates.cullsDisabled != 0u;

	if (isBase) {
		if (candidates.useBasePath == 0u)
			return false;
	} else {
#if SLOPE_EXTRA_BLADES > 0
		uint emitExtraIndex = candidateIndex - 1;
		if ((emitExtraIndex % PATCH_BLADE_COUNT) != candidates.bladeIndex)
			return false;

		candidateHash = ExtraCandidateHash(candidates.patchPos, emitExtraIndex, candidates.quadrantHash);
#	if defined(FAR_LOD)
		// Separate hashes keep the retained slots' grass types and placement unbiased.
		uint3 keepHash = Random::pcg3d(candidateHash ^ uint3(0x9E3779B9u, 0x7F4A7C15u, 0x94D049BBu));
		float keepRoll = float(keepHash.x) * UINT_TO_FLOAT;
		float rankRoll = (candidates.farPatchRank.x + float(keepHash.y) * UINT_TO_FLOAT) * candidates.farPatchRank.y;
		if (!cullsDisabled && (keepRoll >= candidates.extraKeep - float(emitExtraIndex) || rankRoll > candidates.farLODKeep))
			return false;
#	else
		float slopeRoll = float(candidateHash.z) * UINT_TO_FLOAT;
		if (!cullsDisabled && slopeRoll > candidates.extraKeep)
			return false;
#	endif

		float2 candidateQuadPos = ExtraCandidateQuadPos(candidates.patchPos, candidateHash);
		candidateWorldPos = candidateQuadPos + candidates.quadWorldPos;
		candidateMapSamplePos = GrassMapSamplePos(candidateQuadPos, candidateWorldPos, candidateHash);
		if (candidates.objectSurface == 0u) {
			packedGrassCell = LoadGrassCell(candidateMapSamplePos, candidates.quadrant);
			if (!cullsDisabled && packedGrassCell == 0u)
				return false;
		}
		candidateWorldZ = candidates.baseWorldZ + dot(candidates.terrainSlope, candidateWorldPos - candidates.baseWorldPos2D);
#else
		return false;
#endif
	}

	return BuildBlade(candidateHash, candidateMapSamplePos, candidateWorldPos, candidateWorldZ, candidates.terrainSlope, candidates.terrainNormalZ,
		candidates.quadWorldPos, candidates.quadrant, candidates.hasLand != 0u, packedGrassCell, cullsDisabled, candidates.insideFrustum != 0u, isBase, candidates.objectSurface != 0u, blade, outerGeometry);
}

// Stages a built blade in this thread's next group slot.
void StageBlade(Blade blade, bool outerGeometry, uint groupIndex, inout uint2 emittedBladeCounts)
{
	uint slot = (emittedBladeCounts.x + emittedBladeCounts.y) * THREADGROUP_SIZE + groupIndex;
	GroupBlades[slot] = blade;
#if defined(PGRASS_OUTER_LIST)
	GroupBladeOuter[slot] = outerGeometry ? 1u : 0u;
#endif

	if (outerGeometry)
		emittedBladeCounts.y++;
	else
		emittedBladeCounts.x++;
}

#if defined(PGRASS_STAGED_ROUNDS)
/** @brief Stages candidates [candidateBegin, candidateEnd), at most MAX_BLADES_PER_THREAD of them. */
void EmitPatchCandidates(PatchCandidates candidates, uint candidateBegin, uint candidateEnd, uint groupIndex, inout uint2 emittedBladeCounts)
{
	for (uint candidateIndex = candidateBegin; candidateIndex < candidateEnd; ++candidateIndex) {
		Blade blade;
		bool outerGeometry;
		if (BuildPatchCandidate(candidates, candidateIndex, blade, outerGeometry))
			StageBlade(blade, outerGeometry, groupIndex, emittedBladeCounts);
	}
}
#endif

void GenerateThreadBlades(uint3 dispatch, uint groupIndex, out uint2 emittedBladeCounts)
{
	emittedBladeCounts = 0u;

	PatchCandidates candidates;
	if (!PreparePatchCandidates(dispatch, candidates))
		return;

#if defined(FAR_LOD)
	for (uint candidateIndex = 0; candidateIndex < candidates.candidateCount; ++candidateIndex) {
#else
	// Keep one emit path, unrolled: AMD's driver has miscompiled the loop form of the Mid generator, passing the extra
	// candidate a wrong position so that it emitted no blades.
	[unroll] for (uint candidateIndex = 0; candidateIndex < 1u + SLOPE_EXTRA_BLADES; ++candidateIndex)
	{
#endif
		Blade blade;
		bool outerGeometry;
		if (BuildPatchCandidate(candidates, candidateIndex, blade, outerGeometry))
			StageBlade(blade, outerGeometry, groupIndex, emittedBladeCounts);
	}
}

#endif
