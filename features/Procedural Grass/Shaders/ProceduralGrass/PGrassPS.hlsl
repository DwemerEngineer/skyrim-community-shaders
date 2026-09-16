#define PSHADER
#define DEFERRED
#define FRAMEBUFFER
#define TRUE_PBR
#define GRASS_LIGHTING

#include "Common/PBRMath.hlsli"

static const uint PBRFlags = PBR::Flags::Subsurface;

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/LightingEval.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"

SamplerState SampColorSampler : register(s0);
#define LinearSampler SampColorSampler

#include "Common/ShadowSampling.hlsli"
#include "Common/SharedData.hlsli"

#include "ProceduralGrass/PGrassCommon.hlsli"

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

#if defined(SKYLIGHTING) && defined(HIGH_LOD)
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(__INTELLISENSE__)
#	define ISL
#	define TERRAIN_SHADOWS
#	define CLOUD_SHADOWS
#	define SKYLIGHTING
#	define SCREEN_SPACE_SHADOWS
#	define WETNESS_EFFECTS
#endif

struct PS_INPUT
{
	float4 Position: SV_POSITION;
#if defined(FAR_LOD)
	float4 CameraPositionSide: TEXCOORD0;                // xyz: camera-relative position; w: across-blade coordinate
	float4 BladeTColor: TEXCOORD1;                       // x: blade parameter; yzw: base-to-tip colour
	nointerpolation uint4 PackedBladeParams: TEXCOORD2;  // facing/tilt, seed/type, root Z/width/height, continuous Far ramp
#else
	float4 CameraRelativePosition: TEXCOORD0;          // xyz: camera-relative position; w: across-blade coordinate
	float4 PreviousCameraRelativePosition: TEXCOORD1;  // xyz: previous camera-relative position; w: Bezier t
#	if defined(HIGH_LOD) || defined(MID_LOD)
	nointerpolation float2 WindOffset: TEXCOORD2;  // Current tip offset used to reconstruct the wind-bent tangent.
#	endif
	float4 AOThicknessRoughness: TEXCOORD3;             // xyz: AO, thickness, roughness; w: root-relative height
	nointerpolation float4 BezierTipAndMid: TEXCOORD4;  // xy: tip; zw: midpoint in facing/up space
	nointerpolation float4 BladeParams: TEXCOORD5;      // xy: facing; z: type; w: two f16 randoms
	float4 BaseToTipColor: TEXCOORD7;                   // xyz: blade colour; w: positive view depth.
#	if defined(SKYLIGHTING) && defined(HIGH_LOD)
	nointerpolation float4 SkylightingVertexSH: TEXCOORD9;  // Per-blade SH from the generator.
#	endif
#endif
};

struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
	float4 MotionVectors: SV_Target1;
#if !defined(FAR_LOD)
	float4 NormalGlossiness: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
	float4 Reflectance: SV_Target5;
	float4 Masks: SV_Target6;
#endif
};

Texture2D<uint> GrassDensityTexture : register(t71);
Texture2D<float4> DistantAmbientLUT : register(t73);
#if defined(FAR_LOD)
Texture2D<float> TerrainHeightTexture : register(t74);
#endif

SamplerState SampShadowMaskSampler : register(s14);

Texture2D TexShadowMaskSampler : register(t14);

/** @brief Returns a stable random value for one integer grass-detail cell. */
float GrassNoiseHash(float2 cell)
{
	return float(Random::iqint3(asuint(int2(cell)))) * (1.0f / 4294967296.0f);
}

/** @brief Bilinearly interpolates GrassNoiseHash for smooth blade-surface variation. */
float GrassValueNoise(float2 p)
{
	float2 fl = floor(p);
	float2 fr = frac(p);
	fr = fr * fr * (3.0 - 2.0 * fr);
	float a = GrassNoiseHash(fl);
	float b = GrassNoiseHash(fl + float2(1.0, 0.0));
	float c = GrassNoiseHash(fl + float2(0.0, 1.0));
	float d = GrassNoiseHash(fl + float2(1.0, 1.0));
	return lerp(lerp(a, b, fr.x), lerp(c, d, fr.x), fr.y);
}

float GetProcGrassCanopyNdotL(float3 lightDirection)
{
	float verticalBladeResponse = length(lightDirection.xy) * (2.0f / Math::PI);
	float bentTipResponse = saturate(lightDirection.z);
	return lerp(verticalBladeResponse, bentTipResponse, 0.25f);
}

float GetProcGrassFiberSpecularMask(float3 tangent, float3 halfVector, float roughness, float anisotropy)
{
	float tangentDotHalf = dot(tangent, halfVector);
	float fibreLobe = saturate(1.0 - tangentDotHalf * tangentDotHalf);
	fibreLobe *= fibreLobe;
	fibreLobe *= lerp(fibreLobe, 1.0, saturate(roughness));
	return lerp(1.0, fibreLobe, saturate(anisotropy * 2.0));
}

void GetProcGrassCanopyDirect(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material, float diffuseNdotL, float diffuseWrap, float3 tangent, float anisotropy)
{
	lightingOutput = (DirectLightingOutput)0;

	float normalDotHalf = saturate(dot(context.worldNormal, context.halfVector));
	float viewDotHalf = saturate(dot(context.viewDir, context.halfVector));
	float3 fresnel = BRDF::F_Schlick(material.F0, viewDotHalf);
	float normalDotHalf2 = normalDotHalf * normalDotHalf;
	float specularLobe = normalDotHalf2 * normalDotHalf2;
	specularLobe *= GetProcGrassFiberSpecularMask(tangent, context.halfVector, material.Roughness, anisotropy);
	specularLobe *= lerp(0.14, 0.055, saturate(material.Roughness));

	float specularNdotL = saturate(dot(context.worldNormal, context.lightDir));
	float3 detailedLightColor = context.lightColor * context.detailedShadow;
	lightingOutput.specular = detailedLightColor * specularNdotL * fresnel * specularLobe;

	float wrappedNdotL = saturate((diffuseNdotL + diffuseWrap) / (1.0 + diffuseWrap));
	float diffuseAO = MultiBounceAO(material.BaseColor, material.AO).y;
	lightingOutput.diffuse = detailedLightColor * wrappedNdotL * BRDF::Diffuse_Lambert() * saturate(1.0 - fresnel) * diffuseAO;

	if ((PBRFlags & PBR::Flags::Subsurface) != 0)
		lightingOutput.transmission = PBR::GetGrassTransmission(context, material, diffuseNdotL, 1.0);
}

void GetDirectLightInputProcGrass(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material, float diffuseNdotL, float diffuseWrap, float3 tangent, float anisotropy, float specularDetailWeight)
{
	DirectLightingOutput detailedLighting;
	PBR::GetDirectLightInputGrass(detailedLighting, context, material, specularDetailWeight > 0.0, diffuseNdotL, diffuseNdotL, diffuseWrap);
	detailedLighting.diffuse *= MultiBounceAO(material.BaseColor, material.AO).y;
	detailedLighting.specular *= GetProcGrassFiberSpecularMask(tangent, context.halfVector, material.Roughness, anisotropy) * 0.65;

	DirectLightingOutput canopyLighting;
	GetProcGrassCanopyDirect(canopyLighting, context, material, diffuseNdotL, diffuseWrap, tangent, anisotropy);

	// Keep broad lighting identical between tiers. Only the directional highlight gains blade detail nearby.
	lightingOutput.diffuse = canopyLighting.diffuse;
	lightingOutput.specular = lerp(canopyLighting.specular, detailedLighting.specular, specularDetailWeight);
	lightingOutput.transmission = canopyLighting.transmission;
	lightingOutput.coatDiffuse = 0.0;
}

PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout;

#if defined(FAR_LOD)
	uint packedFacingTilt = input.PackedBladeParams.x;
	uint packedSeedAndType = input.PackedBladeParams.y;
	uint packedPositionWidthHeight = input.PackedBladeParams.z;
	uint grassTypeIndex = packedSeedAndType & 0xFFu;
	float clumpDensity = float(packedSeedAndType >> 24) * (1.0f / 255.0f);
	float farWidthT = asfloat(input.PackedBladeParams.w);
#else
	uint grassTypeIndex = (uint)input.BladeParams.z;
#endif

	GrassType bladeType = grassType[grassTypeIndex];

#if defined(FAR_LOD)
	float4 packedDirections = float4(packedFacingTilt & 0xFFu, (packedFacingTilt >> 8) & 0xFFu, (packedFacingTilt >> 16) & 0xFFu, packedFacingTilt >> 24);
	packedDirections = packedDirections * (2.0f / 255.0f) - 1.0f;

	float2 facing = packedDirections.xy;
	float2 derivative = packedDirections.zw;  // Tangent normalization absorbs the omitted height scale.
	float3 cameraRelativePosition = input.CameraPositionSide.xyz;
	float3 previousCameraRelativePosition = cameraRelativePosition + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz);
	float viewDepth = mul(FrameBuffer::CameraView, float4(cameraRelativePosition, 1.0f)).z;

	float across = input.CameraPositionSide.w;
	float along = input.BladeTColor.x;
	float3 baseToTipColor = input.BladeTColor.yzw;

	float randHeight = bladeType.height * float(packedPositionWidthHeight & 0xFFu) * (1.0f / 255.0f);
	float bladeHeight = along * derivative.y * randHeight;
	float3 sideAndBladeT = float3(across, along, bladeHeight);

	// Match Low's authored roughness curve at the shared stabilized blade sample.
	float roughness = lerp(bladeType.baseMinTipRoughnessStart.x, bladeType.baseMinTipRoughnessStart.y,
		smoothstep(0.0f, bladeType.baseMinTipRoughnessStart.w, along));
	roughness = lerp(roughness, bladeType.baseMinTipRoughnessStart.z,
		smoothstep(bladeType.baseMinTipRoughnessStart.x, 1.0f, along));
	float clumpAO = lerp(1.0f, bladeType.minAO, clumpDensity * bladeType.clumpAOStrength);
	float3 aoThicknessRoughness = float3(lerp(bladeType.minAO, 1.0f, along) * clumpAO, lerp(bladeType.minMaxSubsurfaceOpacity.x, bladeType.minMaxSubsurfaceOpacity.y, along), roughness);

	uint bladeSeed = (packedSeedAndType >> 8) & 0xFFFFu;
	float bladeRand = (float(bladeSeed & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float bladeRand2 = (float(bladeSeed >> 8) + 0.5f) * (1.0f / 256.0f);
#else
	float3 cameraRelativePosition = input.CameraRelativePosition.xyz;
	float3 previousCameraRelativePosition = input.PreviousCameraRelativePosition.xyz;
	float3 sideAndBladeT = float3(input.CameraRelativePosition.w, input.PreviousCameraRelativePosition.w, input.AOThicknessRoughness.w);

	float3 aoThicknessRoughness = input.AOThicknessRoughness.xyz;
	float3 baseToTipColor = input.BaseToTipColor.xyz;
	float viewDepth = input.BaseToTipColor.w;
	float2 facing = input.BladeParams.xy;
	float2 derivative = 2.0f * (1.0f - sideAndBladeT.y) * input.BezierTipAndMid.zw + 2.0f * sideAndBladeT.y * (input.BezierTipAndMid.xy - input.BezierTipAndMid.zw);

	float across = sideAndBladeT.x;
	float along = sideAndBladeT.y;

	uint bladeRandBits = asuint(input.BladeParams.w);
	float bladeRand = f16tof32(bladeRandBits >> 16);
	float bladeRand2 = f16tof32(bladeRandBits);
#endif

	float2 screenUV = input.Position.xy * dynamicResolutionInverted;
	// Keep stochastic lighting samples fixed in screen space.
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, 0u);
#if defined(HIGH_LOD) || defined(MID_LOD)
	static const float SPECULAR_FADE_START = 2048.0f;
	static const float SPECULAR_FADE_END = 6144.0f;
	float2 lightingLodOffset = cameraRelativePosition.xy + FrameBuffer::CameraPosAdjust.xy - grassLodOrigin;
	float lightingLodDistance = ApproximateGrassDistance(lightingLodOffset);
	float detailedSpecularWeight = 1.0f - smoothstep(SPECULAR_FADE_START, SPECULAR_FADE_END, lightingLodDistance);
#else
	static const float detailedSpecularWeight = 0.0f;
#endif
#if !defined(LOW_LOD)
	static const float DETAIL_FADE_START = 1024.0;
	static const float DETAIL_FADE_END = 3072.0;
	float detailFade = saturate((DETAIL_FADE_END - lightingLodDistance) * (1.0 / (DETAIL_FADE_END - DETAIL_FADE_START)));
#endif

	float3 worldSpaceViewDirection = -normalize(cameraRelativePosition);

	float4 baseColor = float4(baseToTipColor, 1.0f);
	float4 rawRMAOS = float4(aoThicknessRoughness.z, 0.0f, aoThicknessRoughness.x, bladeType.specular);

	// Reconstruct the blade basis and curve its normal toward the visible edge.
	float3 bitangent = float3(-facing.y, facing.x, 0.0f);
	float3 tangent = float3(facing * derivative.x, derivative.y);
#if defined(HIGH_LOD) || defined(MID_LOD)
	// Position uses windOffset * t^2, so its tangent gains the derivative 2t * windOffset.
	tangent.xy += input.WindOffset * (2.0f * along);
#endif
	tangent = normalize(tangent);
	float3 normal = cross(-bitangent, tangent);
	float side = across * 2.0f - 1.0f;

	float3 edgeNormal = bitangent * sign(side);
	float3 curvedNormal = normalize(lerp(normal, edgeNormal, 0.4f * abs(side)));
	float3 worldSpaceNormal = frontFace ? curvedNormal : reflect(curvedNormal, normal);
	float3 screenSpaceNormal = normalize(FrameBuffer::WorldToView(worldSpaceNormal, false));

#if defined(HIGH_LOD)
	float groundProximity = 1.0 - smoothstep(0.0, max(grassTerrainBlend.y, 0.01), sideAndBladeT.z);
	float groundBlend = groundProximity * grassTerrainBlend.x;
#else
	const float groundBlend = 0.0;
#endif
	float grassOpacity = 1.0f - groundBlend;

	float3 veinTint = bladeType.grassVeinParams.rgb;
	float veinAlbedoStrength = saturate(bladeType.grassVeinParams.w * 1.20);
	float mottleStrength = bladeType.grassColorVar.w;
	float3 tipDryTint = bladeType.grassColorTipDry.rgb;
	float microDetail = bladeType.grassSurfParams.x;

#if defined(LOW_LOD)
	float vein = 0.0;
#else
	float vein = 0.0;
	[branch] if (detailFade > 0.0)
	{
		static const float MidribHalfWidth = 0.050;
		static const float LateralHalfWidth = 0.032;
		static const float LateralOffset = 0.23;
		static const float VeinRipplePeriod = 26.0;  // Ripples per blade length.
		float veinRippleDepth = bladeType.grassVeinParams2.y;

		float centerVein = 1.0 - smoothstep(0.0, MidribHalfWidth, abs(across - 0.5));
		float sideVeinL = 1.0 - smoothstep(0.0, LateralHalfWidth, abs(across - (0.5 - LateralOffset)));
		float sideVeinR = 1.0 - smoothstep(0.0, LateralHalfWidth, abs(across - (0.5 + LateralOffset)));
		vein = saturate(centerVein + 0.50 * (sideVeinL + sideVeinR));
		vein *= smoothstep(0.0, 0.16, along) * smoothstep(0.0, 0.20, 1.0 - along);

		vein *= (1.0 - veinRippleDepth) + veinRippleDepth * sin(along * VeinRipplePeriod + bladeRand * Math::TAU);
		vein *= detailFade;

		static const float WigglePeriod = 40.0;

		float veinStrength = bladeType.grassVeinParams2.x;
		float microWiggle = sin(along * WigglePeriod + bladeRand * Math::TAU) * bladeType.grassVeinParams2.z * detailFade;
		float3 veinOffset = bitangent * ((across - 0.5) * 2.0 * vein * veinStrength + microWiggle);

		worldSpaceNormal = normalize(worldSpaceNormal + veinOffset);
	}
#endif

	// Turn the base normal toward the ground plane so it shades like terrain, not an edge-on blade.
	worldSpaceNormal = normalize(lerp(worldSpaceNormal, float3(0.0, 0.0, 1.0), groundBlend * grassTerrainBlend.z));

	// Stabilize broad lighting while retaining blade shape in direct highlights.
	static const float lightingStability = 0.75f;
	static const float directLightingStability = 0.50f;
	static const float3 stableGrassNormal = float3(0.0f, 0.0f, 1.0f);
	float3 lightingNormal = normalize(lerp(worldSpaceNormal, stableGrassNormal, lightingStability));
	float3 directLightingNormal = normalize(lerp(worldSpaceNormal, stableGrassNormal, directLightingStability));
	// Fade from curved blade normals to the stable canopy normal.
	float specularNormalStability = lerp(directLightingStability, 0.0f, detailedSpecularWeight);
	float3 specularNormal = normalize(lerp(worldSpaceNormal, stableGrassNormal, specularNormalStability));
	float3 outputNormal = normalize(lerp(lightingNormal, specularNormal, detailedSpecularWeight));
	screenSpaceNormal = normalize(FrameBuffer::WorldToView(outputNormal, false));

#if !defined(LOW_LOD) && !defined(HIGH_LOD)
	[branch] if (detailFade > 0.0)
	{
		float mottle = sin(along * 5.0 + bladeRand * Math::TAU) * 0.5 + 0.5;
		baseColor.rgb *= 1.0 + (mottle - 0.5) * 2.0 * mottleStrength * detailFade;
	}
#endif

#if defined(HIGH_LOD)
	float speckle = 0.5;
	float speckleAmount = 0.0;
	[branch] if (detailFade > 0.0)
	{
		float mottle = sin(along * 5.0 + bladeRand * Math::TAU) * 0.5 + 0.5;
		baseColor.rgb *= 1.0 + (mottle - 0.5) * 2.0 * mottleStrength * detailFade;

		float2 bladeUV = float2(across, along);
		float2 noiseOffset = float2(bladeRand, bladeRand2) * 37.0;
		float blotch = smoothstep(0.28, 0.72,
			GrassValueNoise(bladeUV * float2(1.0, 4.0) * bladeType.grassTextureParams.y + noiseOffset));
		float blotchAmount = bladeType.grassTextureParams.x * detailFade;
		float3 blotchTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, blotch);
		float blotchTintLuma = dot(blotchTint, float3(0.2126, 0.7152, 0.0722));
		blotchTint *= rcp(max(blotchTintLuma, 0.25));
		baseColor.rgb *= lerp(1.0, blotchTint, blotchAmount);
		baseColor.rgb = lerp(baseColor.rgb, baseColor.rgb * tipDryTint,
			saturate(blotch - 0.55) * blotchAmount * 0.65);

		float2 grainCoord = bladeUV * float2(6.0, 26.0) * bladeType.grassTextureParams.w;
		// Keep pixel-scale grain separate from the generator's per-blade colour.
		speckle = saturate((GrassValueNoise(grainCoord + noiseOffset * 1.7) - 0.5) * 2.0 + 0.5);
		float grainSpot = smoothstep(0.58, 0.82, speckle);
		float grainFootprint = max(fwidth(grainCoord.x), fwidth(grainCoord.y));
		float grainVisibility = saturate(1.5 - grainFootprint);
		float textureFade = saturate(1.0 - viewDepth * (1.0 / 2500.0));
		speckleAmount = saturate(bladeType.grassTextureParams.z * 1.5) * textureFade * detailFade * grainVisibility;
		baseColor.rgb *= 1.0 - grainSpot * speckleAmount * 0.60;
		baseColor.rgb = lerp(baseColor.rgb, baseColor.rgb * veinTint, vein * veinAlbedoStrength);
	}
#elif defined(LOW_LOD)
	float speckle = 0.5;
	float speckleAmount = 0.0;
#else
	float speckle = 0.5;
	float speckleAmount = 0.0;
	[branch] if (detailFade > 0.0)
	{
		// Broad surface variation is generated per blade; retain only pixel-dependent vein shaping here.
		float blotch = saturate((bladeRand - 0.5) * 1.5 + 0.5);
		baseColor.rgb *= 1.0 + (blotch - 0.5) * 2.0 * bladeType.grassTextureParams.x * detailFade;
		baseColor.rgb = lerp(baseColor.rgb, baseColor.rgb * tipDryTint, saturate(blotch - 0.55) * bladeType.grassTextureParams.x * detailFade);

		float textureFade = saturate(1.0 - viewDepth * (1.0 / 2500.0));
		speckle = saturate((bladeRand2 - 0.5) * 2.0 + 0.5);
		speckleAmount = bladeType.grassTextureParams.z * textureFade * detailFade;
		baseColor.rgb *= 1.0 + (speckle - 0.5) * 2.0 * speckleAmount;
		baseColor.rgb = lerp(baseColor.rgb, baseColor.rgb * veinTint, vein * veinAlbedoStrength);
	}
#endif

	// Determine the authored color in display space, then convert it once for Linear Lighting.
	baseColor.rgb = Color::ColorToLinear(baseColor.rgb);

	// Flatten ambient normals toward world-up to reduce per-blade noise.
	float3 ambientNormal = normalize(lerp(lightingNormal, float3(0.0, 0.0, 1.0), bladeType.grassSurfParams.y));

	float canopyHeight01 = saturate(sideAndBladeT.z / max(bladeType.height, 1.0));
	float canopyAO = lerp(1.0 - grassLightParams.y, 1.0, canopyHeight01);
	float canopyDensity = 1.0;  // Full density is the off-map default.

#if !defined(LOW_LOD)
	// Low/Far use the off-map default rather than sampling this finite density field.
	float3 worldPosAO = cameraRelativePosition + FrameBuffer::CameraPosAdjust.xyz;
	float2 densityUVAO = (worldPosAO.xy - occlusionParams.xy) * occlusionInvExtent + 0.5;

	if (densityUVAO.x == saturate(densityUVAO.x) && densityUVAO.y == saturate(densityUVAO.y))

	{
		float bladeCount = GrassDensityTexture[uint2(densityUVAO * grassAOParams.x)];
		float onMapDensity = saturate(bladeCount / max(grassAOParams.z, 1.0));
		float edgeFade = saturate(min(min(densityUVAO.x, 1.0 - densityUVAO.x), min(densityUVAO.y, 1.0 - densityUVAO.y)) * 10.0);

		canopyDensity = lerp(1.0, onMapDensity, edgeFade);
		canopyAO *= 1.0 - grassLightParams.x * onMapDensity * (1.0 - canopyHeight01) * edgeFade;
	}
#endif

	float canopyOverhead = (1.0 - canopyHeight01) * lerp(0.4, 1.0, canopyDensity);
	float canopySunShadow = exp2(-canopyOverhead * grassLightParams.z);

	float4 shadowColor = 1.0;

	float2 adjustedShadowUV = screenUV;
	float2 shadowUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(adjustedShadowUV);
	shadowColor = TexShadowMaskSampler.Sample(SampShadowMaskSampler, shadowUV);

	MaterialProperties material = (MaterialProperties)0;
	material.Noise = screenNoise;
	material.Roughness = saturate(rawRMAOS.x);
	material.Roughness = lerp(material.Roughness, material.Roughness * 0.68, vein * 0.35);

#if !defined(LOW_LOD)
	[branch] if (detailFade > 0.0)
		material.Roughness = saturate(material.Roughness + (bladeRand2 - 0.5) * 0.5 * microDetail * detailFade);
#endif

	material.Roughness = saturate(material.Roughness + (speckle - 0.5) * 0.5 * speckleAmount);
	material.Roughness = saturate(lerp(material.Roughness, 1.0, groundBlend * grassTerrainBlend.w));
	material.Metallic = saturate(rawRMAOS.y);
	material.AO = rawRMAOS.z;
	material.F0 = lerp(saturate(rawRMAOS.w), Color::IrradianceToLinear(baseColor.xyz), material.Metallic);
	material.F0 = lerp(material.F0, material.F0 * 1.12, vein * 0.25);
	baseColor.xyz *= 1 - material.Metallic;
	material.BaseColor = baseColor.xyz;
	// Scale transmission by albedo so dark blades remain energy bounded.
	material.SubsurfaceColor = saturate(bladeType.grassSubsurfaceColor.rgb * baseColor.rgb * bladeType.grassTypeLightParams.y);
	material.Thickness = aoThicknessRoughness.y;

	float3 specularColorPBR = 0;
	float3 transmissionColor = 0;
	float pbrGlossiness = 1 - material.Roughness;

#if defined(SKYLIGHTING) && defined(HIGH_LOD)
	float3 positionMSSkylight = cameraRelativePosition;
	// The generator provides per-blade SH; evaluate it here with the pixel's ambient normal.
	sh2 skylightingSH = input.SkylightingVertexSH;
#endif

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewDepth);
	float waterHeight = SharedData::GetWaterData(cameraRelativePosition).w;
	float waterRoughnessSpecular = 1.0;
	float wetness = 0.0;
	float wetnessDistToWater = abs(cameraRelativePosition.z - waterHeight);
	float shoreFactor = saturate(1.0 - (wetnessDistToWater / (float)SharedData::wetnessEffectsSettings.ShoreRange));
	float shoreFactorAlbedo = shoreFactor;

	[flatten] if (cameraRelativePosition.z < waterHeight)
		shoreFactorAlbedo = 1.0;

	float minWetnessValue = SharedData::wetnessEffectsSettings.MinRainWetness;
	float minWetnessAngle = 0;
	minWetnessAngle = saturate(max(minWetnessValue, worldSpaceNormal.z));

#	if !defined(PGRASS_DRY_WETNESS)
#		if defined(SKYLIGHTING) && defined(HIGH_LOD)
	float wetnessOcclusion = saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1)));
	wetnessOcclusion *= wetnessOcclusion;
#		else
	float wetnessOcclusion = 1;
#		endif

	float4 raindropInfo = float4(0, 0, 1, 0);
	if (worldSpaceNormal.z > 0 && SharedData::wetnessEffectsSettings.Raining > 0.0f && SharedData::wetnessEffectsSettings.EnableRaindropFx) {
		float4 precipOcclusionTexCoord = mul(SharedData::wetnessEffectsSettings.OcclusionViewProj, float4(cameraRelativePosition, 1));
		precipOcclusionTexCoord.y = -precipOcclusionTexCoord.y;
		float2 precipOcclusionUV = precipOcclusionTexCoord.xy * 0.5 + 0.5;

		if (saturate(precipOcclusionUV.x) == precipOcclusionUV.x && saturate(precipOcclusionUV.y) == precipOcclusionUV.y) {
			float precipOcclusionZ = WetnessEffects::TexPrecipOcclusion.SampleLevel(SampColorSampler, precipOcclusionUV, 0).x;

			if (precipOcclusionTexCoord.z < precipOcclusionZ + 0.1)
				raindropInfo = WetnessEffects::GetRainDrops(cameraRelativePosition + FrameBuffer::CameraPosAdjust.xyz, SharedData::wetnessEffectsSettings.Time, worldSpaceNormal);
		}
	}

	float rainWetness = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
	rainWetness = max(rainWetness, raindropInfo.w);

	float puddleWetness = SharedData::wetnessEffectsSettings.PuddleWetness * minWetnessAngle;

	rainWetness *= wetnessOcclusion;
	puddleWetness *= wetnessOcclusion;

	wetness = max(shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness, rainWetness);
#	else
	wetness = shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness;
#	endif

	float3 wetnessNormal = worldSpaceNormal;

	float3 puddleCoords = ((cameraRelativePosition + FrameBuffer::CameraPosAdjust.xyz) * 0.5 + 0.5) * 0.01 / SharedData::wetnessEffectsSettings.PuddleRadius;
	float puddle = wetness;

#	if defined(PGRASS_DRY_WETNESS)
	bool needsPuddleNoise = wetness > 0.0;
#	else
	bool needsPuddleNoise = wetness > 0.0 || puddleWetness > 0.0;
#	endif

	if (needsPuddleNoise) {
		puddle = GrassValueNoise(puddleCoords.xy);
		puddle = puddle * ((minWetnessAngle / SharedData::wetnessEffectsSettings.PuddleMaxAngle) * SharedData::wetnessEffectsSettings.MaxPuddleWetness * 0.25) + 0.5;
#	if defined(PGRASS_DRY_WETNESS)
		wetness = lerp(wetness, 0.0, saturate(puddle - 0.25));
#	else
		wetness = lerp(wetness, puddleWetness, saturate(puddle - 0.25));
#	endif
		puddle *= wetness;
	}

	puddle *= nearFactor;

	float wetnessGlossinessAlbedo = max(puddle, shoreFactorAlbedo * SharedData::wetnessEffectsSettings.MaxShoreWetness);
	wetnessGlossinessAlbedo *= wetnessGlossinessAlbedo;

	float wetnessGlossinessSpecular = puddle;
	wetnessGlossinessSpecular = lerp(wetnessGlossinessSpecular, wetnessGlossinessSpecular * shoreFactor, cameraRelativePosition.z < waterHeight);

	float flatnessAmount = smoothstep(SharedData::wetnessEffectsSettings.PuddleMaxAngle, 1.0, minWetnessAngle);

	flatnessAmount *= smoothstep(SharedData::wetnessEffectsSettings.PuddleMinWetness, 1.0, wetnessGlossinessSpecular);

	wetnessNormal = normalize(lerp(wetnessNormal, float3(0, 0, 1), flatnessAmount));

#	if !defined(PGRASS_DRY_WETNESS)
	float3 rippleNormal = normalize(lerp(float3(0, 0, 1), raindropInfo.xyz, lerp(1.0, flatnessAmount, 0.8)));
	wetnessNormal = ReorientNormal(rippleNormal, wetnessNormal);
#	endif

	waterRoughnessSpecular = 1.0 - wetnessGlossinessSpecular * 0.9;
#endif

	float3 dirLightColor = grassFrameLight.xyz;
	float3 dirLightDirection = SharedData::DirLightDirection.xyz;
	float dirDiffuseNdotL = GetProcGrassCanopyNdotL(dirLightDirection);

	float dirDetailShadow = 1.0;
#if defined(SCREEN_SPACE_SHADOWS) && !defined(LOW_LOD)
	[branch] if (detailFade > 0.0)
		dirDetailShadow = lerp(1.0, ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, screenUV, screenNoise), detailFade);
#endif

	float dirShadow = ShadowSampling::GetWorldShadow(cameraRelativePosition, FrameBuffer::CameraPosAdjust.xyz);
	float dirLightColorMultiplier = shadowColor.x * dirShadow * canopySunShadow;

	float3 diffuseColor = 0;

	DirectContext directContext = CreateDirectLightingContext(specularNormal, specularNormal, specularNormal, worldSpaceViewDirection, worldSpaceViewDirection, dirLightDirection, dirLightDirection, dirLightColor * dirLightColorMultiplier, dirDetailShadow, dirDetailShadow);
	DirectLightingOutput dirLighting;
	GetDirectLightInputProcGrass(
		dirLighting, directContext, material, dirDiffuseNdotL, bladeType.grassSurfParams.z,
		tangent, bladeType.grassSurfParams.w, detailedSpecularWeight);

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
#	if defined(MID_LOD)
	if (detailedSpecularWeight > 0.0 && waterRoughnessSpecular < 1.0)
#	else
	if (waterRoughnessSpecular < 1.0)
#	endif
		EvaluateWetnessLighting(wetnessNormal, directContext, waterRoughnessSpecular, dirLighting);
#endif

	diffuseColor += dirLighting.diffuse;
	transmissionColor += dirLighting.transmission;
	specularColorPBR += dirLighting.specular;

#if defined(LIGHT_LIMIT_FIX) && !defined(LOW_LOD)
	uint numClusteredLights = 0;
#	if !defined(PGRASS_NO_LOCAL_LIGHTS)
	if (detailedSpecularWeight > 0.0) {
	uint totalLightCount = LightLimitFix::NumStrictLights;
	uint clusterIndex = 0;
	uint lightOffset = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewDepth, clusterIndex)) {
		numClusteredLights = LightLimitFix::lightGrid[clusterIndex].lightCount;
		totalLightCount += numClusteredLights;
		lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
	}

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

		float3 lightDirection = light.positionWS.xyz - cameraRelativePosition;
		float distSq = dot(lightDirection, lightDirection);

#		if defined(ISL)
		float lightDist = sqrt(distSq);
		float intensityMultiplier = InverseSquareLighting::GetAttenuation(lightDist, light);
		if (intensityMultiplier < 1e-5)
			continue;
		float3 normalizedLightDirection = lightDirection * rcp(max(lightDist, 1e-5));
#		else
		float radiusSq = light.radius * light.radius;
		if (distSq >= radiusSq)
			continue;
		float intensityMultiplier = 1 - distSq / radiusSq;
		float3 normalizedLightDirection = lightDirection * rsqrt(max(distSq, 1e-10));
#		endif

		const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
		float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * intensityMultiplier * light.fade;
		float lightShadow = 1.0;
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow)
			lightShadow = shadowColor[light.shadowLightIndex];

		DirectContext pointContext = CreateDirectLightingContext(directLightingNormal, directLightingNormal, directLightingNormal, worldSpaceViewDirection, worldSpaceViewDirection, normalizedLightDirection, normalizedLightDirection, lightColor, lightShadow, lightShadow);

		DirectLightingOutput pointLighting = (DirectLightingOutput)0;
		float pointDiffuseNdotL = GetProcGrassCanopyNdotL(normalizedLightDirection);

		PBR::GetDirectLightInputGrass(pointLighting, pointContext, material, false, pointDiffuseNdotL, pointDiffuseNdotL, bladeType.grassSurfParams.z);
		pointLighting.diffuse *= MultiBounceAO(material.BaseColor, material.AO).y;
#		if defined(WETNESS_EFFECTS)
		if (waterRoughnessSpecular < 1.0)
			EvaluateWetnessLighting(wetnessNormal, pointContext, waterRoughnessSpecular, pointLighting);
#		endif
		diffuseColor += pointLighting.diffuse * detailedSpecularWeight;
		transmissionColor += pointLighting.transmission * detailedSpecularWeight;
		specularColorPBR += pointLighting.specular * detailedSpecularWeight;
	}
	}
#	endif
#endif

	float3 directionalAmbientColor = DistantAmbientLUT.SampleLevel(SampColorSampler, GBuffer::EncodeNormal(ambientNormal), 0).rgb;
	float ambientLuma = dot(directionalAmbientColor, float3(0.2126, 0.7152, 0.0722));
	directionalAmbientColor = lerp(directionalAmbientColor, ambientLuma, bladeType.grassTypeLightParams.w);

#if defined(SKYLIGHTING) && defined(HIGH_LOD)
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, positionMSSkylight, ambientNormal);
	// Fade skylighting to neutral where the generator stops sampling probes.
	skylightingDiffuse = lerp(1.0, skylightingDiffuse, detailFade);
#endif

	directionalAmbientColor *= canopyAO;

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	[branch] if (wetnessGlossinessAlbedo > 0.0)
	{
		float porosity = 1.0 - saturate(sqrt(material.Metallic));
		float wetnessDarkeningAmount = porosity * wetnessGlossinessAlbedo;
		float3 wetBaseColor = Color::LLLinearToGamma(baseColor.xyz);
		wetBaseColor = lerp(wetBaseColor, pow(abs(wetBaseColor), 1.0 + wetnessDarkeningAmount), 0.8);
		baseColor.xyz = Color::LLGammaToLinear(wetBaseColor);
	}
#endif

	material.BaseColor = baseColor.xyz;
	IndirectLobeWeights indirectLobeWeights = (IndirectLobeWeights)0;
	IndirectContext grassIndirectContext = CreateIndirectLightingContext(lightingNormal, lightingNormal, worldSpaceViewDirection);
	PBR::GetIndirectLobeWeightsGrass(indirectLobeWeights, grassIndirectContext, material, true);

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	float3 wetnessReflectance = 0.0;
	[branch] if (waterRoughnessSpecular < 1.0)
	{
		IndirectContext indirectContext = CreateIndirectLightingContext(worldSpaceNormal, worldSpaceNormal, worldSpaceViewDirection);
		wetnessReflectance = GetWetnessIndirectLobeWeights(indirectLobeWeights, wetnessNormal, waterRoughnessSpecular, indirectContext);
	}
#endif

	// Direct irradiance uses albedo; ambient already contains the full indirect lobe.
	float3 ambientDiffuseColor = directionalAmbientColor * indirectLobeWeights.diffuse;
	float3 shadedDiffuseColor = diffuseColor.xyz * baseColor.xyz + ambientDiffuseColor + transmissionColor;
	shadedDiffuseColor += directionalAmbientColor * bladeType.grassBounceColor.rgb * bladeType.grassTypeLightParams.x * (1.0 - canopyHeight01) * baseColor.xyz;

#if defined(SKYLIGHTING) && defined(HIGH_LOD)
	Skylighting::ApplySkylighting(shadedDiffuseColor, ambientDiffuseColor, indirectLobeWeights.diffuse, skylightingDiffuse);
#endif

	float grassLightingScale = grassFrameLight.w;
	shadedDiffuseColor *= grassLightingScale;

	float specOcclusion = lerp(1.0, canopyAO, bladeType.grassTypeLightParams.z);
	specularColorPBR *= specOcclusion;
	specularColorPBR *= grassLightingScale;
	// Match the PBR scale removed from the deferred albedo buffer.
	indirectLobeWeights.diffuse *= grassPBRLightingScale;
	float3 specularColor = specularColorPBR;

	psout.Diffuse.w = grassOpacity;

#if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			psout.Diffuse.xyz = Color::TurboColormap(LightLimitFix::NumStrictLights >= 7.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			psout.Diffuse.xyz = Color::TurboColormap((float)LightLimitFix::NumStrictLights / 15.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 2) {
			psout.Diffuse.xyz = Color::TurboColormap((float)numClusteredLights / MAX_CLUSTER_LIGHTS);
		} else {
			psout.Diffuse.xyz = shadowColor.xyz;
		}
		baseColor.xyz = 0.0;
	} else {
		psout.Diffuse.xyz = shadedDiffuseColor;
	}
#else
	psout.Diffuse.xyz = shadedDiffuseColor;
#	if defined(FAR_LOD)
	// Match the deferred density shadow without its four integer density reads.
	float densityShadowOuterStart = saturate(1.0f - 4096.0f * farParams.y);
	float densityShadowOuterFade = 1.0f - smoothstep(densityShadowOuterStart, 1.0f, farWidthT);
	float densityShadow = lerp(1.0f, 0.875f, smoothstep(0.0f, 0.2f, farWidthT)) * densityShadowOuterFade;
	float densityShadowHeight = 1.0f - 0.75f * saturate(sideAndBladeT.z / max(grassAOParams.w, 1.0f));
	if (heightMapScale.x != 0.0f && heightMapScale.y != 0.0f) {
		float3 worldPosition = cameraRelativePosition + FrameBuffer::CameraPosAdjust.xyz;
		float terrainZ = lerp(heightMapZRange.x, heightMapZRange.y,
			TerrainHeightTexture.SampleLevel(LinearSampler, worldPosition.xy * heightMapScale + heightMapOffset, 0));
		densityShadowHeight = 1.0f - 0.75f * saturate((worldPosition.z - terrainZ) / max(grassAOParams.w, 1.0f));
	}
	psout.Diffuse.xyz *= saturate(1.0f - densityShadow * saturate(grassAOParams.y) * densityShadowHeight);

	// Far runs after deferred composite, so resolve diffuse and specular in the same order here.
	psout.Diffuse.xyz = Color::IrradianceToGamma(Color::IrradianceToLinear(psout.Diffuse.xyz) + specularColor);
#	endif
#endif

#if !defined(FAR_LOD)
	psout.Specular = float4(specularColor, psout.Diffuse.w);

	float3 outputAlbedo = indirectLobeWeights.diffuse;

	psout.Albedo = float4(outputAlbedo, psout.Diffuse.w);

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	indirectLobeWeights.specular += wetnessReflectance;
	if (waterRoughnessSpecular < 1.0) {
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(wetnessNormal, false));
		pbrGlossiness = saturate(1.0 - waterRoughnessSpecular);
	}
#endif

	psout.Reflectance = float4(indirectLobeWeights.specular * specOcclusion, psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), pbrGlossiness, psout.Diffuse.w);
#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	float wetnessNormalAmount = saturate(dot(float3(0, 0, 1), wetnessNormal) * saturate(flatnessAmount));
	psout.Masks = float4(0, 0, wetnessNormalAmount, psout.Diffuse.w);
#else
	psout.Masks = float4(0, 0, 0, psout.Diffuse.w);
#endif

#endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(float4(cameraRelativePosition, 1), float4(previousCameraRelativePosition, 1));
	psout.MotionVectors.xy = screenMotionVector.xy;
	psout.MotionVectors.zw = float2(0, psout.Diffuse.w);

	return psout;
}
