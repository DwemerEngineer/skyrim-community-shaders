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

#if defined(MID_LOD) || (defined(LOW_LOD) && !defined(FAR_LOD))
Texture2D<uint> GrassDensityTexture : register(t71);
#endif

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

#if defined(SKYLIGHTING) && !defined(FAR_LOD)
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
	float4 BladeTColor: TEXCOORD1;                       // x: actual blade parameter; yzw: stabilized base-to-tip colour
	nointerpolation uint4 PackedBladeParams: TEXCOORD2;  // facing/tilt, seed/type, root Z/width/height, f16 Far ramp/base half-width
	nointerpolation float2 RootPixel: TEXCOORD3;         // Pixel where the blade root meets the ground
#else
	float4 CameraRelativePosition: TEXCOORD0;  // xyz: camera-relative position; w: across-blade coordinate
#	if defined(HIGH_LOD)
	float4 PreviousCameraRelativePosition: TEXCOORD1;  // xyz: previous camera-relative position; w: Bezier t
#	elif defined(LOW_LOD)
	float BladeT: TEXCOORD1;
#	endif
#	if defined(HIGH_LOD)
	nointerpolation float4 WindLodDensity: TEXCOORD2;  // xy: tip wind offset; z: detail fade; w: canopy density and shadow
#	elif defined(MID_LOD)
	nointerpolation float4 WindRootPosition: TEXCOORD2;  // xy: tip wind offset; zw: root camera-relative XY
#	elif defined(LOW_LOD)
	nointerpolation float4 RootPosition: TEXCOORD2;  // xy: camera-relative blade root XY; zw: root pixel for the shadow mask
#	endif
#	if defined(MID_LOD)
	float3 BladeTDepth: TEXCOORD3;                 // x: Bezier t; y: positive view depth; z: root camera-relative height
	nointerpolation uint MaterialData: TEXCOORD7;  // clump seed/density and double-blade flag
#	elif !defined(LOW_LOD)
	float4 AOThicknessRoughness: TEXCOORD3;  // xyz: AO, thickness, roughness; w: root-relative height, or Bezier t for Mid
#	endif
#	if defined(LOW_LOD)
	nointerpolation float2 BezierTipAndMid: TEXCOORD4;  // Low reconstructs its midpoint per visible pixel.
#	else
	nointerpolation float4 BezierTipAndMid: TEXCOORD4;  // xy: tip; zw: midpoint in facing/up space
#	endif
	nointerpolation float4 BladeParams: TEXCOORD5;  // xy: facing; z: type; w: two f16 randoms
#	if !defined(LOW_LOD) && !defined(MID_LOD)
	float4 BaseToTipColor: TEXCOORD7;  // xyz: blade colour; w: positive view depth.
#	endif
#	if defined(SKYLIGHTING) && !defined(LOW_LOD)
#		if defined(MID_LOD)
	nointerpolation float3 SkylightingRoot: TEXCOORD9;  // Full-precision probe position from the generator.
#		else
	nointerpolation float4 SkylightingVertexSH: TEXCOORD9;  // Per-blade SH from the generator.
#		endif
#	endif
#endif
};

struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
#if !defined(FAR_LOD)
	float4 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
	float4 Reflectance: SV_Target5;
	float4 Masks: SV_Target6;
#endif
};

Texture2D<float4> DistantAmbientLUT : register(t73);
#if defined(LOW_LOD)
Texture2D<float> GrassSceneDepth : register(t74);
#endif
#if defined(FAR_LOD)
Texture2D<float> GrassScreenAO : register(t76);
#elif defined(HIGH_LOD)
Texture2DArray<float4> GrassMaterialDetailTexture : register(t75);
#endif

SamplerState SampGrassDetail : register(s13);
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

#if defined(LOW_LOD)
/**
 * @brief Finds the root pixel and how much nearer geometry obscures it.
 * They shade before writing depth, so their own pixels describe whatever lies behind the blade. The root pixel shows
 * the ground the blade stands on unless nearer terrain or grass covers it, and that surface's shadows do not apply.
 * @param coverWeight How surely something stands in front of the root, which within grass range is nearer grass.
 * @param distantForegroundWeight How far a nearer surface is from sharing the root's shadows at all.
 */
float2 GetDistantShadowPixel(float2 rootPixel, float3 rootPosition, out float coverWeight, out float distantForegroundWeight)
{
	rootPixel = clamp(rootPixel, 0.0f, rcp(dynamicResolutionInverted) - 1.0f);
	float rootViewDepth = dot(FrameBuffer::CameraViewProj[3], float4(rootPosition, 1.0f));
	float sceneViewDepth = SharedData::GetScreenDepth(GrassSceneDepth.Load(int3(rootPixel, 0)));
	// Rendered terrain can sit somewhat above LAND, so only clearly nearer surfaces hide the root.
	float foregroundMargin = max(64.0f, rootViewDepth * 0.03f);
	// Terrain is drawn from triangles and roots from the heightmap, so bare ground misses the root's depth slightly.
	float coverMargin = max(24.0f, rootViewDepth * 0.004f);
	coverWeight = smoothstep(coverMargin, coverMargin * 2.0f, rootViewDepth - sceneViewDepth);
	distantForegroundWeight = smoothstep(foregroundMargin * 4.0f, foregroundMargin * 8.0f, rootViewDepth - sceneViewDepth);
	return rootPixel;
}
#endif

#if defined(FAR_LOD)
/** @brief Returns the camera-relative position of the scene surface at a pixel. */
float3 GetScenePosition(int2 pixel)
{
	float2 ndc = (float2(pixel) + 0.5f) * dynamicResolutionInverted * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, GrassSceneDepth.Load(int3(pixel, 0)), 1.0f));
	return position.xyz / position.w;
}

/**
 * @brief Returns how much of the sun reaches the ground at a root, from the slope of the scene depth around it.
 * The shadow mask ends at the shadow distance, and blade lighting does not depend on which way the ground faces, so
 * without this distant grass on slopes turned away from a low sun stays fully lit while the terrain under it darkens.
 */
float GetGroundSunFacing(float2 rootPixel, float3 lightDirection)
{
	static const int SlopeStep = 4;
	int2 pixel = int2(rootPixel);
	float3 centre = GetScenePosition(pixel);
	float3 across = GetScenePosition(pixel + int2(SlopeStep, 0)) - centre;
	float3 beyond = GetScenePosition(pixel - int2(0, SlopeStep)) - centre;
	// A neighbour on another surface says nothing about this slope; leave such roots lit.
	float reach = length(centre) * 0.1f;
	if (dot(across, across) > reach * reach || dot(beyond, beyond) > reach * reach)
		return 1.0f;
	float3 groundNormal = cross(across, beyond);
	groundNormal *= groundNormal.z < 0.0f ? -1.0f : 1.0f;
	float facing = dot(groundNormal, lightDirection) * rsqrt(max(dot(groundNormal, groundNormal), 1.0e-8f));
	// Blades stand above the ground, so they keep catching the sun until the slope turns clearly away from it.
	return smoothstep(-0.1f, 0.1f, facing);
}
#endif

#if defined(SKYLIGHTING) && defined(LOW_LOD)
sh2 SampleLowSkylighting(float3 positionMS, float3 positionOffset, uint3 arrayOrigin)
{
	sh2 scaledUnitSH = Skylighting::UNIT_SH / 1e-10;
	if (SharedData::InInterior)
		return scaledUnitSH;

	positionMS.z += Skylighting::CELL_SIZE.z * 0.5f;
	float3 positionMSAdjusted = positionMS - positionOffset;
	float3 cellCoord = positionMSAdjusted / Skylighting::CELL_SIZE + float3(Skylighting::ARRAY_DIM) * 0.5f - 0.5f;
	cellCoord = clamp(cellCoord, 0.0f, float3(Skylighting::ARRAY_DIM) - 1.001f);
	int3 cell000 = int3(floor(cellCoord));
	float3 f = cellCoord - cell000;

	float largest = max(f.x, max(f.y, f.z));
	float smallest = min(f.x, min(f.y, f.z));
	float middle = max(min(f.x, f.y), min(max(f.x, f.y), f.z));
	int3 offset1 = int3(f.x >= f.y && f.x >= f.z, f.y > f.x && f.y >= f.z, f.z > f.x && f.z > f.y);
	int3 offset2 = int3(f.x >= f.y || f.x >= f.z, f.y > f.x || f.y >= f.z, f.z > f.x || f.z > f.y);
	float4 weights = float4(1.0f - largest, largest - middle, middle - smallest, smallest);

	uint3 tex000 = (uint3(cell000) + arrayOrigin) % Skylighting::ARRAY_DIM;
	uint3 tex1 = (uint3(cell000 + offset1) + arrayOrigin) % Skylighting::ARRAY_DIM;
	uint3 tex2 = (uint3(cell000 + offset2) + arrayOrigin) % Skylighting::ARRAY_DIM;
	uint3 tex111 = (uint3(cell000 + 1) + arrayOrigin) % Skylighting::ARRAY_DIM;
	return Skylighting::SkylightingProbeArray[tex000] * weights.x +
	       Skylighting::SkylightingProbeArray[tex1] * weights.y +
	       Skylighting::SkylightingProbeArray[tex2] * weights.z +
	       Skylighting::SkylightingProbeArray[tex111] * weights.w;
}
#endif

float3 GetStableClumpColor(GrassType bladeType, uint seed)
{
	float colorRandom = (float(seed) + 0.5f) * (1.0f / 256.0f);
	float valueRandom = (float((seed * 73u + 41u) & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float3 tint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, colorRandom);
	float value = 1.0f + (valueRandom * 2.0f - 1.0f) * bladeType.grassColorVar.y * 0.75f;
	return lerp(1.0f, tint * value, bladeType.clumpColorStrength);
}

float3 GetDistantAOThicknessRoughness(GrassType bladeType, float appearanceT, float clumpDensity)
{
	float roughness = lerp(bladeType.baseMinTipRoughnessStart.x, bladeType.baseMinTipRoughnessStart.y,
		smoothstep(0.0f, bladeType.baseMinTipRoughnessStart.w, appearanceT));
	roughness = lerp(roughness, bladeType.baseMinTipRoughnessStart.z,
		smoothstep(bladeType.baseMinTipRoughnessStart.x, 1.0f, appearanceT));
	float clumpAO = lerp(1.0f, bladeType.minAO, clumpDensity * bladeType.clumpAOStrength);
	return float3(lerp(bladeType.minAO, 1.0f, appearanceT) * clumpAO,
		lerp(bladeType.minMaxSubsurfaceOpacity.x, bladeType.minMaxSubsurfaceOpacity.y, appearanceT), roughness);
}

void GetDirectLightInputProcGrass(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material, float diffuseNdotL, float diffuseWrap, float detailedSpecularWeight, float3 transmissionNormal, float waxSheenStrength, float waxRoughnessMultiplier)
{
	lightingOutput = (DirectLightingOutput)0;
	const float3 detailedLightColor = context.lightColor * context.detailedShadow;
	const float3 N = context.worldNormal;
	const float3 V = context.viewDir;
	const float3 L = context.lightDir;
	const float3 H = context.halfVector;
	const float satVdotH = saturate(dot(V, H));
	const float satNdotV = saturate(abs(dot(N, V)) + EPSILON_DOT_CLAMP);
	const float satNdotH = saturate(abs(dot(N, H)));

	float horizontalHalf2 = dot(H.xy, H.xy);
	float verticalHalf2 = H.z * H.z;
	float canopyLobe = lerp(horizontalHalf2 * horizontalHalf2 * 0.375f, verticalHalf2 * verticalHalf2, 0.25f);
	canopyLobe = saturate(canopyLobe * (8.0f / 3.0f));
	canopyLobe *= lerp(0.14f, 0.055f, saturate(material.Roughness));
	float3 canopyF = BRDF::F_Schlick(material.F0, satVdotH);
	float3 canopySpecular = canopyF * canopyLobe * diffuseNdotL;

	detailedSpecularWeight = saturate(detailedSpecularWeight);
	float3 fresnel = canopyF;
	float3 directSpecular = canopySpecular;
	[branch] if (detailedSpecularWeight > 0.0f)
	{
		float detailNdotL = saturate(abs(dot(N, L)));
		float3 detailedF;
		float3 detailedSpecular = PBR::SpecularMicrofacet(material.Roughness, material.F0, detailNdotL, satNdotV, satNdotH, satVdotH, detailedF) * detailNdotL;
		// Keep the detailed tip highlight from overpowering the broad canopy response.
		detailedSpecular = min(detailedSpecular, canopySpecular * 6.0f);
		fresnel = lerp(canopyF, detailedF, detailedSpecularWeight);
		directSpecular = lerp(canopySpecular, detailedSpecular, detailedSpecularWeight);
	}

	float waxNdotL = saturate(abs(dot(N, L)));
	float waxNdotV = saturate(abs(dot(N, V)) + EPSILON_DOT_CLAMP);
	float3 waxF0 = float3(0.035f, 0.035f, 0.035f);
	float waxRoughness = saturate(material.Roughness * waxRoughnessMultiplier);
#if defined(LOW_LOD)
	float3 waxSpecular = BRDF::F_Schlick(waxF0, satVdotH) * canopyLobe * lerp(1.15f, 0.75f, waxRoughness) * waxNdotL;
#else
	float waxNdotH = saturate(abs(dot(N, H)));
	float3 waxFresnel;
	float3 waxSpecular = PBR::SpecularMicrofacet(waxRoughness, waxF0, waxNdotL, waxNdotV, waxNdotH, satVdotH, waxFresnel) * waxNdotL;
#endif
	float waxGrazing = 1.0f - waxNdotV;
	float waxWeight = waxSheenStrength * lerp(0.15f, 1.0f, smoothstep(0.10f, 0.70f, waxGrazing));
	float3 diffuseEnergy = 1.0f - fresnel;

	// Keep reflected lighting two-sided; transmission uses the signed sheet hemisphere.
	float wrappedDiffuseNdotL = saturate((diffuseNdotL + diffuseWrap) / (1.0f + diffuseWrap));
	float3 baseDiffuse = wrappedDiffuseNdotL * BRDF::Diffuse_Lambert() * diffuseEnergy;

	// Transfer diffuse energy into the wax lobe instead of adding energy.
	float3 waxTransfer = min(waxSpecular * waxWeight, baseDiffuse * 0.35f);
	lightingOutput.diffuse = (baseDiffuse - waxTransfer) * detailedLightColor;
	lightingOutput.specular = (directSpecular + waxTransfer) * detailedLightColor;

	float sheetNdotL = dot(transmissionNormal, L);
	float frontVisibility = smoothstep(-0.10f, 0.10f, sheetNdotL);
	if ((PBRFlags & PBR::Flags::Subsurface) != 0) {
		// Keep a small wrapped shoulder at grazing angles on the back hemisphere.
		float wrappedBackNdotL = saturate((-sheetNdotL + diffuseWrap * 0.35f) / (1.0f + diffuseWrap * 0.35f));
		float backVisibility = 1.0f - frontVisibility;
		wrappedBackNdotL *= backVisibility;
		float thinness = saturate(1.0f - material.Thickness);
		float forwardScatter = pow(saturate(-dot(V, L)), 4.0f);
		float transmissionAmount = wrappedBackNdotL * thinness * lerp(1.0f, 1.75f, forwardScatter);
		float3 transmissionTint = saturate(material.SubsurfaceColor * 1.20f);
		float transmissionShadow = lerp(context.softShadow, 1.0f, thinness * 0.35f);
		float3 sheetTransmission = transmissionTint * context.lightColor * transmissionShadow *
		                           BRDF::Diffuse_Lambert() * diffuseEnergy * transmissionAmount;
		lightingOutput.transmission = sheetTransmission;
	}
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
	float farWidthT = f16tof32(input.PackedBladeParams.w & 0xFFFFu);
	// Far's base occlusion sits a little under Low's and holds with distance: easing it off as the triangles widen
	// left distant grass visibly brighter than the near tiers, most of all under a low sun.
	static const float farCanopyOcclusionScale = 0.8f;
#else
	uint grassTypeIndex = (uint)input.BladeParams.z;
#endif

	GrassType bladeType = grassType[grassTypeIndex];

#if defined(FAR_LOD)
	float4 packedDirections = float4(packedFacingTilt & 0xFFu, (packedFacingTilt >> 8) & 0xFFu, (packedFacingTilt >> 16) & 0xFFu, packedFacingTilt >> 24);
	packedDirections = packedDirections * (2.0f / 255.0f) - 1.0f;

	float2 facing = packedDirections.xy;
	float2 tiltDir = packedDirections.zw;
	float3 cameraRelativePosition = input.CameraPositionSide.xyz;
	float3 previousCameraRelativePosition = cameraRelativePosition + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz);
	float viewDepth = mul(FrameBuffer::CameraView, float4(cameraRelativePosition, 1.0f)).z;

	float across = input.CameraPositionSide.w;
	float along = input.BladeTColor.x;
	float3 baseToTipColor = input.BladeTColor.yzw;
	float appearanceT = 0.25f * (along + 1.0f);

	float randHeight = bladeType.height * float(packedPositionWidthHeight & 0xFFu) * (1.0f / 255.0f);
	float2 tip = tiltDir * randHeight;
	float randBend = bladeType.stiffness * (0.25f + float((packedSeedAndType >> 20) & 0xFu) * (1.6f / 15.0f));
	float2 midPoint = tip * bladeType.mid + float2(-tip.y, tip.x) * randBend;
	// Shade the simplified geometry with the same authored curve as Low.
	float2 derivative = 2.0f * (1.0f - along) * midPoint + 2.0f * along * (tip - midPoint);
	float bladeHeight = 2.0f * (1.0f - along) * along * midPoint.y + along * along * tip.y;
	float3 sideAndBladeT = float3(across, along, bladeHeight);

	// Match Low's authored roughness curve at the shared stabilized blade sample.
	float3 aoThicknessRoughness = GetDistantAOThicknessRoughness(bladeType, appearanceT, clumpDensity);
#else
	float3 cameraRelativePosition = input.CameraRelativePosition.xyz;
#	if defined(MID_LOD) || defined(LOW_LOD)
	float3 previousCameraRelativePosition = cameraRelativePosition + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz);
#		if defined(MID_LOD)
	float along = input.BladeTDepth.x;
	float2 derivative = 2.0f * (1.0f - along) * input.BezierTipAndMid.zw + 2.0f * along * (input.BezierTipAndMid.xy - input.BezierTipAndMid.zw);
	float bladeHeight = 2.0f * (1.0f - along) * along * input.BezierTipAndMid.w + along * along * input.BezierTipAndMid.y;
#		else
	float along = input.BladeT;
	uint lowBladeData = asuint(input.BladeParams.w);
	float2 lowTip = input.BezierTipAndMid;
	float lowRandBend = bladeType.stiffness * (0.25f + float((lowBladeData >> 16) & 0xFu) * (1.6f / 15.0f));
	float2 lowMidPoint = lowTip * bladeType.mid + float2(-lowTip.y, lowTip.x) * lowRandBend;
	float2 derivative = 2.0f * (1.0f - along) * lowMidPoint + 2.0f * along * (lowTip - lowMidPoint);
	float bladeHeight = 2.0f * (1.0f - along) * along * lowMidPoint.y + along * along * lowTip.y;
#		endif
	float3 sideAndBladeT = float3(input.CameraRelativePosition.w, along, bladeHeight);
#	else
	float3 previousCameraRelativePosition = input.PreviousCameraRelativePosition.xyz;
	float3 sideAndBladeT = float3(input.CameraRelativePosition.w, input.PreviousCameraRelativePosition.w, input.AOThicknessRoughness.w);
	float2 derivative = 2.0f * (1.0f - sideAndBladeT.y) * input.BezierTipAndMid.zw + 2.0f * sideAndBladeT.y * (input.BezierTipAndMid.xy - input.BezierTipAndMid.zw);
	float along = sideAndBladeT.y;
#	endif

#	if defined(LOW_LOD)
	float appearanceT = 0.25f * (along + 1.0f);
	float clumpDensity = float(lowBladeData >> 24) * (1.0f / 255.0f);
	float3 aoThicknessRoughness = GetDistantAOThicknessRoughness(bladeType, appearanceT, clumpDensity);

	uint clumpSeed = (lowBladeData >> 8) & 0xFFu;
	float3 stableClumpColor = GetStableClumpColor(bladeType, clumpSeed);
	float3 tipDryMul = lerp(1.0f, bladeType.grassColorTipDry.rgb,
		smoothstep(0.5f, 1.0f, appearanceT) * bladeType.grassColorVar.z);
	float baseShade = lerp(1.0f - grassLightParams.w, 1.0f, smoothstep(0.0f, 0.5f, appearanceT));
	float3 baseToTipColor = lerp(bladeType.baseColor.rgb, bladeType.tipColor.rgb, appearanceT) * stableClumpColor * tipDryMul * baseShade;
#	elif defined(MID_LOD)
	float appearanceT = 0.25f * (along + 1.0f);
	float clumpDensity = float((input.MaterialData >> 8) & 0xFFu) * (1.0f / 255.0f);
	float clumpAO = lerp(1.0f, bladeType.minAO, clumpDensity * bladeType.clumpAOStrength);
	bool doubleBlade = (input.MaterialData & (1u << 16)) != 0u;
	float segmentWidth = doubleBlade ? 1.0f : 0.5f;
	float segmentStart = doubleBlade ? 0.0f : step(0.5f, along) * 0.5f;
	float2 rungT = 0.25f + 0.25f * float2(segmentStart, segmentStart + segmentWidth);
	float rungBlend = (along - segmentStart) / segmentWidth;
	float2 rungRoughness = mad(mad(bladeType.midRoughnessPolynomial.x, rungT, bladeType.midRoughnessPolynomial.y), rungT * rungT, bladeType.midRoughnessPolynomial.z);
	float3 aoThicknessRoughness = float3(lerp(bladeType.minAO, 1.0f, appearanceT) * clumpAO,
		lerp(bladeType.minMaxSubsurfaceOpacity.x, bladeType.minMaxSubsurfaceOpacity.y, appearanceT), lerp(rungRoughness.x, rungRoughness.y, rungBlend));
	uint clumpSeed = input.MaterialData & 0xFFu;
	float3 stableClumpColor = GetStableClumpColor(bladeType, clumpSeed);
	// Preserve the existing rung interpolation. Mid's appearance samples never exceed t = 0.5.
	float2 rungShade = lerp(1.0f - grassLightParams.w, 1.0f, smoothstep(0.0f, 0.5f, rungT));
	float3 rungColor0 = lerp(bladeType.baseColor.rgb, bladeType.tipColor.rgb, rungT.x) * stableClumpColor * rungShade.x;
	float3 rungColor1 = lerp(bladeType.baseColor.rgb, bladeType.tipColor.rgb, rungT.y) * stableClumpColor * rungShade.y;
	float3 baseToTipColor = lerp(rungColor0, rungColor1, rungBlend);
	float viewDepth = input.BladeTDepth.y;
#	else
	float3 aoThicknessRoughness = input.AOThicknessRoughness.xyz;
	float3 baseToTipColor = input.BaseToTipColor.xyz;
	float viewDepth = input.BaseToTipColor.w;
#	endif
	float2 facing = input.BladeParams.xy;

	float across = sideAndBladeT.x;

#	if !defined(LOW_LOD)
	uint bladeRandBits = asuint(input.BladeParams.w);
#		if defined(MID_LOD)
	float packedBladeColor = float(bladeRandBits & 0xFFFu);
	float bladeRand = round(frac(packedBladeColor * 0.61803398875f + 0.17f) * 255.0f) * (1.0f / 255.0f);
	float bladeRand2 = round(frac(packedBladeColor * 0.38196601125f + 0.61f) * 255.0f) * (1.0f / 255.0f);
#		else
	float bladeRand = f16tof32(bladeRandBits >> 16);
	float bladeRand2 = f16tof32(bladeRandBits);
#		endif
#	endif
#endif

	float2 screenUV = input.Position.xy * dynamicResolutionInverted;
	// Keep stochastic lighting samples fixed in screen space.
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, 0u);
#if defined(HIGH_LOD)
	static const float detailedSpecularWeight = 1.0f;
#	if defined(HIGH_INNER)
	float detailFade = input.WindLodDensity.z;
#	else
	static const float detailFade = 0.0f;
#	endif
#elif defined(MID_LOD)
	static const float SPECULAR_FADE_START = 4096.0f;
	static const float SPECULAR_FADE_END = 6144.0f;
	float rootDistance = float(bladeRandBits >> 16) * (6144.0f / 65535.0f);
	float detailedSpecularWeight = 1.0f - smoothstep(SPECULAR_FADE_START, SPECULAR_FADE_END, rootDistance);
	// Fade surface detail over the same range as High's generator, so Mid adds none beyond High's outer blades.
	float detailFade = 1.0f - smoothstep(512.0f, 1536.0f, rootDistance);
#else
	static const float detailedSpecularWeight = 0.0f;
#endif

#if defined(HIGH_LOD)
#	if defined(HIGH_INNER)
	uint materialVariant = min((uint)(bladeRand * 4.0f), 3u);
	uint materialSlice = grassTypeIndex * 4u + materialVariant;
	float4 materialDetail = GrassMaterialDetailTexture.SampleLevel(SampGrassDetail, float3(across, along, float(materialSlice)), 0.0f);
#	else
	static const float4 materialDetail = 0.0f;
#	endif
#endif

	float3 worldSpaceViewDirection = -normalize(cameraRelativePosition);

	float4 baseColor = float4(baseToTipColor, 1.0f);
	float4 rawRMAOS = float4(aoThicknessRoughness.z, 0.0f, aoThicknessRoughness.x, bladeType.specular);

	// Reconstruct the blade basis and curve its normal toward the visible edge.
	float3 bitangent = float3(-facing.y, facing.x, 0.0f);
	float3 tangent = float3(facing * derivative.x, derivative.y);
#if defined(HIGH_LOD)
	// Position uses windOffset * t^2, so its tangent gains the derivative 2t * windOffset.
	tangent.xy += input.WindLodDensity.xy * (2.0f * along);
#elif defined(MID_LOD)
	tangent.xy += input.WindRootPosition.xy * (2.0f * along);
#endif
	tangent = normalize(tangent);
	float3 normal = cross(-bitangent, tangent);
	float side = across * 2.0f - 1.0f;

	float3 edgeNormal = bitangent * sign(side);
	float3 curvedNormal = normalize(lerp(normal, edgeNormal, bladeType.grassVeinParams2.w * abs(side)));
	float3 worldSpaceNormal = frontFace ? curvedNormal : reflect(curvedNormal, normal);
	float3 bladePlaneNormal = normalize(normal);
	// SV_IsFrontFace follows triangle winding, which does not reliably identify the transmission hemisphere here.
	float3 transmissionNormal = dot(bladePlaneNormal, worldSpaceViewDirection) >= 0.0f ? bladePlaneNormal : -bladePlaneNormal;
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

#if defined(LOW_LOD)
	float vein = 0.0;
#elif defined(HIGH_LOD)
#	if defined(HIGH_INNER)
	static const float MaterialDetailNormalRange = 1.25f;
	float vein = materialDetail.z * detailFade;
	float veinNormalOffset = (materialDetail.w * 2.0f - 1.0f) * MaterialDetailNormalRange * detailFade;
	worldSpaceNormal = normalize(worldSpaceNormal + bitangent * veinNormalOffset);
#	else
	float vein = 0.0f;
#	endif
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

	screenSpaceNormal = normalize(FrameBuffer::WorldToView(worldSpaceNormal, false));

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
#	if defined(HIGH_INNER)
	float mottle = sin(along * 5.0 + bladeRand * Math::TAU) * 0.5 + 0.5;
	baseColor.rgb *= 1.0 + (mottle - 0.5) * 2.0 * mottleStrength * detailFade;

	float blotch = smoothstep(0.28, 0.72, materialDetail.x);
	float blotchAmount = bladeType.grassTextureParams.x * detailFade;
	float3 blotchTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, blotch);
	float blotchTintLuma = dot(blotchTint, float3(0.2126, 0.7152, 0.0722));
	blotchTint *= rcp(max(blotchTintLuma, 0.25));
	baseColor.rgb *= lerp(1.0, blotchTint, blotchAmount);
	baseColor.rgb = lerp(baseColor.rgb, baseColor.rgb * tipDryTint,
		saturate(blotch - 0.55) * blotchAmount * 0.65);

	float2 grainCoord = float2(across, along) * float2(6.0, 26.0) * bladeType.grassTextureParams.w;
	float grainFootprint = max(fwidth(grainCoord.x), fwidth(grainCoord.y));
	float grainVisibility = saturate(1.5 - grainFootprint);
	float textureFade = saturate(1.0 - viewDepth * (1.0 / 2500.0));
	speckleAmount = saturate(bladeType.grassTextureParams.z * 1.5) * textureFade * detailFade * grainVisibility;
	if (speckleAmount > 0.0) {
		speckle = saturate((materialDetail.y - 0.5) * 2.0 + 0.5);
		float grainSpot = smoothstep(0.58, 0.82, speckle);
		baseColor.rgb *= 1.0 - grainSpot * speckleAmount * 0.60;
	}
	baseColor.rgb = lerp(baseColor.rgb, baseColor.rgb * veinTint, vein * veinAlbedoStrength);
#	endif
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

	float canopyHeight01 = saturate(sideAndBladeT.z / max(bladeType.height, 1.0));
	float canopyAO = lerp(1.0 - grassLightParams.y, 1.0, canopyHeight01);

#if defined(HIGH_LOD)
	uint packedCanopyShadow = (uint)input.WindLodDensity.w;
	uint packedCanopy = packedCanopyShadow & 0xFFu;
	float canopyDensity = float(packedCanopy & 0xFu) * (1.0f / 15.0f);
	float canopyAODensity = float(packedCanopy >> 4) * (1.0f / 15.0f);
	float cachedWorldShadow = float((packedCanopyShadow >> 8) & 0xFFu) * (1.0f / 255.0f);
	canopyAO *= 1.0 - grassLightParams.x * canopyAODensity * (1.0 - canopyHeight01);
#elif defined(MID_LOD) || (defined(LOW_LOD) && !defined(FAR_LOD))
	float canopyDensity = 1.0f;
	float canopyAODensity = 0.0f;
#	if defined(MID_LOD)
	float2 densityUV = (input.WindRootPosition.zw + FrameBuffer::CameraPosAdjust.xy - occlusionParams.xy) * occlusionInvExtent + 0.5f;
#	else
	float2 densityUV = (input.RootPosition.xy + FrameBuffer::CameraPosAdjust.xy - occlusionParams.xy) * occlusionInvExtent + 0.5f;
#	endif
	if (densityUV.x == saturate(densityUV.x) && densityUV.y == saturate(densityUV.y)) {
		float bladeCount = GrassDensityTexture[uint2(densityUV * grassAOParams.x)];
		float onMapDensity = saturate(bladeCount / max(grassAOParams.z, 1.0f));
		float edgeFade = saturate(min(min(densityUV.x, 1.0f - densityUV.x), min(densityUV.y, 1.0f - densityUV.y)) * 10.0f);
		canopyDensity = lerp(1.0f, onMapDensity, edgeFade);
		canopyAODensity = onMapDensity * edgeFade;
	}
	canopyAO *= 1.0 - grassLightParams.x * canopyAODensity * (1.0 - canopyHeight01);
#else
	static const float canopyDensity = 1.0f;
#endif

	float canopyOverhead = (1.0 - canopyHeight01) * lerp(0.4, 1.0, canopyDensity);
	float canopySunShadow = exp2(-canopyOverhead * grassLightParams.z);

	float4 shadowColor = 1.0;

#if defined(FAR_LOD)
	// Remove both the straight tip offset and the base-side offset to recover the actual root for shadows.
	float baseHalfWidth = f16tof32(input.PackedBladeParams.w >> 16);
	float2 baseSideOffset = float2(-facing.y, facing.x) * (baseHalfWidth * (1.0f - along) * (across * 2.0f - 1.0f));
	float3 rootPosition = cameraRelativePosition - float3(facing * (along * tip.x) + baseSideOffset, along * tip.y);
	float rootCoverWeight;
	float rootDistantForegroundWeight;
	float2 shadowPixel = GetDistantShadowPixel(input.RootPixel, rootPosition, rootCoverWeight, rootDistantForegroundWeight);
#elif defined(LOW_LOD)
	// Low's straight blade rises t * tip, so its root lies directly below this pixel's position.
	float3 rootPosition = float3(input.RootPosition.xy, cameraRelativePosition.z - along * lowTip.y);
	float rootCoverWeight;
	float rootDistantForegroundWeight;
	float2 shadowPixel = GetDistantShadowPixel(input.RootPosition.zw, rootPosition, rootCoverWeight, rootDistantForegroundWeight);
#else
	float2 shadowPixel = input.Position.xy;
#endif
	float2 shadowUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(shadowPixel * dynamicResolutionInverted);
	shadowColor = TexShadowMaskSampler.Sample(SampShadowMaskSampler, shadowUV);
#if defined(LOW_LOD)
	// Nearer grass covering the root stands in the same shadows, so its mask value still applies. This pixel's own
	// mask value does not: it belongs to whatever lies behind the blade, which at a grazing distance is far beyond it.
	// A surface much nearer than the root says nothing about it, so the blade is left lit.
	shadowColor = lerp(shadowColor, 1.0f, rootDistantForegroundWeight);
	// The mask keeps applying beyond the loaded cells: object LOD casts there what the objects themselves cast once
	// their cells load.
#endif
#if defined(FAR_LOD)
	// Within the shadow distance the mask already holds the terrain's own shadow; the slope term takes over as Far
	// leaves the range Low covers.
	float groundSunFade = smoothstep(0.0f, 0.1f, farWidthT) * (1.0f - rootCoverWeight);
	[branch] if (groundSunFade > 0.0f)
		shadowColor.x *= lerp(1.0f, GetGroundSunFacing(shadowPixel, SharedData::DirLightDirection.xyz), groundSunFade);
#endif

#if defined(LOW_LOD)
	// Match the darker lower blades on Mid while preserving the lit tips.
	static const float LowContactOcclusionBase = 0.62f;
	// Mid's tips sit in each other's screen-space shadows, so Low's tips stay short of fully open.
	static const float LowContactOcclusionTip = 0.9f;
	float lowContactOcclusion = lerp(LowContactOcclusionBase, LowContactOcclusionTip, smoothstep(0.0f, 0.9f, along));
#	if defined(FAR_LOD)
	lowContactOcclusion = lerp(1.0f, lowContactOcclusion, farCanopyOcclusionScale);
#	endif
	rawRMAOS.z *= lowContactOcclusion;
#endif

	MaterialProperties material = (MaterialProperties)0;
	material.Noise = screenNoise;
	material.Roughness = saturate(rawRMAOS.x);
	material.Roughness = saturate(lerp(material.Roughness, 1.0, groundBlend * grassTerrainBlend.w));
	material.Metallic = saturate(rawRMAOS.y);
	// Thin surfaces should not receive the same deep crevice occlusion as solid geometry.
	material.AO = sqrt(saturate(rawRMAOS.z));
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

#if defined(SKYLIGHTING) && !defined(FAR_LOD)
	float3 positionMSSkylight = cameraRelativePosition;
#	if defined(LOW_LOD)
	sh2 skylightingSH = Skylighting::UNIT_SH;
	// Low only uses diffuse skylighting, which is fully faded outside the probe volume.
	[branch] if (!SharedData::InInterior && Skylighting::GetFadeOutFactor(positionMSSkylight) > 0.0f)
	{
		float3 probeCell = round(FrameBuffer::CameraPosAdjust.xyz / Skylighting::CELL_SIZE);
		float3 probeOffset = probeCell * Skylighting::CELL_SIZE - FrameBuffer::CameraPosAdjust.xyz;
		uint3 probeArrayOrigin = (uint3)((int3)probeCell - (int3)(Skylighting::ARRAY_DIM / 2)) % Skylighting::ARRAY_DIM;
		float3 probeExtent = Skylighting::ARRAY_SIZE * 0.5f - Skylighting::CELL_SIZE;
		float3 samplePosition = clamp(positionMSSkylight - probeOffset, -probeExtent, probeExtent) + probeOffset;
		skylightingSH = SampleLowSkylighting(samplePosition, probeOffset, probeArrayOrigin);
	}
#	elif defined(MID_LOD)
	float3 probeCell = round(FrameBuffer::CameraPosAdjust.xyz / Skylighting::CELL_SIZE);
	float3 probeOffset = probeCell * Skylighting::CELL_SIZE - FrameBuffer::CameraPosAdjust.xyz;
	uint3 probeArrayOrigin = (uint3)((int3)probeCell - (int3)(Skylighting::ARRAY_DIM / 2)) % Skylighting::ARRAY_DIM;
	float3 probeExtent = Skylighting::ARRAY_SIZE * 0.5f - Skylighting::CELL_SIZE;
	float3 samplePosition = clamp(input.SkylightingRoot - probeOffset, -probeExtent, probeExtent) + probeOffset;
	sh2 skylightingSH = Skylighting::SampleWithOrigin(samplePosition, float3(0.0f, 0.0f, 1.0f), probeOffset, probeArrayOrigin);
#	else
	sh2 skylightingSH = input.SkylightingVertexSH;
#	endif
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
#		if defined(SKYLIGHTING) && !defined(FAR_LOD)
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
	float dirDiffuseNdotL = saturate(abs(dot(worldSpaceNormal, dirLightDirection)));

	float dirDetailShadow = 1.0;
#if defined(SCREEN_SPACE_SHADOWS) && !defined(LOW_LOD)
	dirDetailShadow = ScreenSpaceShadows::GetScreenSpaceShadow(float3(shadowPixel, input.Position.z), screenUV, screenNoise);
#elif defined(SCREEN_SPACE_SHADOWS)
	// Low and Far are absent from the depth that screen-space shadows trace, so they take what terrain and objects
	// cast onto the ground at the root, the same shadows Mid shows there. Object LOD casts much the same shadows as the
	// objects it stands for, so these carry on past the loaded cells and fade where the trace's depth-space thickness
	// turns distant silhouettes into long false shadows.
	// Blades shadowing each other is reproduced statistically: some blades stay lit, some are fully shadowed, and the
	// rest are shadowed up to a random height, so lower blade sections are shadowed more often. The lit share follows
	// the sun's elevation, as neighbouring blades block more of a low sun.
	// Bare ground at the root shows exactly what is cast onto it. Where nearer grass covers the root, the texture holds
	// that grass's blades shadowing each other, which the pattern below already stands for. Reading it would make Low
	// darker wherever Mid is present than where it is not, such as before a cell loads, so covered roots stay unshadowed
	// here and the covering Mid blades carry the shadow.
	float rootDetailShadow = lerp(ScreenSpaceShadows::ScreenSpaceShadowsTexture.Load(int3(shadowPixel + 0.5f, 0)).x, 1.0f, rootCoverWeight);
	float detailShadowFade = saturate(3.0f - length(rootPosition.xy) * (2.0f / farParams.x));
	// Terrain LOD blocks are four cells wide and meet at small steps, which the trace turns into dark streaks along
	// the seam that vanish when the cells load. Beyond the loaded cells, skip the strip a seam's streak covers.
	static const float TerrainLodBlockSize = 16384.0f;
	float2 rootWorldXY = rootPosition.xy + FrameBuffer::CameraPosAdjust.xy;
	[flatten] if (any(rootWorldXY < loadedLandBounds.xy) || any(rootWorldXY > loadedLandBounds.zw))
	{
		float2 seamDistance = abs(frac(rootWorldXY * (1.0f / TerrainLodBlockSize) + 0.5f) - 0.5f) * TerrainLodBlockSize;
		detailShadowFade *= smoothstep(384.0f, 512.0f, min(seamDistance.x, seamDistance.y));
	}
	else
	{
		// Mid blades also shadow the ground between them, so inside Mid's range even an uncovered root reads blade
		// shadows that the pattern below stands for. Mid thins out across its handoff to Low, which spans the last
		// MidHandoffBand units before MidHandoffEnd; take the trace only over the outer half of that band.
		static const float MidHandoffEnd = 8192.0f;
		static const float MidHandoffBand = 2048.0f;
		float midThinning = saturate((length(rootWorldXY - grassLodOrigin) - (MidHandoffEnd - MidHandoffBand)) * (1.0f / MidHandoffBand));
		detailShadowFade *= smoothstep(0.5f, 1.0f, midThinning);
	}
	dirDetailShadow = lerp(1.0f, rootDetailShadow, detailShadowFade * (1.0f - rootDistantForegroundWeight));
	static const float BladeSunBlocking = 0.2f;
	float sunHeight = max(abs(dirLightDirection.z), 0.05f);
	// Shadowed blade sections face away from the sun, so they are hidden with the sun behind the camera and in full
	// view when facing it. Mid's screen-space shadows show the same swing, so the pattern follows the view direction.
	float sunFacingView = 1.0f - smoothstep(0.0f, 0.9f, dot(worldSpaceViewDirection, dirLightDirection));
	// Blades block more of a lower sun, up to the reach of the screen-space trace this stands in for.
	static const float BladeSunBlockingLimit = 3.0f;
	float bladeSunVisibility = exp(-BladeSunBlocking * sunFacingView * min(sqrt(saturate(1.0f - sunHeight * sunHeight)) / sunHeight, BladeSunBlockingLimit));
	float lowShadowLit = bladeSunVisibility * bladeSunVisibility;
	float lowShadowFull = 0.85f * pow(1.0f - bladeSunVisibility, 1.5f);
	float lowShadowPartial = max(1.0f - lowShadowLit - lowShadowFull, 1.0e-3f);
	static const float LowShadowMaxLine = 0.8f;
	// The clump seed and per-blade bend are stable across frames, unlike the f16 camera-relative root.
#	if defined(FAR_LOD)
	uint lowShadowSeed = (packedSeedAndType >> 8) & 0xFFu | ((packedSeedAndType >> 20) & 0xFu) << 8;
#	else
	uint lowShadowSeed = (lowBladeData >> 8) & 0xFFFu;
#	endif
	float lowShadowRandom = GrassNoiseHash(float2(lowShadowSeed, 0.0f));
	float lowShadowLine = (lowShadowRandom - lowShadowLit) * (LowShadowMaxLine / lowShadowPartial);
	lowShadowLine = lowShadowRandom >= 1.0f - lowShadowFull ? 2.0f : lowShadowLine;
	float bladeShadow = smoothstep(lowShadowLine - 0.05f, lowShadowLine + 0.05f, along);
#	if defined(FAR_LOD)
	// Widened Far triangles stand in for several blades, so ease toward the pattern's expected visibility.
	float expectedLowShadow = lowShadowLit + lowShadowPartial * saturate(along * (1.0f / LowShadowMaxLine));
	bladeShadow = lerp(bladeShadow, expectedLowShadow, smoothstep(0.0f, 1.0f, farWidthT));
#	endif
	dirDetailShadow *= bladeShadow;
#endif
#if defined(LOW_LOD)
	dirDetailShadow *= lowContactOcclusion;
#endif

#if defined(HIGH_LOD)
	float dirShadow = cachedWorldShadow;
#elif defined(MID_LOD)
	// The authored lighting curve does not describe the drawn height after Mid morphs to Low's straight profile.
	float3 shadowPosition = float3(input.WindRootPosition.zw, input.BladeTDepth.z);
	float dirShadow = ShadowSampling::GetWorldShadow(shadowPosition, FrameBuffer::CameraPosAdjust.xyz);
#else
	float dirShadow = ShadowSampling::GetWorldShadow(rootPosition, FrameBuffer::CameraPosAdjust.xyz);
#endif
	// Keep world shadow in radiance and pass canopy visibility separately for transmission.
	float dirSurfaceShadow = shadowColor.x * canopySunShadow;
	float dirDetailedVisibility = dirSurfaceShadow * dirDetailShadow;

	float3 diffuseColor = 0;

	DirectContext directContext = CreateDirectLightingContext(worldSpaceNormal, worldSpaceNormal, worldSpaceNormal, worldSpaceViewDirection, worldSpaceViewDirection, dirLightDirection, dirLightDirection, dirLightColor * dirShadow, dirDetailedVisibility, dirSurfaceShadow);
	DirectLightingOutput dirLighting;
	float bladeTipWeight = smoothstep(0.40f, 0.85f, along);
	float bladeHighlightWeight = detailedSpecularWeight * bladeTipWeight;
	GetDirectLightInputProcGrass(dirLighting, directContext, material, dirDiffuseNdotL, bladeType.grassSurfParams.z, bladeHighlightWeight, transmissionNormal, bladeType.grassSurfParams.x, bladeType.grassSurfParams.w);

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

			DirectContext pointContext = CreateDirectLightingContext(worldSpaceNormal, worldSpaceNormal, worldSpaceNormal, worldSpaceViewDirection, worldSpaceViewDirection, normalizedLightDirection, normalizedLightDirection, lightColor, lightShadow, lightShadow);

			DirectLightingOutput pointLighting = (DirectLightingOutput)0;
			float pointDiffuseNdotL = saturate(abs(dot(worldSpaceNormal, normalizedLightDirection)));

			PBR::GetDirectLightInputGrass(pointLighting, pointContext, material, false, pointDiffuseNdotL, pointDiffuseNdotL, bladeType.grassSurfParams.z);
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

	float3 directionalAmbientColor = DistantAmbientLUT.SampleLevel(SampColorSampler, GBuffer::EncodeNormal(worldSpaceNormal), 0).rgb;
	float ambientLuma = dot(directionalAmbientColor, float3(0.2126, 0.7152, 0.0722));
	directionalAmbientColor = lerp(directionalAmbientColor, ambientLuma, bladeType.grassTypeLightParams.w);

#if defined(SKYLIGHTING) && !defined(FAR_LOD)
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, positionMSSkylight, worldSpaceNormal);
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
	IndirectContext grassIndirectContext = CreateIndirectLightingContext(worldSpaceNormal, worldSpaceNormal, worldSpaceViewDirection);
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

#if defined(SKYLIGHTING) && !defined(FAR_LOD)
	Skylighting::ApplySkylighting(shadedDiffuseColor, ambientDiffuseColor, indirectLobeWeights.diffuse, skylightingDiffuse);
#endif

	float grassLightingScale = grassFrameLight.w;
	shadedDiffuseColor *= grassLightingScale;

	// The tighter specular lobe benefits from the authored AO that diffuse intentionally softens above.
	float canopySpecOcclusion = lerp(1.0, canopyAO, bladeType.grassTypeLightParams.z);
	float specOcclusion = canopySpecOcclusion * saturate(rawRMAOS.z);
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
	float densityShadow = densityShadowOuterFade;
	// The deferred pass darkens by the drawn height above terrain.
	float rootHeight = cameraRelativePosition.z - f16tof32(packedPositionWidthHeight >> 16);
	float densityShadowHeight = farCanopyOcclusionScale * (1.0f - saturate(rootHeight / max(grassAOParams.w, 1.0f)));
	psout.Diffuse.xyz *= saturate(1.0f - densityShadow * saturate(grassAOParams.y) * densityShadowHeight);

	// Far bypasses deferred composite; apply its AO the same way using the existing mask at the terrain root.
	// Grass writes no ambient mask, so the composite applies sqrt(AO) to all of its diffuse.
	// An unbound mask reads zero, preserving visibility when SSGI is disabled.
	float screenAO = 1.0f - farCanopyOcclusionScale * saturate(GrassScreenAO[uint2(shadowPixel)]);
	float3 linDiffuse = Color::IrradianceToLinear(psout.Diffuse.xyz);
	[branch] if (screenAO < 1.0f)
	{
		float3 linAlbedo = Color::IrradianceToLinear(indirectLobeWeights.diffuse / Color::PBRLightingScale);
		linDiffuse *= sqrt(MultiBounceAO(linAlbedo, screenAO));
	}

	// Far runs after deferred composite, so resolve diffuse and specular in the same order here.
	psout.Diffuse.xyz = Color::IrradianceToGamma(linDiffuse + specularColor);
#	endif
#endif

#if !defined(FAR_LOD)
	psout.Specular = float4(specularColor, psout.Diffuse.w);

	float3 outputAlbedo = indirectLobeWeights.diffuse;

	psout.Albedo = float4(outputAlbedo, psout.Diffuse.w);

#	if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	indirectLobeWeights.specular += wetnessReflectance;
	if (waterRoughnessSpecular < 1.0) {
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(wetnessNormal, false));
		pbrGlossiness = saturate(1.0 - waterRoughnessSpecular);
	}
#	endif

	psout.Reflectance = float4(indirectLobeWeights.specular * specOcclusion * lerp(0.125f, 0.5f, dirSurfaceShadow), psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), pbrGlossiness, psout.Diffuse.w);
#	if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	float wetnessNormalAmount = saturate(dot(float3(0, 0, 1), wetnessNormal) * saturate(flatnessAmount));
	psout.Masks = float4(0, 0, wetnessNormalAmount, psout.Diffuse.w);
#	else
	psout.Masks = float4(0, 0, 0, psout.Diffuse.w);
#	endif

#endif

#if !defined(FAR_LOD)
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(float4(cameraRelativePosition, 1), float4(previousCameraRelativePosition, 1));
	psout.MotionVectors.xy = screenMotionVector.xy;
	psout.MotionVectors.zw = float2(0, psout.Diffuse.w);
#endif

	return psout;
}
