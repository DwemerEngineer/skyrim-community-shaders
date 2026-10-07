#ifndef __PGRASS_CANOPY_HLSLI__
#define __PGRASS_CANOPY_HLSLI__

// The terrain canopy map caches the grass type of each LAND sample around the player for Far and terrain darkening.
Texture2D<uint> TerrainCanopyTypes : register(t64);

bool LoadTerrainCanopySample(int2 sample, out uint type)
{
	type = 0u;
	int2 quadrant = sample >> 4;
	if (any(quadrant < terrainCanopyWindow.xy) || any(quadrant >= terrainCanopyWindow.zw))
		return false;

	uint packed = TerrainCanopyTypes.Load(int3(sample & 1023, 0));
	uint2 tagCoordinates = asuint(quadrant) & 2047u;
	uint tag = (1u << 30u) | (tagCoordinates.x << 8u) | (tagCoordinates.y << 19u);
	if ((packed & 0x7FFFFF00u) != tag)
		return false;

	type = packed & 0xFFu;
	return true;
}

/** @brief Reads a world-stable material mixture only when all four cache samples are valid. */
bool LoadTerrainCanopyTypes(float2 worldPosition, out uint4 types, out float4 weights)
{
	float2 grid = worldPosition * (1.0f / 128.0f);
	int2 base = int2(floor(grid));
	float2 blend = frac(grid);
	weights = float4((1.0f - blend.x) * (1.0f - blend.y), blend.x * (1.0f - blend.y),
		(1.0f - blend.x) * blend.y, blend.x * blend.y);
	types = 0u;

	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		int2 sample = base + int2(i & 1u, i >> 1u);
		uint type;
		if (!LoadTerrainCanopySample(sample, type))
			return false;
		types[i] = type;
	}
	return true;
}

#if defined(FAR_LOD)
// Far helpers below read the scene depth pyramid, GrassHiZ.
float GetTerrainCanopyDistanceBlend(float2 worldPosition)
{
	if (terrainCanopyParams.y <= 0.0f)
		return 0.0f;
	float2 offset = abs(worldPosition - grassLodOrigin);
	return smoothstep(0.0f, 1.0f, (max(offset.x, offset.y) - terrainCanopyParams.x) * terrainCanopyParams.y);
}

/** @brief Keeps blade geometry where the current scene depth crosses a silhouette or another surface. */
float GetTerrainCanopyInterior(float3 cameraPosition)
{
	if (grassHiZParams.w < 1.0f)
		return 0.0f;
	float4 clipPosition = mul(FrameBuffer::CameraViewProj, float4(cameraPosition, 1.0f));
	if (clipPosition.w <= 1.0f)
		return 0.0f;
	float2 uv = clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f;
	int2 pixel = int2(floor(uv * grassHiZParams.xy));
	int step = (int)ceil(terrainCanopyParams.z * cameraViewRow1Sum * (0.5f * grassHiZParams.y) / clipPosition.w) + 2;
	if (any(pixel - step < 0) || any(pixel + step >= int2(grassHiZParams.xy)))
		return 0.0f;

	float centreDepth = GrassHiZ.Load(int3(pixel, 0));
	if (centreDepth >= 1.0f)
		return 0.0f;
	float inverseDepth = rcp(SharedData::GetScreenDepth(centreDepth));
	float4 depths = float4(
		GrassHiZ.Load(int3(pixel + int2(-step, 0), 0)),
		GrassHiZ.Load(int3(pixel + int2(step, 0), 0)),
		GrassHiZ.Load(int3(pixel + int2(0, -step), 0)),
		GrassHiZ.Load(int3(pixel + int2(0, step), 0)));
	if (any(depths >= 1.0f))
		return 0.0f;
	float4 neighbours = rcp(SharedData::GetScreenDepths(depths));
	// Inverse depth is affine across a planar surface, including steep grazing terrain.
	// Opposite samples cancel that slope; residual curvature retains blades at surface breaks.
	float2 residual = abs(float2(neighbours.x + neighbours.y, neighbours.z + neighbours.w) * 0.5f - inverseDepth);
	float curvature = max(residual.x, residual.y) / max(inverseDepth, 1.0e-12f);
	return 1.0f - smoothstep(0.01f, 0.03f, curvature);
}

/** @brief Retires a whole root box only beyond the fade and within fully cached, continuous scene depth. */
bool IsTerrainCanopyBoxCovered(float3 worldMin, float3 worldMax)
{
	if (terrainCanopyParams.y <= 0.0f || grassHiZParams.w < 1.0f)
		return false;
	float2 nearest = clamp(grassLodOrigin, worldMin.xy, worldMax.xy) - grassLodOrigin;
	float fadeEnd = terrainCanopyParams.x + rcp(terrainCanopyParams.y);
	if (max(abs(nearest.x), abs(nearest.y)) < fadeEnd)
		return false;

	int2 firstSample = int2(floor(worldMin.xy * (1.0f / 128.0f)));
	int2 lastSample = int2(floor(worldMax.xy * (1.0f / 128.0f))) + 1;
	int2 sampleCount = lastSample - firstSample + 1;
	// Larger boxes retain their per-blade checks instead of scanning an unbounded region.
	if (sampleCount.x * sampleCount.y > 64)
		return false;

	float2 uvMin = 1.0f;
	float2 uvMax = 0.0f;
	float minClipW = 1.0e30f;
	[unroll] for (uint corner = 0u; corner < 8u; ++corner)
	{
		float3 position = lerp(worldMin, worldMax, float3(corner & 1u, (corner >> 1u) & 1u, corner >> 2u));
		float4 clip = mul(FrameBuffer::CameraViewProj, float4(position - FrameBuffer::CameraPosAdjust.xyz, 1.0f));
		if (clip.w <= 1.0f)
			return false;
		float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
		uvMin = min(uvMin, uv);
		uvMax = max(uvMax, uv);
		minClipW = min(minClipW, clip.w);
	}

	int padding = (int)ceil(terrainCanopyParams.z * cameraViewRow1Sum * (0.5f * grassHiZParams.y) / minClipW) + 3;
	int2 pixelMin = int2(floor(uvMin * grassHiZParams.xy)) - padding;
	int2 pixelMax = int2(floor(uvMax * grassHiZParams.xy)) + padding;
	if (any(pixelMin < 0) || any(pixelMax >= int2(grassHiZParams.xy)))
		return false;
	int2 pixelCount = pixelMax - pixelMin + 1;
	if (pixelCount.x * pixelCount.y > 256)
		return false;

	float minDepth = 1.0e30f;
	float maxDepth = 0.0f;
	[loop] for (int y = pixelMin.y; y <= pixelMax.y; ++y)
	{
		[loop] for (int x = pixelMin.x; x <= pixelMax.x; ++x)
		{
			float depth = GrassHiZ.Load(int3(x, y, 0));
			if (depth >= 1.0f)
				return false;
			float viewDepth = SharedData::GetScreenDepth(depth);
			minDepth = min(minDepth, viewDepth);
			maxDepth = max(maxDepth, viewDepth);
			if (maxDepth > minDepth * 1.02f)
				return false;
		}
	}

	[loop] for (int sampleY = firstSample.y; sampleY <= lastSample.y; ++sampleY)
	{
		[loop] for (int sampleX = firstSample.x; sampleX <= lastSample.x; ++sampleX)
		{
			uint type;
			if (!LoadTerrainCanopySample(int2(sampleX, sampleY), type) || type == 0u)
				return false;
		}
	}
	return true;
}
#endif

#endif
