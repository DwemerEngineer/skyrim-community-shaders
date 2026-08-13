// Samples density to directly darken blades to fake self-occlusion and darken terrain albedo by how much grass stands over each point to fake canopy AO.

#define FRAMEBUFFER
#include "Common/FrameBuffer.hlsli"

cbuffer GrassGlobals : register(b8)
{
	float4 color;
	float2 dynamicResolutionInverted;
	float voronoiGridSize;
	float inverseVoronoiGridSize;
	float cameraViewRow0Sum;
	float cameraViewRow1Sum;
	float windSpeed;
	float windTimer;
	float2 windDir;
	float2 heightMapScale;
	float2 heightMapOffset;
	float2 heightMapZRange;
	float4 debugFlags;
	float4 occlusionParams;
	float4 grassParams;
	float4 occlusionWindow;   // xy: window centre in world space, z: half extent
	float4 grassAOParams;     // x: density map dim, y: darken strength, z: blades-per-texel for full dark
}

Texture2D<float> DepthTexture : register(t0);
Texture2D<uint> GrassDensityTexture : register(t1);
Texture2D<float> TerrainHeightTexture : register(t2);
SamplerState LinearSampler : register(s0);

float SampleDensity(float2 densityUV)
{
	// Manual bilinear over the integer counts, so the darkening does not step at texel edges.
	float2 texel = densityUV * grassAOParams.x - 0.5f;
	int2 base = int2(floor(texel));
	float2 frac = texel - base;

	float d00 = GrassDensityTexture[clamp(base + int2(0, 0), 0, (int)grassAOParams.x - 1)];
	float d10 = GrassDensityTexture[clamp(base + int2(1, 0), 0, (int)grassAOParams.x - 1)];
	float d01 = GrassDensityTexture[clamp(base + int2(0, 1), 0, (int)grassAOParams.x - 1)];
	float d11 = GrassDensityTexture[clamp(base + int2(1, 1), 0, (int)grassAOParams.x - 1)];

	return lerp(lerp(d00, d10, frac.x), lerp(d01, d11, frac.x), frac.y);
}

float4 main(float4 position : SV_POSITION) : SV_Target0
{
	float depth = DepthTexture.Load(int3(position.xy, 0));

	if (depth >= 1.0f)
		return 1.0f;

	float2 screenUV = position.xy * dynamicResolutionInverted;
	float2 ndc = float2(screenUV.x * 2.0f - 1.0f, 1.0f - screenUV.y * 2.0f);

	float4 cr = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, depth, 1.0f));
	cr.xyz /= cr.w;
	float3 world = cr.xyz + FrameBuffer::CameraPosAdjust.xyz;

	float2 densityUV = (world.xy - occlusionWindow.xy) / (occlusionWindow.z * 2.0f) + 0.5f;
	if (densityUV.x != saturate(densityUV.x) || densityUV.y != saturate(densityUV.y))
		return 1.0f;

	float density = SampleDensity(densityUV);
	float ao = saturate(density / max(grassAOParams.z, 1.0f)) * grassAOParams.y;

	// Calculate height gradient for blades, so that the ground is the darkest part and fade to nothing at the canopy top (grassAOParams.w, world units)
	float terrainZ = lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, world.xy * heightMapScale + heightMapOffset, 0));
	float heightFraction = saturate((world.z - terrainZ) / max(grassAOParams.w, 1.0f));
	ao *= 1.0f - heightFraction;

	return saturate(1.0f - ao);
}
