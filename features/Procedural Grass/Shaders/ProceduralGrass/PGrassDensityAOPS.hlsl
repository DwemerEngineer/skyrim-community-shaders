// Samples density to directly darken blades to fake self-occlusion and darken terrain albedo by how much grass stands over each point to fake canopy AO.

#define FRAMEBUFFER
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"

#include "ProceduralGrass/PGrassCommon.hlsli"

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

float StableFarDensity(float2 worldPosition)
{
	// A low-frequency world-space field is stable as the camera-centred density window moves.
	float2 p = worldPosition * (1.0f / 4096.0f);
	int2 cell = int2(floor(p));
	float2 blend = frac(p);
	float2 blend2 = blend * blend;
	blend = blend2 * blend * (blend * (blend * 6.0f - 15.0f) + 10.0f);

	float n00 = float(Random::iqint3(asuint(cell))) * (1.0f / 4294967296.0f);
	float n10 = float(Random::iqint3(asuint(cell + int2(1, 0)))) * (1.0f / 4294967296.0f);
	float n01 = float(Random::iqint3(asuint(cell + int2(0, 1)))) * (1.0f / 4294967296.0f);
	float n11 = float(Random::iqint3(asuint(cell + int2(1, 1)))) * (1.0f / 4294967296.0f);
	float noise = lerp(lerp(n00, n10, blend.x), lerp(n01, n11, blend.x), blend.y);

	// Keep the approximation consistently dark while retaining broad natural variation.
	return grassAOParams.z * lerp(0.75f, 1.0f, noise);
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

	float2 worldOffset = world.xy - occlusionParams.xy;
	float radialDistance = length(worldOffset);
	float farRange = 1.0f / max(farParams.y, 1.0e-6f);
	float farEnd = farParams.x + farRange;
	if (radialDistance >= farEnd)
		return 1.0f;

	// Radially blend the local density window into Far's approximation and stop sampling after the handoff point
	float handoffStart = occlusionHalfExtent * 0.8f;
	float handoffT = saturate((radialDistance - handoffStart) / max(farParams.x - handoffStart, 1.0f));
	float handoffT2 = handoffT * handoffT;
	handoffT = handoffT2 * handoffT * (handoffT * (handoffT * 6.0f - 15.0f) + 10.0f);
	float density = StableFarDensity(world.xy);
	[branch] if (handoffT < 1.0f)
	{
		float2 densityUV = worldOffset * occlusionInvExtent + 0.5f;
		density = lerp(SampleDensity(densityUV), density, handoffT);
	}
	float ao = saturate(density / max(grassAOParams.z, 1.0f)) * grassAOParams.y;

	// Hold Far terrain darkening steady, then retire it only over the outer 4096 units.
	float outerFadeStart = max(farParams.x, farEnd - 4096.0f);
	float outerT = saturate((radialDistance - outerFadeStart) / max(farEnd - outerFadeStart, 1.0f));
	float outerT2 = outerT * outerT;
	float outerFade = 1.0f - outerT2 * outerT * (outerT * (outerT * 6.0f - 15.0f) + 10.0f);
	ao *= outerFade;

	float terrainZ = lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, world.xy * heightMapScale + heightMapOffset, 0));
	float heightFraction = saturate((world.z - terrainZ) / max(grassAOParams.w, 1.0f));
	ao *= 1.0f - heightFraction;

	return saturate(1.0f - ao);
}
