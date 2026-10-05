#ifndef __PGRASS_GENERATION_HLSLI__
#define __PGRASS_GENERATION_HLSLI__

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
	// Extra candidates perform their own grass checks; skip a redundant pre-scan.
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

#endif
