#ifndef __PGRASS_CULLING_HLSLI__
#define __PGRASS_CULLING_HLSLI__

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
	float rootDepth = rootClip.z / rootClip.w;

	// TAA jitter moves silhouettes and the base of a surface across the root's texel every frame. Treat the root as
	// covered only when it and its four neighbours all see a raised surface in front of it, so edge blades stay put.
	float4 scenePosition = 0.0f;
	float minGroundHeight = 1.0e30f;
	[unroll] for (uint sampleIndex = 0u; sampleIndex < 5u; ++sampleIndex)
	{
		int2 offset = int2(sampleIndex == 1u ? -1 : (sampleIndex == 2u ? 1 : 0), sampleIndex == 3u ? -1 : (sampleIndex == 4u ? 1 : 0));
		float4 samplePosition = GetHiZScenePosition(depthTexel + offset);
		if (samplePosition.w == 0.0f || GrassHiZ.Load(int3(depthTexel + offset, 0)) >= rootDepth)
			return false;
		float2 sceneRootOffset = samplePosition.xy - rootView.xy;
		float sceneGroundHeight = samplePosition.z - (rootView.z + dot(terrainSlope, sceneRootOffset));
		if (sceneGroundHeight <= 16.0f || dot(sceneRootOffset, sceneRootOffset) >= 768.0f * 768.0f)
			return false;
		minGroundHeight = min(minGroundHeight, sceneGroundHeight);
		if (sampleIndex == 0u)
			scenePosition = samplePosition;
	}
	if (minGroundHeight > 64.0f)
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
/** @brief Tests a projected blade rectangle with the common Hi-Z and silhouette margins. */
bool IsProjectedBladeOccluded(float2 ndcMin, float2 ndcMax, float minClipW)
{
	const float2 hiZSize = grassHiZParams.xy;
	float2 uvMin = float2(ndcMin.x, -ndcMax.y) * 0.5f + 0.5f - 1.0f / hiZSize;
	float2 uvMax = float2(ndcMax.x, -ndcMin.y) * 0.5f + 0.5f + 1.0f / hiZSize;
	if (any(uvMax <= 0.0f) || any(uvMin >= 1.0f))
		return false;
	uvMin = max(uvMin, 0.0f);
	uvMax = min(uvMax, 1.0f);
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

	return IsProjectedBladeOccluded(ndcMin, ndcMax, minClipW);
}

#	if defined(FAR_LOD)
/** @brief Bounds Far's packed triangle directly, retaining the same depth bias and a Hi-Z texel around its silhouette. */
bool IsFarTriangleOccluded(float3 rootView, float3 tipView, float2 facing, float halfWidth, bool cullsDisabled)
{
	if (cullsDisabled || grassHiZParams.w < 1.0f || !HasForwardPerspective())
		return false;
	float4 root = mul(FrameBuffer::CameraViewProj, float4(rootView, 1.0f));
	float4 width = mul(FrameBuffer::CameraViewProj, float4(float2(-facing.y, facing.x) * halfWidth, 0.0f, 0.0f));
	float4 left = root - width;
	float4 right = root + width;
	float4 tip = mul(FrameBuffer::CameraViewProj, float4(tipView, 1.0f));
	float minClipW = min(min(left.w, right.w), tip.w) - 8.0f * length(FrameBuffer::CameraViewProj[3].xyz);
	if (minClipW <= 1.0f)
		return false;
	float2 leftNdc = left.xy / left.w;
	float2 rightNdc = right.xy / right.w;
	float2 tipNdc = tip.xy / tip.w;
	return IsProjectedBladeOccluded(min(min(leftNdc, rightNdc), tipNdc), max(max(leftNdc, rightNdc), tipNdc), minClipW);
}
#	endif
#endif

bool ResolveTileHeightBounds(uint quadrant, uint tile, out float2 heightBounds)
{
	heightBounds = TileHeightBounds[quadrant * OCCUPANCY_TILES_PER_AXIS * OCCUPANCY_TILES_PER_AXIS + tile];
	return heightBounds.x > -1.0e30f && heightBounds.y >= heightBounds.x;
}

#if defined(FAR_LOD)
bool IsFarRootBoxReplaced(uint quadrant, float2 localMin, float2 localMax, float2 heightBounds, float geometryRadius)
{
	// Cover clumped roots, half-packed rounding and the existing terrain lift envelope.
	float rootPadding = grassHiZBounds.y + 32.0f;
	float2 worldMin = data[quadrant].quadWorldPos + localMin - rootPadding;
	float2 worldMax = data[quadrant].quadWorldPos + localMax + rootPadding;
	float lift = GetTerrainLiftReach((worldMin + worldMax) * 0.5f, max(worldMax.x - worldMin.x, worldMax.y - worldMin.y) * 0.5f);
	return IsTerrainCanopyBoxCovered(float3(worldMin, heightBounds.x - geometryRadius),
		float3(worldMax, heightBounds.y + geometryRadius + lift));
}
#endif

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

#if defined(FAR_LOD)
	if (IsFarRootBoxReplaced(quadrant, tileMin, tileMax, heightBounds, geometryRadius))
		return true;
#endif
	return IsRootBoxOccluded(quadrant, tileMin, tileMax, heightBounds, geometryRadius, 768.0f, 2.0f, debugFlags.x > 0.5f);
}

#if defined(FAR_LOD)
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
	float2 localMin = float2(minPatch) * patchWidth;
	float2 localMax = float2(maxPatch) * patchWidth;
	float geometryRadius = max(grassHiZBounds.x, 96.0f);
	if (IsFarRootBoxReplaced(quadrant, localMin, localMax, heightBounds, geometryRadius))
		return true;
	return IsRootBoxOccluded(quadrant, localMin, localMax, heightBounds, geometryRadius, 768.0f, 1.5f, false);
}
#endif

#endif
