#define FRAMEBUFFER
#define TRUE_PBR
#define GRASS_LIGHTING
#define LOW_LOD
#define FAR_LOD

#include "Common/PBRMath.hlsli"
static const uint PBRFlags = PBR::Flags::Subsurface;

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/LightingEval.hlsli"
#include "Common/Random.hlsli"

SamplerState LinearSampler : register(s0);
SamplerState ShadowSampler : register(s14);
#include "Common/ShadowSampling.hlsli"
#include "ProceduralGrass/PGrassCommon.hlsli"

Texture2D<float2> LandscapeMasks : register(t2);
Texture2D<float> GrassHiZ : register(t8);
Texture2D<float4> ShadowMask : register(t14);
Texture2D<unorm float> GrassScreenShadow : register(t45);
Texture2D<float4> DistantAmbientLUT : register(t73);
Texture2D<float> GrassSceneDepth : register(t74);
Texture2D<float> GrassScreenAO : register(t76);
RWTexture2D<float4> MainLighting : register(u0);

#include "ProceduralGrass/PGrassCanopy.hlsli"
#include "ProceduralGrass/PGrassLighting.hlsli"
#include "ProceduralGrass/PGrassMaterial.hlsli"

// Far lighting builds on the shared lighting and material helpers above.
#include "ProceduralGrass/PGrassFarLighting.hlsli"

struct CanopyMaterial
{
	float3 baseColor;
	float3 reflection;
	float3 transmission;
	float3 bounce;
	float ao;
	float f0;
	float fuzzWeight;
	float fuzzTint;
	float fuzzRoughness;
	float ambientDesaturation;
	float overhead;
	float projectedArea;
};

void AddCanopyMaterial(inout CanopyMaterial material, uint typeIndex, float weight, float widthScale)
{
	GrassType type = grassType[typeIndex];
	// A tapered triangle's area-weighted blade position is one third of its height.
	static const float Along = 1.0f / 3.0f;
	float3 meanClumpColor = lerp(1.0f, (type.grassColorCool.rgb + type.grassColorWarm.rgb) * 0.5f, type.clumpColorStrength);
	float3 color = saturate(Color::ColorToLinear(GetDistantBladeColor(type, meanClumpColor, Along)));
	float3 surface = GetDistantAOThicknessRoughness(type, 1.0f / 3.0f, 0.5f, Along);
	float3 scatteringColor = saturate(color * type.grassSubsurfaceColor.rgb);
	float3 reflection, transmission;
	GetGrassScatteringAlbedos(color, scatteringColor, surface.y, type.grassSurfParams.y,
		reflection, transmission);

	material.baseColor += color * weight;
	material.reflection += reflection * weight;
	material.transmission += transmission * weight;
	material.bounce += type.grassBounceColor.rgb * type.grassTypeLightParams.x * weight;
	material.ao += surface.x * weight;
	material.f0 += type.specular * weight;
	material.fuzzWeight += type.grassSurfParams.x * weight;
	material.fuzzTint += type.grassSubsurfaceColor.w * weight;
	material.fuzzRoughness += type.grassSurfParams.w * weight;
	material.ambientDesaturation += type.grassTypeLightParams.w * weight;
	material.overhead += (1.0f - saturate(type.grassTypeLightParams.y * Along)) * weight;
	material.projectedArea += type.grassTypeLightParams.z * widthScale * weight;
}

float3 ShadeTerrainCanopy(CanopyMaterial canopy, float3 position, int2 pixel, float farWidthT, float groundSunFacing)
{
	float3 V = normalize(-position);
	float3 L = SharedData::DirLightDirection.xyz;
	// Average the view-facing blade orientations, then follow the blades' existing normal ramp.
	float normalBlend = smoothstep(2048.0f, 24576.0f, length(position.xy));
	float2 meanFacing = V.xy * rsqrt(max(dot(V.xy, V.xy), 1.0e-8f)) * (2.0f / Math::PI);
	float3 N = normalize(float3(meanFacing * (1.0f - normalBlend), max(normalBlend, 1.0e-4f)));
	float occlusionScale = lerp(1.0f, 0.8f, smoothstep(0.0f, 1.0f, farWidthT));
	float contact = lerp(1.0f, lerp(0.62f, 0.9f, smoothstep(0.0f, 0.9f, 1.0f / 3.0f)), occlusionScale);

	MaterialProperties material = (MaterialProperties)0;
	material.BaseColor = canopy.baseColor;
	material.AO = sqrt(saturate(canopy.ao * contact));
	material.F0 = saturate(canopy.f0);
	float fuzzNdotV = clamp(dot(N, V), EPSILON_DOT_CLAMP, 1.0f);
	float fuzzRoughness = clamp(canopy.fuzzRoughness, 0.01f, 1.0f);
	FarGrassSurface surface;
	surface.reflectionAlbedo = canopy.reflection;
	surface.transmissionAlbedo = canopy.transmission;
	surface.bounceColor = canopy.bounce;
	surface.ambientDesaturation = canopy.ambientDesaturation;
	surface.fuzzAlbedo = saturate(canopy.fuzzWeight) * PBR::FuzzDirectionalAlbedo(fuzzNdotV, fuzzRoughness);
	surface.fuzzColor = GetGrassFuzzColor(canopy.baseColor, canopy.fuzzTint);

	float2 shadowUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition((float2(pixel) + 0.5f) * dynamicResolutionInverted);
	float surfaceShadow = ShadowMask.SampleLevel(ShadowSampler, shadowUV, 0).x *
	                      lerp(1.0f, groundSunFacing, smoothstep(0.0f, 0.1f, farWidthT));
	float detailShadow = 1.0f;
#if defined(SCREEN_SPACE_SHADOWS)
	float traceFade = GetFarGrassRootShadowFade(position);
	[branch] if (traceFade > 0.0f)
		detailShadow = lerp(1.0f, SampleGrassRootContactShadow(GrassScreenShadow, LinearSampler, float2(pixel) + 0.5f), traceFade);
	detailShadow *= GetBladeShadowPattern(V, L, 0u, 1.0f / 3.0f, 1.0f);
#endif
	detailShadow *= contact;
	float worldShadow = ShadowSampling::GetWorldShadow(position, FrameBuffer::CameraPosAdjust.xyz);
	float3 lightColor = grassFrameLight.xyz * worldShadow;
	FarGrassLighting lighting = GetFarGrassLighting(surface, material, N, N, V, L, lightColor,
		surfaceShadow, detailShadow, 1.0f, canopy.overhead, 1.0f - canopy.overhead);

	float screenAO = 1.0f - occlusionScale * saturate(GrassScreenAO.Load(int3(pixel, 0)));
	return ResolveFarGrassLighting(lighting, screenAO);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	int2 pixel = int2(dispatchID.xy);
	if (any(dispatchID.xy >= uint2(rcp(dynamicResolutionInverted))))
		return;
	float landscape = LandscapeMasks.Load(int3(pixel, 0)).y;
	if (landscape < GBuffer::LandscapeMask - (0.5f / 65535.0f))
		return;
	float depth = GrassSceneDepth.Load(int3(pixel, 0));
	if (depth >= 1.0f)
		return;
	float3 position = GetScenePosition(pixel);
	float2 worldPosition = position.xy + FrameBuffer::CameraPosAdjust.xy;
	float fade = GetTerrainCanopyDistanceBlend(worldPosition);
	if (fade <= 0.0f)
		return;
	fade *= GetTerrainCanopyInterior(position);
	if (fade <= 0.0f)
		return;

	uint4 types;
	float4 weights;
	if (!LoadTerrainCanopyTypes(worldPosition, types, weights))
		return;
	// Most pixels have one material; evaluate it once rather than repeating the same four lookups.
	if (all(types == types.x)) {
		types.yzw = 0u;
		weights = float4(1.0f, 0.0f, 0.0f, 0.0f);
	}
	float coverage = dot(weights, float4(types != 0u));
	if (coverage <= 0.0f)
		return;

	float2 farCoverage = GetFarCoverage(worldPosition - grassLodOrigin, FrameBuffer::CameraProj._m00);
	float widthScale = GetFarWidthScale(worldPosition - grassLodOrigin) * farCoverage.y;
	CanopyMaterial canopy = (CanopyMaterial)0;
	[loop] for (uint i = 0u; i < 4u; ++i)
	{
		if (types[i] != 0u && weights[i] > 0.0f)
			AddCanopyMaterial(canopy, types[i], weights[i] / coverage, widthScale);
	}

	float3 across = GetScenePosition(pixel + int2(4, 0)) - position;
	float3 beyond = GetScenePosition(pixel - int2(0, 4)) - position;
	float3 groundNormal = cross(across, beyond);
	float normalLength = dot(groundNormal, groundNormal);
	groundNormal = normalLength > 1.0e-8f ? groundNormal * rsqrt(normalLength) : float3(0.0f, 0.0f, 1.0f);
	groundNormal *= groundNormal.z < 0.0f ? -1.0f : 1.0f;
	float groundSunFacing = smoothstep(-0.1f, 0.1f, dot(groundNormal, SharedData::DirLightDirection.xyz));

	float2 lodOffset = worldPosition - grassLodOrigin;
	float keep = GetFarCanopyKeep(lodOffset, FrameBuffer::CameraProj._m00, terrainCanopyParams.w);
	float3 V = normalize(-position);
	float grazingCoverage = length(V.xy) / max(abs(dot(groundNormal, V)), 0.08f);
	float opticalDepth = canopy.projectedArea * (1.0f + GetSlopeFillKeep(groundNormal.z)) * keep * coverage * grazingCoverage /
	                     max(4.0f * farParams.z * farParams.z, 1.0f);
	// Replaced and retained blades cover complementary portions of the same canopy optical depth.
	float opacity = 1.0f - exp(-opticalDepth * fade);
	if (opacity <= 0.0f)
		return;

	float3 grassLighting = ShadeTerrainCanopy(canopy, position, pixel, farCoverage.x, groundSunFacing);
	float4 scene = MainLighting[pixel];
	scene.rgb = Color::IrradianceToGamma(lerp(Color::IrradianceToLinear(scene.rgb), grassLighting, opacity));
	MainLighting[pixel] = scene;
}
