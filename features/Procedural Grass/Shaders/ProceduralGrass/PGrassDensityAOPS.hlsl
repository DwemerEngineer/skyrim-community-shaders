// Darkens resolved lighting under the grass canopy, fading out above the terrain. Coverage follows the generator's own
// grass-map, edge and object tests, so the darkening matches the grass it sits under.

#define FRAMEBUFFER
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"

#include "ProceduralGrass/PGrassCommon.hlsli"

// These read the windows and constants PGrassCommon declares.
#include "ProceduralGrass/PGrassCanopy.hlsli"
#include "ProceduralGrass/PGrassCoverage.hlsli"

Texture2D<float> DepthTexture : register(t0);
Texture2D<uint> GrassPresenceTexture : register(t1);  // Filled, slope-limited grass ids around the player; zero is bare.
Texture2D<float> RenderedDepthTexture : register(t3);
Texture2D<float> TerrainHeightTexture : register(t5);
SamplerState LinearSampler : register(s0);

#if defined(PGRASS_DARKENING_COPY)
Texture2D<float4> SceneLighting : register(t6);
#else
RWTexture2D<float4> SceneLighting : register(u0);
#endif

/** @brief Holds terrain darkening through Far's range and retires it over the last 4096 units before Far ends. */
float GetTerrainDarkeningOuterFade(float distance)
{
	float fadeEnd = farParams.x + rcp(max(farParams.y, 1.0e-6f));
	float fadeStart = max(farParams.x, fadeEnd - 4096.0f);
	float t = saturate((distance - fadeStart) / max(fadeEnd - fadeStart, 1.0f));
	return 1.0f - t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/**
 * @brief Darkens terrain lighting by the darkness setting, scaled by the grass weight. The setting is perceptual: under
 * full grass the terrain shows at (1 - darkness) of its brightness, whatever the lighting.
 */
float3 ApplyGrassTerrainDarkening(float3 lighting, float weight)
{
	float perceivedBrightness = 1.0f - saturate(grassAOParams.y) * saturate(weight);
	return lighting * pow(perceivedBrightness, 2.2f);
}

/** @brief Reads the four presence samples around a position, or returns false outside the presence window. */
bool LoadGrassPresenceIds(float2 worldPosition, out uint4 ids, out float4 weights)
{
	float2 grid = (worldPosition - grassPresenceParams.xy) * grassPresenceParams.z;
	int2 base = int2(floor(grid));
	float2 blend = grid - float2(base);
	weights = float4((1.0f - blend.x) * (1.0f - blend.y), blend.x * (1.0f - blend.y), (1.0f - blend.x) * blend.y, blend.x * blend.y);
	ids = 0u;
	if (any(base < 0) || any(base + 1 >= int(grassPresenceParams.w)))
		return false;

	ids = uint4(GrassPresenceTexture.Load(int3(base, 0)), GrassPresenceTexture.Load(int3(base + int2(1, 0), 0)),
		GrassPresenceTexture.Load(int3(base + int2(0, 1), 0)), GrassPresenceTexture.Load(int3(base + int2(1, 1), 0)));
	return true;
}

/**
 * @brief Returns the share of blades the generator's object test keeps, tent-filtered over 4x4 occlusion map texels so
 * the darkening fades over about two texels at object edges instead of stepping texel by texel.
 */
float GetObjectCoverage(float3 rootPosition)
{
	float dim = float(occlusionMapDim);
	float2 texel = ((rootPosition.xy - occlusionParams.xy) * occlusionInvExtent + 0.5f) * dim - 0.5f;
	float2 base = floor(texel) - 1.0f;
	// The generator culls nothing outside the map.
	if (any(base < 0.0f) || any(base + 3.0f >= dim))
		return 1.0f;

	// Tent weights of radius two texels for the four texels along each axis; each set sums to one.
	float2 f = texel - floor(texel);
	float4 weightsX = float4(1.0f - f.x, 2.0f - f.x, 1.0f + f.x, f.x) * 0.25f;
	float4 weightsY = float4(1.0f - f.y, 2.0f - f.y, 1.0f + f.y, f.y) * 0.25f;

	float keep = 0.0f;
	[unroll] for (uint block = 0u; block < 4u; ++block)
	{
		uint2 blockIndex = uint2(block & 1u, block >> 1u);
		float2 gatherUV = (base + float2(blockIndex) * 2.0f + 1.0f) / dim;
		// Gather returns (0,1), (1,1), (1,0), (0,0) of the 2x2 texels around the shared corner.
		float4 texelKeep = GetObjectTexelKeep(OcclusionMaskHigh.Gather(LinearSampler, gatherUV),
			OcclusionMaskLow.Gather(LinearSampler, gatherUV), rootPosition.z);
		float2 wx = blockIndex.x == 0u ? weightsX.xy : weightsX.zw;
		float2 wy = blockIndex.y == 0u ? weightsY.xy : weightsY.zw;
		keep += dot(texelKeep, float4(wx.x * wy.y, wx.y * wy.y, wx.y * wy.x, wx.x * wy.x));
	}
	return keep;
}

/**
 * @brief Returns the darkening weight for grass rooted at this position: the share of blades the generator keeps, eased
 * across cells touching bare LAND so the darkening fades a little ahead of the blades.
 */
float GetGrassDarkeningCoverage(float3 rootPosition)
{
	if (debugFlags.x > 0.5f)
		return 1.0f;

	// The generator samples the map through the same warp, plus a per-blade jitter that averages out here.
	float2 samplePosition = rootPosition.xy + GetGrassMapWarp(rootPosition.xy) * (GrassMapWarpShare * miscParams.x);
	uint4 ids;
	float4 weights;
	// HLSL evaluates both sides of && and ||, so try the canopy map only when the presence window misses.
	bool found = LoadGrassPresenceIds(samplePosition, ids, weights);
	if (!found)
		found = LoadTerrainCanopyTypes(samplePosition, ids, weights);
	if (!found)
		return 0.0f;
	if (all(ids == 0u))
		return 0.0f;

	float coverage = 1.0f;
	[branch] if (any(ids == 0u))
	{
		// The blades thin over part of the edge cell; ease the darkening across all of it.
		float presence = dot(float4(ids != 0u), weights);
		coverage = GetGrassMapEdgeCoverage(rootPosition.xy, presence) * smoothstep(0.0f, 1.0f, presence);
	}
	return coverage * GetObjectCoverage(rootPosition);
}

float GetTerrainDarkeningWeight(float2 position)
{
	// Low writes depth after the blended copy was made. Shade its drawn height, not the terrain behind it.
	float depth = min(DepthTexture.Load(int3(position, 0)), RenderedDepthTexture.Load(int3(position, 0)));

	if (depth >= 1.0f)
		return 0.0f;

	float2 screenUV = position * dynamicResolutionInverted;
	float2 ndc = float2(screenUV.x * 2.0f - 1.0f, 1.0f - screenUV.y * 2.0f);

	float4 cr = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, depth, 1.0f));
	cr.xyz /= cr.w;
	float3 world = cr.xyz + FrameBuffer::CameraPosAdjust.xyz;

	float weight = GetTerrainDarkeningOuterFade(length(world.xy - occlusionParams.xy));
	if (weight <= 0.0f)
		return 0.0f;

	float terrainZ = lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, world.xy * heightMapScale + heightMapOffset, 0));
	weight *= 1.0f - saturate((world.z - terrainZ) / max(grassAOParams.w, 1.0f));
	[branch] if (weight > 0.0f)
		weight *= GetGrassDarkeningCoverage(float3(world.xy, terrainZ));

	return saturate(weight);
}

float4 DarkenSceneLighting(float4 scene, float weight)
{
	float3 lighting = Color::IrradianceToLinear(scene.rgb);
	scene.rgb = Color::IrradianceToGamma(ApplyGrassTerrainDarkening(lighting, weight));
	return scene;
}

#if defined(PGRASS_DARKENING_COPY)
float4 main(float4 position : SV_POSITION) : SV_Target0
{
	float4 scene = SceneLighting.Load(int3(position.xy, 0));
	float weight = GetTerrainDarkeningWeight(position.xy);
	return weight > 0.0f ? DarkenSceneLighting(scene, weight) : scene;
}
#else
void main(float4 position : SV_POSITION)
{
	float weight = GetTerrainDarkeningWeight(position.xy);
	if (weight <= 0.0f)
		return;

	int2 pixel = int2(position.xy);
	SceneLighting[pixel] = DarkenSceneLighting(SceneLighting[pixel], weight);
}
#endif
