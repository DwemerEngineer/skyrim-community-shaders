#define PSHADER
#define DEFERRED
#define FRAMEBUFFER
#define TRUE_PBR
#define LIGHT_LIMIT_FIX

#if defined(__RESHARPER__)
#	define ISL
#	define DYNAMIC_CUBEMAPS
#	define TERRAIN_SHADOWS
#	define CLOUD_SHADOWS
#	define SKYLIGHTING
#	define WATER_EFFECTS
#	define SCREEN_SPACE_SHADOWS
#	define WETNESS_EFFECTS
#endif

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

uint PBRFlags = 0;

struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float4 WorldPosition : TEXCOORD0;
	float4 PreviousWorldPosition : TEXCOORD1;
	float2 TexCoord : TEXCOORD2;
	float2 AOSubsurfaceThickness : TEXCOORD3;
	nointerpolation float4 TipMidpoint : TEXCOORD4;
	nointerpolation float3 FacingAndDoubleBladeBlade1 : TEXCOORD5;
	nointerpolation float2 MetalnessSpecular : TEXCOORD6;
#if defined(VR)
	float ClipDistance : SV_ClipDistance0;  // o11
	float CullDistance : SV_CullDistance0;  // p11
#endif
};

struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
	float4 MotionVectors : SV_Target1;
	float4 NormalGlossiness : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
	float4 Masks : SV_Target6;
};

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
	float2 padgg0;
}

SamplerState SampColorSampler : register(s0);
#define SampNormalSampler SampColorSampler
SamplerState SampGlowSampler : register(s6);
SamplerState SampShadowMaskSampler : register(s14);

Texture2D TexRMAOSSampler : register(t5);
Texture2D TexGlowSampler : register(t6);
Texture2D TexShadowMaskSampler : register(t14);

Texture1DArray<float2> GrassDRTexArray : register(t48);
Texture1DArray GrassLUTsTexArray : register(t49);

SamplerState LinearGrassSampler : register(s15);  // TODO restore original sampler when done

#if defined(WATER_EFFECTS)
#	include "WaterEffects/WaterCaustics.hlsli"
#endif

#if defined(DYNAMIC_CUBEMAPS)
#	include "DynamicCubemaps/DynamicCubemaps.hlsli"
#endif

#if defined(TRUE_PBR)
#	include "Common/PBR.hlsli"
#endif

#if defined(SCREEN_SPACE_SHADOWS)
#	include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#endif

#if defined(LIGHT_LIMIT_FIX)
#	include "LightLimitFix/LightLimitFix.hlsli"
#endif

#if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#	include "InverseSquareLighting/InverseSquareLighting.hlsli"
#endif

#if defined(WETNESS_EFFECTS)
#	include "WetnessEffects/WetnessEffects.hlsli"
#endif

#if defined(TERRAIN_SHADOWS)
#	include "TerrainShadows/TerrainShadows.hlsli"
#endif

#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

#define LinearSampler SampColorSampler

#include "Common/ShadowSampling.hlsli"

PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout;

	uint eyeIndex = Stereo::GetEyeIndexPS(input.Position, 0);  // TODO was VPOSOffset, likely will be per eye offsets in VR

	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewPosition.z);

	float3 worldSpaceViewDirection = -normalize(input.WorldPosition.xyz);

	float2 uv = input.TexCoord.xy;
	float2 diffuseRoughness = GrassDRTexArray.SampleLevel(LinearGrassSampler, float2(uv.x, 0), 0);
	float4 grassColor = GrassLUTsTexArray.SampleLevel(LinearGrassSampler, float2(uv.y, 0), 0);

	float4 rawBaseColor = float4(color.xyz * diffuseRoughness.r, 1);
	rawBaseColor.xyz *= input.AOSubsurfaceThickness.x;
	// float4 baseColor = float4(Color::Diffuse(rawBaseColor.rgb), rawBaseColor.a);
	float4 baseColor = float4(rawBaseColor.rgb, rawBaseColor.a);

	float4 rawRMAOS = float4(diffuseRoughness.g, input.MetalnessSpecular.x, 1, input.MetalnessSpecular.y);

	float2 facing = input.FacingAndDoubleBladeBlade1.xy;
	float3 bitangent = float3(facing.y, -facing.x, 0.0f) * (input.FacingAndDoubleBladeBlade1.z * 2.0f - 1.0f);
	float2 derivative = 2.0f * (1.0f - input.TexCoord.y) * input.TipMidpoint.zw + 2.0f * input.TexCoord.y * (input.TipMidpoint.xy - input.TipMidpoint.zw);
	float3 tangent = normalize(float3(facing * derivative.x, derivative.y));
	float3 normal = cross(-bitangent, tangent);
	float side = input.TexCoord.x * 2.0f - 1.0f;
	float3 edgeNormal = bitangent * sign(side);
	float3 curvedNormal = normalize(lerp(normal, edgeNormal, 0.5f * abs(side)));

	// float3 dx = ddx(input.WorldPosition.xyz);
	// float3 dy = ddy(input.WorldPosition.xyz);
	// float3 worldSpaceNormal  = normalize( cross(dy, dx) );

	float3 worldSpaceNormal = frontFace ? curvedNormal : reflect(curvedNormal, normal);

	float3 screenSpaceNormal = normalize(FrameBuffer::WorldToView(worldSpaceNormal, false, eyeIndex));

	float2 baseShadowUV = 1.0;
	float4 shadowColor = 1.0;

	// baseShadowUV = input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy; // TODO figure out why DynamicResolutionParams2.xy is incorrect
	// baseShadowUV = input.Position.xy * float2(1.0f / 2560.0f, 1.0f / 1440.0f);
	baseShadowUV = input.Position.xy * dynamicResolutionInverted;
	// float2 adjustedShadowUV = baseShadowUV * VPOSOffset.xy + VPOSOffset.zw;
	float2 adjustedShadowUV = baseShadowUV;
	float2 shadowUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(adjustedShadowUV);
	shadowColor = TexShadowMaskSampler.Sample(SampShadowMaskSampler, shadowUV);

	PBR::SurfaceProperties pbrSurfaceProperties = PBR::InitSurfaceProperties();

	pbrSurfaceProperties.Noise = screenNoise;

	pbrSurfaceProperties.Roughness = saturate(rawRMAOS.x);
	pbrSurfaceProperties.Metallic = saturate(rawRMAOS.y);
	pbrSurfaceProperties.AO = rawRMAOS.z;
	pbrSurfaceProperties.F0 = lerp(saturate(rawRMAOS.w), Color::GammaToLinear(baseColor.xyz), pbrSurfaceProperties.Metallic);

	baseColor.xyz *= 1 - pbrSurfaceProperties.Metallic;

	pbrSurfaceProperties.BaseColor = baseColor.xyz;
	pbrSurfaceProperties.SubsurfaceColor = baseColor.xyz;
	pbrSurfaceProperties.Thickness = input.AOSubsurfaceThickness.y;

	float3 specularColorPBR = 0;
	float3 transmissionColor = 0;

	float pbrGlossiness = 1 - pbrSurfaceProperties.Roughness;

	float porosity = 1.0;

#if defined(SKYLIGHTING)
#	if defined(VR)
	float3 positionMSSkylight = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#	else
	float3 positionMSSkylight = input.WorldPosition.xyz;
#	endif

	float3 skylightingNormal = normalize(float3(worldSpaceNormal.xy, max(0, worldSpaceNormal.z)));

	sh2 skylightingSH = Skylighting::sample(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, Skylighting::stbn_vec3_2Dx1D_128x128x64, input.Position.xy, positionMSSkylight, worldSpaceNormal);

#endif

	float4 waterData = SharedData::GetWaterData(input.WorldPosition.xyz);
	float waterHeight = waterData.w;

	float waterRoughnessSpecular = 1;

#if defined(WETNESS_EFFECTS)
	float wetness = 0.0;

	float wetnessDistToWater = abs(input.WorldPosition.z - waterHeight);
	float shoreFactor = saturate(1.0 - (wetnessDistToWater / (float)SharedData::wetnessEffectsSettings.ShoreRange));
	float shoreFactorAlbedo = shoreFactor;

	[flatten] if (input.WorldPosition.z < waterHeight)
		shoreFactorAlbedo = 1.0;

	float minWetnessValue = SharedData::wetnessEffectsSettings.MinRainWetness;

	float minWetnessAngle = 0;
	minWetnessAngle = saturate(max(minWetnessValue, worldSpaceNormal.z));
#	if defined(SKYLIGHTING)
	float wetnessOcclusion = pow(saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1))), 2);
#	else
	float wetnessOcclusion = 1;
#	endif

	float4 raindropInfo = float4(0, 0, 1, 0);
	if (worldSpaceNormal.z > 0 && SharedData::wetnessEffectsSettings.Raining > 0.0f && SharedData::wetnessEffectsSettings.EnableRaindropFx) {
		float4 precipOcclusionTexCoord = mul(SharedData::wetnessEffectsSettings.OcclusionViewProj, float4(input.WorldPosition.xyz, 1));
		precipOcclusionTexCoord.y = -precipOcclusionTexCoord.y;
		float2 precipOcclusionUV = precipOcclusionTexCoord.xy * 0.5 + 0.5;

		if (saturate(precipOcclusionUV.x) == precipOcclusionUV.x && saturate(precipOcclusionUV.y) == precipOcclusionUV.y) {
			float precipOcclusionZ = WetnessEffects::TexPrecipOcclusion.SampleLevel(SampColorSampler, precipOcclusionUV, 0).x;

			if (precipOcclusionTexCoord.z < precipOcclusionZ + 0.1)
#	if defined(SKINNED)
				raindropInfo = WetnessEffects::GetRainDrops(input.ModelPosition.xyz, SharedData::wetnessEffectsSettings.Time, worldSpaceNormal);
#	elif defined(DEFERRED)
				raindropInfo = WetnessEffects::GetRainDrops(input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz, SharedData::wetnessEffectsSettings.Time, worldSpaceNormal);
#	else
				raindropInfo = WetnessEffects::GetRainDrops(!FrameBuffer::FrameParams.y ? input.ModelPosition.xyz : input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz, SharedData::wetnessEffectsSettings.Time, worldSpaceNormal);
#	endif
		}
	}

	float rainWetness = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
	rainWetness = max(rainWetness, raindropInfo.w);

	float puddleWetness = SharedData::wetnessEffectsSettings.PuddleWetness * minWetnessAngle;

	rainWetness *= wetnessOcclusion;
	puddleWetness *= wetnessOcclusion;

	wetness = max(shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness, rainWetness);

	float3 wetnessNormal = worldSpaceNormal;

	float3 puddleCoords = ((input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz) * 0.5 + 0.5) * 0.01 / SharedData::wetnessEffectsSettings.PuddleRadius;
	float puddle = wetness;
	if (wetness > 0.0 || puddleWetness > 0) {
#	if !defined(SKINNED)
		puddle = Random::perlinNoise(puddleCoords) * .5 + .5;
		puddle = puddle * ((minWetnessAngle / SharedData::wetnessEffectsSettings.PuddleMaxAngle) * SharedData::wetnessEffectsSettings.MaxPuddleWetness * 0.25) + 0.5;
		wetness = lerp(wetness, puddleWetness, saturate(puddle - 0.25));
#	endif
		puddle *= wetness;
	}

	puddle *= nearFactor;

	float3 wetnessSpecular = 0.0;

	float wetnessGlossinessAlbedo = max(puddle, shoreFactorAlbedo * SharedData::wetnessEffectsSettings.MaxShoreWetness);
	wetnessGlossinessAlbedo *= wetnessGlossinessAlbedo;

	float wetnessGlossinessSpecular = puddle;
	wetnessGlossinessSpecular = lerp(wetnessGlossinessSpecular, wetnessGlossinessSpecular * shoreFactor, input.WorldPosition.z < waterHeight);

	float flatnessAmount = smoothstep(SharedData::wetnessEffectsSettings.PuddleMaxAngle, 1.0, minWetnessAngle);

	flatnessAmount *= smoothstep(SharedData::wetnessEffectsSettings.PuddleMinWetness, 1.0, wetnessGlossinessSpecular);

	wetnessNormal = normalize(lerp(wetnessNormal, float3(0, 0, 1), flatnessAmount));

	float3 rippleNormal = normalize(lerp(float3(0, 0, 1), raindropInfo.xyz, lerp(1.0, flatnessAmount, 0.8)));
	wetnessNormal = WetnessEffects::ReorientNormal(rippleNormal, wetnessNormal);

	waterRoughnessSpecular = 1.0 - wetnessGlossinessSpecular * 0.9;
#endif

	float3 dirLightColor = Color::Light(SharedData::DirLightColor.xyz);
	float3 dirLightColorMultiplier = 1;

#if defined(WATER_EFFECTS)
	dirLightColorMultiplier *= WaterEffects::ComputeCaustics(waterData, input.WorldPosition.xyz, eyeIndex);
#endif

	float3 dirLightDirection = SharedData::DirLightDirection.xyz;
	dirLightColorMultiplier *= shadowColor.x;

	float dirDetailShadow = 1.0;
#if defined(SCREEN_SPACE_SHADOWS)
	dirDetailShadow = ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, screenUV, screenNoise, eyeIndex);
#endif

	float dirShadow = ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);

	dirLightColorMultiplier *= dirShadow;

	float3 diffuseColor = 0;
	float3 specularColor = 0;

	float3 lightsDiffuseColor = 0;
	float3 lightsSpecularColor = 0;

	PBR::LightProperties lightProperties = PBR::InitLightProperties(dirLightColor, dirLightColorMultiplier * dirDetailShadow, 1);
	float3 dirDiffuseColor, dirTransmissionColor, dirSpecularColor;
	PBR::GetDirectLightInputProcGrass(dirDiffuseColor, dirTransmissionColor, dirSpecularColor, worldSpaceNormal, worldSpaceViewDirection, dirLightDirection, lightProperties, pbrSurfaceProperties);
	lightsDiffuseColor += dirDiffuseColor;
	transmissionColor += dirTransmissionColor;
	specularColorPBR += dirSpecularColor;
#if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1.0)
		specularColorPBR += PBR::GetWetnessDirectLightSpecularInput(wetnessNormal, worldSpaceViewDirection, dirLightDirection, lightProperties.CoatLightColor, waterRoughnessSpecular) * wetnessGlossinessSpecular;
#endif

	uint numClusteredLights = 0;
	uint totalLightCount = LightLimitFix::NumStrictLights;
	uint clusterIndex = 0;
	uint lightOffset = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		numClusteredLights = LightLimitFix::lightGrid[clusterIndex].lightCount;
		totalLightCount += numClusteredLights;
		lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
	}

	uint contactShadowSteps = round(4.0 * (1.0 - saturate(viewPosition.z / 1024.0)));

	[loop] for (uint lightIndex = 0; lightIndex < totalLightCount; lightIndex++)
	{
		LightLimitFix::Light light;
		if (lightIndex < LightLimitFix::NumStrictLights) {
			light = LightLimitFix::StrictLights[lightIndex];
		} else {
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + (lightIndex - LightLimitFix::NumStrictLights)];
			light = LightLimitFix::lights[clusteredLightIndex];

			if (LightLimitFix::IsLightIgnored(light) || (!(Permutation::PixelShaderDescriptor & Permutation::LightingFlags::DefShadow) && light.lightFlags & LightLimitFix::LightFlags::Shadow)) {
				continue;
			}
		}

		float3 lightDirection = light.positionWS[eyeIndex].xyz - input.WorldPosition.xyz;
		float lightDist = length(lightDirection);

#if defined(ISL)
		float intensityMultiplier = InverseSquareLighting::GetAttenuation(lightDist, light);
		if (intensityMultiplier < 1e-5)
			continue;
#else
		float intensityFactor = saturate(lightDist / light.radius);
		if (intensityFactor == 1)
			continue;
		float intensityMultiplier = 1 - intensityFactor * intensityFactor;
#endif

		float3 lightColor = Color::Light(light.color.xyz) * intensityMultiplier;
		float lightShadow = 1.0;

		float shadowComponent = 1.0;
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
			shadowComponent = shadowColor[light.shadowLightIndex];
			lightShadow *= shadowComponent;
		}

		float3 normalizedLightDirection = normalize(lightDirection);
		float lightAngle = dot(worldSpaceNormal.xyz, normalizedLightDirection.xyz);

		float contactShadow = 1.0;
		[branch] if (
			!FrameBuffer::FrameParams.z &&
			SharedData::lightLimitFixSettings.EnableContactShadows &&
			!(light.lightFlags & LightLimitFix::LightFlags::Simple) &&
			shadowComponent != 0.0 &&
			lightAngle > 0.0)
		{
			float3 normalizedLightDirectionVS = normalize(light.positionVS[eyeIndex].xyz - viewPosition.xyz);
			contactShadow = LightLimitFix::ContactShadows(viewPosition, screenNoise, normalizedLightDirectionVS, contactShadowSteps, eyeIndex);
		}

		PBR::LightProperties pointLightProperties = PBR::InitLightProperties(lightColor, lightShadow * contactShadow, 1);
		float3 pointDiffuseColor, pointTransmissionColor, pointSpecularColor;
		PBR::GetDirectLightInputProcGrass(pointDiffuseColor, pointTransmissionColor, pointSpecularColor, worldSpaceNormal, worldSpaceViewDirection, normalizedLightDirection, pointLightProperties, pbrSurfaceProperties);
		lightsDiffuseColor += pointDiffuseColor;
		transmissionColor += pointTransmissionColor;
		specularColorPBR += pointSpecularColor;
#if defined(WETNESS_EFFECTS)
		if (waterRoughnessSpecular < 1.0)
			specularColorPBR += PBR::GetWetnessDirectLightSpecularInput(wetnessNormal, worldSpaceViewDirection, normalizedLightDirection, pointLightProperties.CoatLightColor, waterRoughnessSpecular) * wetnessGlossinessSpecular;
#endif

#if defined(WETNESS_EFFECTS)
		if (waterRoughnessSpecular < 1.0)
			wetnessSpecular += WetnessEffects::GetWetnessSpecular(wetnessNormal, normalizedLightDirection, worldSpaceViewDirection, lightColor, waterRoughnessSpecular);
#endif
	}

	diffuseColor += lightsDiffuseColor;
	specularColor += lightsSpecularColor;

	float3 directionalAmbientColor = max(0, mul(SharedData::DirectionalAmbient, float4(worldSpaceNormal, 1.0f)));

#if defined(SKYLIGHTING)
	float skylightingFadeOutFactor = Skylighting::getFadeOutFactor(input.WorldPosition.xyz);

	float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(skylightingNormal)) / Math::PI;
	skylightingDiffuse = saturate(skylightingDiffuse);

	skylightingDiffuse = lerp(1.0, skylightingDiffuse, skylightingFadeOutFactor);

	skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);

	directionalAmbientColor = Color::GammaToLinear(directionalAmbientColor);

	directionalAmbientColor *= skylightingDiffuse;
	directionalAmbientColor *= 1.0 + saturate(worldSpaceNormal.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);
	directionalAmbientColor = Color::LinearToGamma(directionalAmbientColor);
#endif

	float3 reflectionDiffuseColor = diffuseColor + directionalAmbientColor;

#if defined(WETNESS_EFFECTS)
	porosity = lerp(porosity, 0.0, saturate(sqrt(pbrSurfaceProperties.Metallic)));

	float wetnessDarkeningAmount = porosity * wetnessGlossinessAlbedo;
	baseColor.xyz = lerp(baseColor.xyz, pow(baseColor.xyz, 1.0 + wetnessDarkeningAmount), 0.8);

	float3 wetnessReflectance = WetnessEffects::GetWetnessAmbientSpecular(screenUV, wetnessNormal, worldSpaceNormal, worldSpaceViewDirection, waterRoughnessSpecular) * wetnessGlossinessSpecular;
#endif

#if defined(SKYLIGHTING)
	float3 vertexColor = 1;
	float vertexAO = max(max(vertexColor.r, vertexColor.g), vertexColor.b);

	vertexColor *= 1.0 + (1.0 - vertexAO) * (1.0 - skylightingDiffuse);
#else
	// float3 vertexColor = input.Color.xyz;
	float3 vertexColor = 1;
#endif

	float4 color = 0;

	color.xyz += diffuseColor * baseColor.xyz;

	float3 indirectDiffuseLobeWeight, indirectSpecularLobeWeight;
	PBR::GetIndirectLobeWeightsProcGrass(indirectDiffuseLobeWeight, indirectSpecularLobeWeight, worldSpaceNormal, worldSpaceViewDirection, worldSpaceNormal, baseColor.xyz, pbrSurfaceProperties);
#if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1.0)
		indirectSpecularLobeWeight += PBR::GetWetnessIndirectSpecularLobeWeight(wetnessNormal, worldSpaceViewDirection, worldSpaceNormal, waterRoughnessSpecular) * wetnessGlossinessSpecular;
#endif

#if !defined(SSGI)
	color.xyz += indirectDiffuseLobeWeight * directionalAmbientColor;
#endif

	// indirectDiffuseLobeWeight *= vertexColor;

	color.xyz += transmissionColor;

	// color.xyz *= vertexColor;

	specularColor = Color::GammaToLinear(specularColor);

	diffuseColor = reflectionDiffuseColor;

#if defined(ENVMAP)
#	if defined(DYNAMIC_CUBEMAPS)
	if (!dynamicCubemap)
#	endif
		specularColor += envColor * Color::GammaToLinear(diffuseColor);
#endif

	color.xyz *= Color::PBRLightingScale;
	specularColorPBR *= Color::PBRLightingScale;
	specularColor = specularColorPBR;

#if defined(TESTCUBEMAP) && defined(ENVMAP) && defined(DYNAMIC_CUBEMAPS)
	baseColor.xyz = 0.0;
	specularColor = 0.0;
	diffuseColor = 0.0;
	dynamicCubemap = true;
	envColor = 1.0;
	envRoughness = 0.0;
	color.xyz = 0;
#endif

	psout.Diffuse.w = 1;

#if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap(LightLimitFix::NumStrictLights >= 7.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap((float)LightLimitFix::NumStrictLights / 15.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 2) {
			psout.Diffuse.xyz = LightLimitFix::TurboColormap((float)numClusteredLights / MAX_CLUSTER_LIGHTS);
		} else {
			psout.Diffuse.xyz = shadowColor.xyz;
		}
		baseColor.xyz = 0.0;
	} else {
		psout.Diffuse.xyz = color.xyz;
	}
#else
	psout.Diffuse.xyz = color.xyz;
#endif

	psout.Specular = float4(specularColor, psout.Diffuse.w);

	float3 outputAlbedo = baseColor.xyz;  // * vertexColor;

	outputAlbedo = indirectDiffuseLobeWeight;

	psout.Albedo = float4(outputAlbedo, psout.Diffuse.w);

	const float wetnessGlossinessGain = 0.65;

	psout.Reflectance = float4(indirectSpecularLobeWeight, psout.Diffuse.w);
#if defined(WETNESS_EFFECTS)
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), lerp(pbrGlossiness, saturate(pbrGlossiness + wetnessGlossinessGain), wetnessGlossinessSpecular), psout.Diffuse.w);
#else
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), pbrGlossiness, psout.Diffuse.w);
#endif

#if defined(ENVMAP)
#	if defined(DYNAMIC_CUBEMAPS)
	if (dynamicCubemap) {
#		if defined(WETNESS_EFFECTS)
		psout.Reflectance.xyz = max(envColor, wetnessReflectance);
		psout.NormalGlossiness.z = lerp(1.0 - envRoughness, saturate(1.0 - envRoughness + wetnessGlossinessGain), wetnessGlossinessSpecular);
#		else
		psout.Reflectance.xyz = envColor;
		psout.NormalGlossiness.z = 1.0 - envRoughness;
#		endif
	}
#	endif
#endif

#if defined(WETNESS_EFFECTS)
	float wetnessNormalAmount = saturate(dot(float3(0, 0, 1), wetnessNormal) * saturate(flatnessAmount));
	psout.Masks = float4(0, 0, wetnessNormalAmount, psout.Diffuse.w);
#else
	psout.Masks = float4(0, 0, 0, psout.Diffuse.w);
#endif

	// psout.Diffuse = float4((screenSpaceNormal * 0.5f + 0.5f), 1);
	// psout.Diffuse.xy = 0;
	// psout.Specular = 0;
	// psout.Albedo = 0;
	// psout.NormalGlossiness = 0;
	// psout.Reflectance = 0;

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, 0);
	psout.MotionVectors.xy = screenMotionVector.xy;
	psout.MotionVectors.zw = float2(0, 1);

	return psout;
}
