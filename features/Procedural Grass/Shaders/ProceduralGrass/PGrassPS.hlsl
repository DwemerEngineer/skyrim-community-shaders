#define PSHADER
#define DEFERRED
#define FRAMEBUFFER
#define TRUE_PBR
#define GRASS_LIGHTING

// Outer High and Mid blades span a few pixels: they light fuzz on the specular normal, use an isotropic specular lobe,
// and reuse the front ambient and skylighting samples, differences their pixels cannot resolve.
#if defined(MID_LOD) || (defined(HIGH_LOD) && !defined(HIGH_INNER))
#	define PGRASS_DISTANT_LIGHTING
#endif

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

#include "ProceduralGrass/PGrassTierIO.hlsli"

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
	float4 Masks2: SV_Target7;
#endif
};

Texture2D<float4> DistantAmbientLUT : register(t73);
Texture2D<float> GrassSceneDepth : register(t74);
#if defined(FAR_LOD)
Texture2D<float> GrassScreenAO : register(t76);
#elif defined(HIGH_LOD)
Texture2DArray<float4> GrassMaterialDetailTexture : register(t75);
#endif

SamplerState SampGrassDetail : register(s13);
SamplerState SampShadowMaskSampler : register(s14);

Texture2D TexShadowMaskSampler : register(t14);

#include "ProceduralGrass/PGrassMaterial.hlsli"
#include "ProceduralGrass/PGrassLighting.hlsli"

#if defined(FAR_LOD)
#	include "ProceduralGrass/PGrassFarLighting.hlsli"
#endif

/** @brief Debug colour for the tier this permutation draws: High inner red, High outer orange, Mid yellow, Low green, Far blue. */
float3 GetTierDebugColor()
{
#if defined(FAR_LOD)
	return float3(0.1f, 0.35f, 1.0f);
#elif defined(LOW_LOD)
	return float3(0.1f, 0.8f, 0.15f);
#elif defined(MID_LOD)
	return float3(0.95f, 0.85f, 0.05f);
#elif defined(HIGH_INNER)
	return float3(0.95f, 0.08f, 0.05f);
#else
	return float3(1.0f, 0.45f, 0.0f);
#endif
}

#if defined(HIGH_LOD) || defined(MID_LOD)
// High and Mid colour passes only read depth, allowing rejection before blade shading.
[earlydepthstencil]
#endif
PS_OUTPUT main(GrassTierIO input, bool frontFace : SV_IsFrontFace)
{
#if defined(HIGH_LOD)
	// Terrain Blending offsets the hardware depth; High is close enough for the offset to show, so test the visible
	// surface before shading. High blends every target by its alpha, so zero opacity leaves them unchanged; a discard
	// would do the same but costs these passes far more. Mid, Low and Far keep the hardware test alone.
	float sceneHidden = GrassSceneDepth.Load(int3(input.Position.xy, 0)) + (2.0f / 16777215.0f) < input.Position.z ? 1.0f : 0.0f;

	// Skip shading only when the whole 2x2 quad is hidden, so derivatives stay valid for partly hidden quads.
	float2 quadSide = frac(input.Position.xy * 0.5f) < 0.5f ? 1.0f : -1.0f;
	float rowHidden = 2.0f * sceneHidden + quadSide.x * ddx_fine(sceneHidden);
	float quadHidden = 2.0f * rowHidden + quadSide.y * ddy_fine(rowHidden);
	[branch] if (quadHidden > 3.5f)
	{
		return (PS_OUTPUT)0;
	}
#endif
	PS_OUTPUT psout;

#if defined(FAR_LOD)
	uint packedFacingTilt = input.PackedBladeParams.x;
	uint packedSeedAndType = input.PackedBladeParams.y;
	uint packedPositionWidthHeight = input.PackedBladeParams.z;
	uint grassTypeIndex = packedSeedAndType & 0x7Fu;
	float clumpDensity = float(packedSeedAndType >> 28) * (1.0f / 15.0f);
	float3 farTerrainNormal = UnpackFarTerrainNormal(packedSeedAndType);
	float farWidthT = f16tof32(input.PackedBladeParams.w & 0xFFFFu);
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

	// Match the VS: Far grows its blades as it thins, so shading measures height against the authored blade.
	float farHeightScale = GetFarHeightScale(cameraRelativePosition.xy + FrameBuffer::CameraPosAdjust.xy - grassLodOrigin, FrameBuffer::CameraProj._m00);
	float randHeight = bladeType.height * float(packedPositionWidthHeight & 0xFFu) * (1.0f / 255.0f) * farHeightScale;
	float2 tip = tiltDir * randHeight;
	float randBend = bladeType.stiffness * (0.25f + float((packedSeedAndType >> 20) & 0xFu) * (1.6f / 15.0f));
	float2 midPoint = tip * bladeType.mid + float2(-tip.y, tip.x) * randBend;

	// Shade the simplified geometry with the same authored curve as Low.
	float2 derivative = 2.0f * (1.0f - along) * midPoint + 2.0f * along * (tip - midPoint);
	// Distant Far blades show mostly their tips behind the blades in front, which would light them brighter than nearer
	// tiers that show whole blades. Their height-dependent occlusion uses a whole blade's mean height instead.
	float farShadeAlong = lerp(along, 0.5f, smoothstep(0.0f, 1.0f, farWidthT));
	float3 sideAndBladeT = float3(across, along, farShadeAlong * tip.y / farHeightScale);

	// Match Low's authored roughness curve at the shared stabilized blade sample.
	float3 aoThicknessRoughness = GetDistantAOThicknessRoughness(bladeType, appearanceT, clumpDensity, farShadeAlong);
#else
	float3 cameraRelativePosition = input.CameraRelativePosition.xyz;
#	if defined(MID_LOD) || defined(LOW_LOD)
	float3 previousCameraRelativePosition = cameraRelativePosition + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz);
#		if defined(MID_LOD)
	float along = input.BladeTDepth.x;
	float2 derivative = 2.0f * (1.0f - along) * input.BezierTipAndMid.zw + 2.0f * along * (input.BezierTipAndMid.xy - input.BezierTipAndMid.zw);

	// Canopy lighting follows the drawn height, including Mid's morph toward Low.
	float bladeHeight = cameraRelativePosition.z - input.BladeTDepth.z;
#		else
	float along = input.BladeT;
	uint lowBladeData = asuint(input.BladeParams.w);
	float2 lowTip = input.BezierTipAndMid;
	float lowRandBend = bladeType.stiffness * (0.25f + float((lowBladeData >> 16) & 0xFu) * (1.6f / 15.0f));
	float2 lowMidPoint = lowTip * bladeType.mid + float2(-lowTip.y, lowTip.x) * lowRandBend;
	float2 derivative = 2.0f * (1.0f - along) * lowMidPoint + 2.0f * along * (lowTip - lowMidPoint);
	float bladeHeight = along * lowTip.y;
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
	float3 aoThicknessRoughness = GetDistantAOThicknessRoughness(bladeType, appearanceT, clumpDensity, along);

	uint clumpSeed = (lowBladeData >> 8) & 0xFFu;
	float3 stableClumpColor = GetStableClumpColor(bladeType, clumpSeed);
	float3 baseToTipColor = GetDistantBladeColor(bladeType, stableClumpColor, along);

#	elif defined(MID_LOD)
	float appearanceT = 0.25f * (along + 1.0f);
	float rootDistance = float(asuint(input.BladeParams.w) >> 16) * (6144.0f / 65535.0f);
	float lowMaterialBlend = GetMidLowBlend(rootDistance);
	float clumpDensity = float((input.MaterialData >> 8) & 0xFFu) * (1.0f / 255.0f);
	bool doubleBlade = (input.MaterialData & (1u << 16)) != 0u;

	float segmentWidth = doubleBlade ? 1.0f : 0.5f;
	float segmentStart = doubleBlade ? 0.0f : step(0.5f, along) * 0.5f;

	// Match outer High's material interpolation at the original geometry rungs.
	float2 rungAlong = float2(segmentStart, segmentStart + segmentWidth);
	float2 rungT = 0.25f + 0.25f * rungAlong;
	float rungBlend = (along - segmentStart) / segmentWidth;
	float2 rungRoughness = GetDistantRoughness(bladeType, rungT);
	float2 rungAO = lerp(bladeType.minAO, 1.0f, rungAlong) * float2(GetClumpAO(bladeType, clumpDensity, rungAlong.x), GetClumpAO(bladeType, clumpDensity, rungAlong.y));
	float3 aoThicknessRoughness = float3(lerp(rungAO.x, rungAO.y, rungBlend),
		lerp(bladeType.minMaxSubsurfaceOpacity.x, bladeType.minMaxSubsurfaceOpacity.y, appearanceT), lerp(rungRoughness.x, rungRoughness.y, rungBlend));

	uint clumpSeed = input.MaterialData & 0xFFu;
	float3 stableClumpColor = GetStableClumpColor(bladeType, clumpSeed);
	float3 rungColor0, rungColor1;
	GetStabilizedBladeColor(bladeType, stableClumpColor, rungT, rungColor0, rungColor1);
	float3 baseToTipColor = lerp(rungColor0, rungColor1, rungBlend);

	// Blend complete material samples so both halves agree at the middle rung throughout the morph.
	[branch] if (lowMaterialBlend > 0.0f && !doubleBlade)
	{
		aoThicknessRoughness = lerp(aoThicknessRoughness, GetDistantAOThicknessRoughness(bladeType, appearanceT, clumpDensity, along), lowMaterialBlend);
		baseToTipColor = lerp(baseToTipColor, GetDistantBladeColor(bladeType, stableClumpColor, along), lowMaterialBlend);
	}

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
	float detailedSpecularWeight = 1.0f - lowMaterialBlend;

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
	float bladeAO = aoThicknessRoughness.x;

	// Reconstruct the blade basis and orient both sides of its rolled surface consistently.
	float3 bitangent = normalize(float3(-facing.y, facing.x, 0.0f));
	float3 tangent = float3(facing * derivative.x, derivative.y);
#if defined(HIGH_LOD)
	// Position uses windOffset * t^2, so its tangent gains the derivative 2t * windOffset.
	tangent.xy += input.WindLodDensity.xy * (2.0f * along);
#elif defined(MID_LOD)
	tangent.xy += input.WindRootPosition.xy * (2.0f * along);
#endif
	// Far only needs the sheet normal, which is normalized after the cross product.
#if !defined(FAR_LOD)
	tangent = normalize(tangent);
#endif
	float3 normal = cross(-bitangent, tangent);
	float normalLengthSquared = dot(normal, normal);

	// Wind can align the tangent with the blade width; reflection still needs a unit sheet normal.
	normal = normalLengthSquared > 1e-8f ? normal * rsqrt(normalLengthSquared) : cross(-bitangent, float3(0.0f, 0.0f, 1.0f));
	float side = across * 2.0f - 1.0f;
	float3 bladePlaneNormal = normal;

	// Triangle winding can disagree with this reconstructed basis after bending; use the view-facing sheet side.
	float faceSign = dot(normal, worldSpaceViewDirection) >= 0.0f ? 1.0f : -1.0f;
	float3 transmissionNormal = normal * faceSign;

	// Roll the normal across the blade width; folded halves reverse winding.
	float curveSign = frontFace ? 1.0f : -1.0f;
	float curveAngle = side * bladeType.grassVeinParams2.w * Math::HALF_PI * curveSign;

	// Use a stronger roll for indirect light and the GI normal; direct light retains the authored curve.
	static const float IndirectCurvatureScale = 3.0f;
	float indirectCurveAngle = clamp(curveAngle * IndirectCurvatureScale, -Math::HALF_PI, Math::HALF_PI);
	float2 curveSines, curveCosines;
	sincos(float2(curveAngle, indirectCurveAngle), curveSines, curveCosines);
	float3 worldSpaceNormal = transmissionNormal * curveCosines.x + bitangent * curveSines.x;
#if defined(FAR_LOD)
	// Far has no vein or ground-normal detail; construct the stronger roll directly from its sheet basis.
	float3 bladeIndirectNormal = normalize(transmissionNormal * curveCosines.y + bitangent * curveSines.y);
#else
	float3 indirectCurveOffset = bitangent * (curveSines.y - curveSines.x) + transmissionNormal * (curveCosines.y - curveCosines.x);
#endif

#if defined(HIGH_LOD)
	float groundProximity = 1.0 - smoothstep(0.0, max(grassTerrainBlend.y, 0.01), sideAndBladeT.z);
	float groundBlend = groundProximity * grassTerrainBlend.x;
#else
	const float groundBlend = 0.0;
#endif
	float grassOpacity = 1.0f - groundBlend;
#if defined(HIGH_LOD)
	grassOpacity *= 1.0f - sceneHidden;
#endif

	float3 veinTint = bladeType.grassVeinParams.rgb;
	float veinAlbedoStrength = saturate(bladeType.grassVeinParams.w * 1.20);
	float mottleStrength = bladeType.grassColorVar.w;

#if defined(LOW_LOD)
	float vein = 0.0;
#elif defined(HIGH_LOD)
#	if defined(HIGH_INNER)
	static const float MaterialDetailNormalRange = 1.25f;
	float vein = materialDetail.z * detailFade;
	float veinNormalOffset = (materialDetail.w * 2.0f - 1.0f) * MaterialDetailNormalRange * detailFade;
	worldSpaceNormal = normalize(worldSpaceNormal + bitangent * (veinNormalOffset * curveSign));
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

		worldSpaceNormal = normalize(worldSpaceNormal + veinOffset * curveSign);
	}
#endif

#if !defined(FAR_LOD)
	// Turn the base normal toward the ground plane so it shades like terrain, not an edge-on blade.
	worldSpaceNormal = normalize(lerp(worldSpaceNormal, float3(0.0, 0.0, 1.0), groundBlend * grassTerrainBlend.z));
	float3 bladeIndirectNormal = normalize(worldSpaceNormal + indirectCurveOffset * (1.0f - groundBlend * grassTerrainBlend.z));
#endif

	// Use the same continuous distance ramp on every tier so unresolved blades share a canopy response.
	// Stop at the blend Low reaches where Far starts: seen at a grazing angle a distant canopy still shows blade sides, and
	// turning Far's blades further toward the sky lit them like bare ground, so distant grass read as sparse.
	float canopyNormalBlend = smoothstep(2048.0f, 24576.0f, min(length(cameraRelativePosition.xy), farParams.x));
#if defined(FAR_LOD)
	// Far's canopy lies on the terrain: on a slope it faces down the slope, not the open sky.
	float3 canopySurfaceNormal = farTerrainNormal;
#else
	static const float3 canopySurfaceNormal = float3(0.0f, 0.0f, 1.0f);
#endif
	float3 indirectNormal = BlendGrassNormalToCanopy(bladeIndirectNormal, canopyNormalBlend, canopySurfaceNormal);

	// The deferred composite's screen-space GI reads this normal.
	float3 screenSpaceNormal = normalize(FrameBuffer::WorldToView(indirectNormal, false));

#if !defined(LOW_LOD)
	// Mottle is low-frequency and fixed to the blade, so distant blades keep it; Mid fades it out as it takes Low's material.
#	if defined(MID_LOD)
	float bladeColorDetail = 1.0f - lowMaterialBlend;
#	else
	static const float bladeColorDetail = 1.0f;
#	endif
	float mottle = sin(along * 5.0 + bladeRand * Math::TAU) * 0.5 + 0.5;
	baseColor.rgb *= 1.0 + (mottle - 0.5) * 2.0 * mottleStrength * bladeColorDetail;
#endif

#if defined(HIGH_LOD)
	float speckle = 0.5;
	float speckleAmount = 0.0;
#	if defined(HIGH_INNER)
	baseColor.rgb = ApplyGrassBlotch(bladeType, baseColor.rgb, smoothstep(0.28, 0.72, materialDetail.x), bladeType.grassTextureParams.x * detailFade);

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
	// As the texture detail fades, each blade keeps one stable blotch so distant blades do not turn a solid colour.
	baseColor.rgb = ApplyGrassBlotch(bladeType, baseColor.rgb, smoothstep(0.28, 0.72, bladeRand), bladeType.grassTextureParams.x * (1.0f - detailFade));

#elif defined(LOW_LOD)
	float speckle = 0.5;
	float speckleAmount = 0.0;
#else
	float speckle = 0.5;
	float speckleAmount = 0.0;
	// Match High's blotch with one stable value per blade.
	baseColor.rgb = ApplyGrassBlotch(bladeType, baseColor.rgb, smoothstep(0.28, 0.72, bladeRand), bladeType.grassTextureParams.x * bladeColorDetail);
	[branch] if (detailFade > 0.0)
	{
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
	// The sky the canopy hides depends on which way the surface faces, so GetCanopySkyVisibility applies it below.
	float canopyAO = 1.0f;

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

	[branch] if (miscParams.y > 0.0f) {
#	if defined(MID_LOD)
		float3 objectRoot = float3(input.WindRootPosition.zw, input.BladeTDepth.z) + FrameBuffer::CameraPosAdjust.xyz;
#	else
		float3 objectRoot = float3(input.RootPosition.xy, cameraRelativePosition.z - along * lowTip.y) + FrameBuffer::CameraPosAdjust.xyz;
#	endif
		float objectHeight, objectDensity;
		float2 objectSlope;
		uint objectType;
		if (LoadGrassObjectSurface(objectRoot.xy, objectHeight, objectSlope, objectType, objectDensity) && abs(objectRoot.z - objectHeight) < 4.0f) {
			canopyDensity = objectDensity;
			canopyAODensity = objectDensity;
		}
	}

	canopyAO *= 1.0 - grassLightParams.x * canopyAODensity * (1.0 - canopyHeight01);
#else
	static const float canopyDensity = 1.0f;
#endif

	float canopyOverhead = (1.0 - canopyHeight01) * lerp(0.4, 1.0, canopyDensity);

	float4 shadowColor = 1.0;

#if defined(FAR_LOD)
	// Remove both the straight tip offset and the base-side offset to recover the actual root for shadows.
	float baseHalfWidth = f16tof32(input.PackedBladeParams.w >> 16);
	float2 baseSideOffset = GetFarSideAxis(cameraRelativePosition.xy) * (baseHalfWidth * (1.0f - along) * (across * 2.0f - 1.0f));
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
	float groundSunFacing = smoothstep(-0.1f, 0.1f, dot(farTerrainNormal, SharedData::DirLightDirection.xyz));
	shadowColor.x *= lerp(1.0f, groundSunFacing, groundSunFade);
#endif

#if defined(MID_LOD) || defined(LOW_LOD)
	// Match the darker lower blades on Mid while preserving the lit tips.
	static const float LowContactOcclusionBase = 0.62f;

	// Mid's tips sit in each other's screen-space shadows, so Low's tips stay short of fully open.
	static const float LowContactOcclusionTip = 0.9f;
#	if defined(FAR_LOD)
	// Keep Low's occlusion at every distance so Far does not brighten as it recedes.
	float lowContactOcclusion = lerp(LowContactOcclusionBase, LowContactOcclusionTip, smoothstep(0.0f, 0.9f, farShadeAlong));
#	else
	float lowContactOcclusion = lerp(LowContactOcclusionBase, LowContactOcclusionTip, smoothstep(0.0f, 0.9f, along));
#	endif
#	if defined(MID_LOD)
	lowContactOcclusion = lerp(1.0f, lowContactOcclusion, lowMaterialBlend);
#	endif
	bladeAO *= lowContactOcclusion;
#endif

	MaterialProperties material = (MaterialProperties)0;
	material.Noise = screenNoise;
	material.Roughness = saturate(aoThicknessRoughness.z);
	material.Roughness = saturate(lerp(material.Roughness, 1.0, groundBlend * grassTerrainBlend.w));

	// Thin surfaces should not receive the same deep crevice occlusion as solid geometry.
	material.AO = sqrt(saturate(bladeAO));
	material.F0 = saturate(bladeType.specular);
	material.F0 = lerp(material.F0, material.F0 * 1.12, vein * 0.25);
	material.Thickness = saturate(aoThicknessRoughness.y);

	float3 specularColorPBR = 0;
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
	// Rain wets a thin blade on every side, so blades take the rain wetness directly. Terrain scales it by how far a
	// surface faces up and breaks it into puddles, which would leave near-vertical blades almost dry. The water film
	// follows the blade's own normal, and fades out before Low, which has no wet layer.
	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewDepth);
	float waterHeight = SharedData::GetWaterData(cameraRelativePosition).w;
	float shoreFactor = saturate(1.0 - (abs(cameraRelativePosition.z - waterHeight) / (float)SharedData::wetnessEffectsSettings.ShoreRange));
	bool submerged = cameraRelativePosition.z < waterHeight;
	float shoreAlbedoWetness = (submerged ? 1.0 : shoreFactor) * SharedData::wetnessEffectsSettings.MaxShoreWetness;
	float wetness = shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness;
#	if !defined(PGRASS_DRY_WETNESS)
#		if defined(SKYLIGHTING) && !defined(FAR_LOD)
	// Grass under cover stays drier.
	float wetnessOcclusion = saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1)));
	wetnessOcclusion *= wetnessOcclusion;
#		else
	float wetnessOcclusion = 1.0;
#		endif
	wetness = max(wetness, SharedData::wetnessEffectsSettings.Wetness * SharedData::wetnessEffectsSettings.MaxRainWetness * wetnessOcclusion);
#	endif

	float waterRoughnessSpecular = 1.0;
	float3 wetnessNormal = worldSpaceNormal;
	float filmWetness = saturate(wetness) * nearFactor;
	// Water darkens porous ground by filling it; a waxy blade mostly turns glossy, so its film darkens it less.
	static const float BladeWetDarkening = 0.6;
	float wetnessGlossinessAlbedo = max(filmWetness * BladeWetDarkening, shoreAlbedoWetness);
	wetnessGlossinessAlbedo *= wetnessGlossinessAlbedo;

	// Without rain or shore water the wet layer is absent: its roughness stays 1, which gates every later use.
	[branch] if (filmWetness > 0.0)
	{
		float wetnessGlossinessSpecular = submerged ? filmWetness * shoreFactor : filmWetness;
		wetnessNormal = BlendGrassNormalToCanopy(wetnessNormal, canopyNormalBlend, canopySurfaceNormal);
		waterRoughnessSpecular = 1.0 - wetnessGlossinessSpecular * 0.9;
	}
#endif

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	[branch] if (wetnessGlossinessAlbedo > 0.0)
	{
		float wetnessDarkeningAmount = wetnessGlossinessAlbedo;
		float3 wetBaseColor = Color::LLLinearToGamma(baseColor.xyz);
		wetBaseColor = lerp(wetBaseColor, pow(abs(wetBaseColor), 1.0 + wetnessDarkeningAmount), 0.8);
		baseColor.xyz = Color::LLGammaToLinear(wetBaseColor);
	}
#endif

	material.BaseColor = saturate(baseColor.xyz);
	// The independent scattering tint retains the same base-to-tip variation, veins and wetness as the diffuse base.
	float3 scatteringColor = saturate(material.BaseColor * bladeType.grassSubsurfaceColor.rgb);
	float3 reflectionAlbedo, transmissionAlbedo;
	GetGrassScatteringAlbedos(material.BaseColor, scatteringColor, material.Thickness, bladeType.grassSurfParams.y,
		reflectionAlbedo, transmissionAlbedo);

	float3 dirLightColor = grassFrameLight.xyz;
	float3 dirLightDirection = SharedData::DirLightDirection.xyz;

	// Preserve the signed curve and vein tilt of the visible face.
	float shadingNormalSheet = dot(worldSpaceNormal, bladePlaneNormal);
	float3 shadingNormalTilt = worldSpaceNormal - bladePlaneNormal * shadingNormalSheet;

	// Halve direct-light curvature while preserving the signed tilt and a nonzero sheet component.
	static const float DirectCurvature = 0.5f;
	float3 visibleFaceNormal = normalize(transmissionNormal * max(abs(shadingNormalSheet), 1e-4f) + shadingNormalTilt * DirectCurvature);
	visibleFaceNormal = BlendGrassNormalToCanopy(visibleFaceNormal, canopyNormalBlend, canopySurfaceNormal);

	// Use signed hemispheres for diffuse reflection and transmission.
	float dirDiffuseNdotL = dot(visibleFaceNormal, dirLightDirection);

	float dirDetailShadow = 1.0;
#if defined(SCREEN_SPACE_SHADOWS) && !defined(LOW_LOD)
	dirDetailShadow = ScreenSpaceShadows::GetScreenSpaceShadow(float3(shadowPixel, input.Position.z), screenUV, screenNoise);

#	if defined(MID_LOD) || (defined(HIGH_LOD) && !defined(HIGH_INNER))
	// Outer High and Mid share the handoff to statistical shadows as blades become unresolved.
	static const float MidPatternStart = 2048.0f;
	static const float MidPatternEnd = 4096.0f;

#		if defined(HIGH_LOD)
	float rootDistance = ApproximateGrassDistance(cameraRelativePosition.xy + FrameBuffer::CameraPosAdjust.xy - grassLodOrigin);
#		endif

	float midPatternBlend = smoothstep(MidPatternStart, MidPatternEnd, rootDistance);

	// Keep traced shadows on resolved blades. The taper correction lets thin tips reach the statistical pattern first.
	float widthFootprint = length(float2(ddx(across), ddy(across))) / max(1.0f - along, 1e-3f);
	midPatternBlend *= smoothstep(0.5f, 1.0f, widthFootprint);

	// Finish the shadow handoff with the geometry and material morph to Low.
	midPatternBlend = lerp(midPatternBlend, 1.0f, GetMidLowBlend(rootDistance));

	[branch] if (midPatternBlend > 0.0f)
	{
#		if defined(HIGH_LOD)
		// High blades carry no clump seed; the low bits of their random value are as stable.
		uint midShadowSeed = bladeRandBits & 0xFFFu;
#		else
		uint midShadowSeed = (input.MaterialData & 0xFFu) | ((bladeRandBits & 0xFu) << 8);
#		endif

		float midBladeShadow = GetBladeShadowPattern(worldSpaceViewDirection, dirLightDirection, midShadowSeed, along, 0.0f);
		dirDetailShadow = lerp(dirDetailShadow, midBladeShadow, midPatternBlend);
	}
#	endif

#elif defined(SCREEN_SPACE_SHADOWS)
	// Low and Far sample object and terrain shadows at the root, and approximate blade shadows statistically.
	// Covered roots skip the screen-space trace so overlapping Mid blades do not shadow them twice.
#	if defined(FAR_LOD)
	float detailShadowFade = GetFarGrassRootShadowFade(rootPosition);
	[branch] if (detailShadowFade > 0.0f && rootCoverWeight < 1.0f && rootDistantForegroundWeight < 1.0f) {
		float rootContactShadow = SampleGrassRootContactShadow(
			ScreenSpaceShadows::ScreenSpaceShadowsTexture, SampColorSampler, shadowPixel);
		float rootDetailShadow = lerp(rootContactShadow, 1.0f, rootCoverWeight);
		dirDetailShadow = lerp(1.0f, rootDetailShadow, detailShadowFade * (1.0f - rootDistantForegroundWeight));
	}
#	else
	float rootContactShadow = SampleGrassRootContactShadow(
		ScreenSpaceShadows::ScreenSpaceShadowsTexture, SampColorSampler, shadowPixel);
	float rootDetailShadow = lerp(rootContactShadow, 1.0f, rootCoverWeight);
	float detailShadowFade = GetGrassRootShadowFade(rootPosition);

	dirDetailShadow = lerp(1.0f, rootDetailShadow, detailShadowFade * (1.0f - rootDistantForegroundWeight));
#	endif

	// The clump seed and per-blade bend are stable across frames, unlike the f16 camera-relative root.
#	if defined(FAR_LOD)
	uint lowShadowSeed = (packedSeedAndType >> 8) & 0xFFu | ((packedSeedAndType >> 20) & 0xFu) << 8;

	// Widened Far triangles stand in for several blades, so ease toward the pattern's expected visibility.
	float lowShadowExpectedBlend = smoothstep(0.0f, 1.0f, farWidthT);
#	else
	uint lowShadowSeed = (lowBladeData >> 8) & 0xFFFu;
	static const float lowShadowExpectedBlend = 0.0f;
#	endif

	dirDetailShadow *= GetBladeShadowPattern(worldSpaceViewDirection, dirLightDirection, lowShadowSeed, along, lowShadowExpectedBlend);
#endif

#if defined(MID_LOD) || defined(LOW_LOD)
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

	// World shadows attenuate all direct light; canopy visibility applies to the blade lobes.
	float dirSurfaceShadow = shadowColor.x;
	float dirDetailedVisibility = dirSurfaceShadow * dirDetailShadow;

#if defined(FAR_LOD)
	FarGrassSurface farSurface;
	farSurface.reflectionAlbedo = reflectionAlbedo;
	farSurface.transmissionAlbedo = transmissionAlbedo;
	farSurface.bounceColor = bladeType.grassBounceColor.rgb * bladeType.grassTypeLightParams.x;
	farSurface.ambientDesaturation = bladeType.grassTypeLightParams.w;
	float fuzzNdotV = clamp(dot(indirectNormal, worldSpaceViewDirection), EPSILON_DOT_CLAMP, 1.0f);
	farSurface.fuzzAlbedo = saturate(bladeType.grassSurfParams.x) *
	                      PBR::FuzzDirectionalAlbedoWithParameters(fuzzNdotV, bladeType.fuzzDirectionalAlbedoParams);
	farSurface.fuzzColor = GetGrassFuzzColor(material.BaseColor, bladeType.grassSubsurfaceColor.w);
	FarGrassLighting farLighting = GetFarGrassLighting(farSurface, material,
		visibleFaceNormal, indirectNormal, worldSpaceViewDirection, dirLightDirection, dirLightColor * dirShadow,
		dirSurfaceShadow, dirDetailShadow, canopyAO, canopyOverhead, canopyHeight01);

	float screenAO = 1.0f - saturate(GrassScreenAO[uint2(shadowPixel)]);

	psout.Diffuse = float4(Color::IrradianceToGamma(ResolveFarGrassLighting(farLighting, screenAO)), grassOpacity);
	// Far draws after the deferred pass, so its pixels still hold the motion of the surface behind them. Against distant
	// terrain or sky that parallax differs from the blade's, and TAA would reproject the blade from the wrong place.
	psout.MotionVectors = float4(MotionBlur::GetSSMotionVector(float4(cameraRelativePosition, 1.0f), float4(previousCameraRelativePosition, 1.0f)), 0.0f, 1.0f);
	[branch] if (debugFlags.y > 0.5f)
		psout.Diffuse.xyz = GetTierDebugColor();
	return psout;
#else
	float3 diffuseColor = 0;

	DirectContext directContext = CreateDirectLightingContext(worldSpaceNormal, worldSpaceNormal, worldSpaceNormal, worldSpaceViewDirection, worldSpaceViewDirection, dirLightDirection, dirLightDirection, dirLightColor * dirShadow, dirDetailedVisibility, dirSurfaceShadow);
	DirectLightingOutput dirLighting;

	// Broaden unresolved highlights before the slower canopy normal transition completes.
	static const float CanopyRoughness = 0.8f;
	float canopyRoughnessBlend = smoothstep(256.0f, 4096.0f, length(cameraRelativePosition));

	// Halve specular curvature and retain the sheet component to avoid cancellation at grazing views.
	static const float SpecularCurvature = 0.5f;
	float3 specularNormal = normalize(transmissionNormal * max(abs(shadingNormalSheet), 1e-4f) + shadingNormalTilt * SpecularCurvature);
	specularNormal = BlendGrassNormalToCanopy(specularNormal, canopyNormalBlend, canopySurfaceNormal);
	float3 canopyNormal = specularNormal;
	canopyNormal = dot(canopyNormal, worldSpaceViewDirection) < 0.0f ? -canopyNormal : canopyNormal;
	float canopyNdotV = clamp(dot(canopyNormal, worldSpaceViewDirection), EPSILON_DOT_CLAMP, 1.0f);
	float canopySlopeVariance = canopyRoughnessBlend * CanopyRoughness * CanopyRoughness * CanopyRoughness * CanopyRoughness;

	// Filter unresolved specular normal variation to prevent single-pixel highlights.
	static const float SpecularAntiAliasingPixelVariance = 0.5f;
	static const float SpecularAntiAliasingMaxVariance = 0.3f;
	float specularAntiAliasingVariance = PBR::SpecularAntiAliasingVariance(ddx(specularNormal), ddy(specularNormal),
		SpecularAntiAliasingPixelVariance, SpecularAntiAliasingMaxVariance);
	float specularSlopeVariance = canopySlopeVariance + specularAntiAliasingVariance;
	float canopyRoughness = PBR::AddRoughnessVariance(material.Roughness, specularSlopeVariance);
	float3 specularAlbedo = PBR::SpecularDirectionalAlbedo(material.F0, canopyRoughness, canopyNdotV);

#if defined(PGRASS_DISTANT_LIGHTING)
	float3 fuzzNormal = canopyNormal;
#else
	// The OpenPBR fuzz layer follows the indirect normal; its curvature fades toward the canopy.
	float indirectNormalSheet = dot(bladeIndirectNormal, bladePlaneNormal);
	float3 indirectNormalTilt = bladeIndirectNormal - bladePlaneNormal * indirectNormalSheet;
	float3 fuzzNormal = normalize(transmissionNormal * max(abs(indirectNormalSheet), 1e-4f) + indirectNormalTilt);
	fuzzNormal = BlendGrassNormalToCanopy(fuzzNormal, canopyNormalBlend, canopySurfaceNormal);
#endif
	float fuzzNdotV = clamp(dot(fuzzNormal, worldSpaceViewDirection), EPSILON_DOT_CLAMP, 1.0f);
	float fuzzRoughness = clamp(bladeType.grassSurfParams.w, 0.01f, 1.0f);
	float fuzzAlbedo = saturate(bladeType.grassSurfParams.x) * PBR::FuzzDirectionalAlbedoWithParameters(fuzzNdotV, bladeType.fuzzDirectionalAlbedoParams);
	float3 fuzzColor = GetGrassFuzzColor(material.BaseColor, bladeType.grassSubsurfaceColor.w);

	// The canopy roughening is isotropic, so the veins' anisotropy fades out where it takes over.
	float specularAnisotropy = saturate(bladeType.specularAnisotropy) * (1.0f - canopyRoughnessBlend);

	float fuzzThroughput = 1.0f - fuzzAlbedo;
	// Keep the anisotropic tangent perpendicular to the specular normal, with a grazing-view fallback.
	float3 acrossTangent = bitangent - specularNormal * dot(specularNormal, bitangent);
	float3 tangentAxis = abs(specularNormal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
	float3 specularTangent = normalize(dot(acrossTangent, acrossTangent) > 0.01f ? acrossTangent : cross(tangentAxis, specularNormal));
	GetDirectLightInputProcGrass(dirLighting, directContext, material, dirDiffuseNdotL,
		reflectionAlbedo, transmissionAlbedo,
		specularNormal, specularTangent, material.Roughness, specularAnisotropy, specularSlopeVariance, specularAlbedo, fuzzNormal, fuzzNdotV, fuzzAlbedo, fuzzColor, fuzzRoughness);

	// Retain canopy extinction along the sun path as blade normals converge.
	float canopyLightPath = canopyOverhead / max(dirLightDirection.z, CanopyReflectionMinElevation);
	dirLighting.specular *= exp2(-canopyLightPath * CanopyReflectionExtinction);

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
#	if defined(MID_LOD)
	[branch] if (detailedSpecularWeight > 0.0 && waterRoughnessSpecular < 1.0)
#	else
	[branch] if (waterRoughnessSpecular < 1.0)
#	endif
		EvaluateWetnessLighting(wetnessNormal, directContext, waterRoughnessSpecular, dirLighting);
#endif

	diffuseColor += dirLighting.diffuse;
	specularColorPBR += dirLighting.specular;

	// Sum escaped canopy scatter as e*a / (1 - (1-e)*a), bounded by the intercepted energy.
	float3 canopySingleScatterAlbedo = saturate(reflectionAlbedo + transmissionAlbedo);
	float3 canopyScatterAlbedo = CanopyScatterEscape * canopySingleScatterAlbedo /
	                            (1.0f - (1.0f - CanopyScatterEscape) * canopySingleScatterAlbedo);
	float canopyBlockedLight = saturate(1.0f - dirDetailShadow) * shadowColor.x * saturate(dirLightDirection.z);

	// Surrounding grass scatters light toward both sides of the blade.
	diffuseColor += dirLightColor * dirShadow * BRDF::Diffuse_Lambert() * canopyBlockedLight * canopyScatterAlbedo *
	                (reflectionAlbedo + transmissionAlbedo) * (1.0f - specularAlbedo) * fuzzThroughput;

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
			float pointDiffuseNdotL = dot(visibleFaceNormal, normalizedLightDirection);
			GetDiffuseLightInputProcGrass(pointLighting, pointContext, pointDiffuseNdotL,
				reflectionAlbedo, transmissionAlbedo, (1.0f - specularAlbedo) * fuzzThroughput);
#		if defined(WETNESS_EFFECTS)
			[branch] if (waterRoughnessSpecular < 1.0)
				EvaluateWetnessLighting(wetnessNormal, pointContext, waterRoughnessSpecular, pointLighting);
#		endif
			diffuseColor += pointLighting.diffuse * detailedSpecularWeight;
			specularColorPBR += pointLighting.specular * detailedSpecularWeight;
		}
	}
#	endif
#endif

	float3 frontAmbientIrradiance = DistantAmbientLUT.SampleLevel(SampColorSampler, GBuffer::EncodeNormal(indirectNormal), 0).rgb;
	float3 directionalAmbientColor = frontAmbientIrradiance;
	float ambientLuma = dot(directionalAmbientColor, float3(0.2126, 0.7152, 0.0722));
	directionalAmbientColor = lerp(directionalAmbientColor, ambientLuma, bladeType.grassTypeLightParams.w);

#if defined(SKYLIGHTING) && !defined(FAR_LOD)
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, positionMSSkylight, indirectNormal);
#endif

	// Ambient light the canopy hides is scattered on like sunlight, so hidden sky still arrives as grass-coloured light.
	float3 canopyAmbientFill = canopyScatterAlbedo;
	float3 frontCanopySkyLight = canopyAO * GetCanopySkyLight(indirectNormal, canopyOverhead, canopyAmbientFill);

	// A curved blade's concave face hides part of its sky behind its own halves, which matters most where ambient light
	// dominates. The hidden sky arrives as grass-scattered light, as under the canopy, and the view factor fades out with
	// the curve as the normals blend to the canopy.
	float curveSkyView = 1.0f;
	if (curveSign < 0.0f)
		curveSkyView = lerp(GetCurvedBladeSkyView(saturate(bladeType.grassVeinParams2.w) * Math::HALF_PI, side), 1.0f, canopyNormalBlend);
	float3 curveSkyLight = lerp(canopyAmbientFill, 1.0f, curveSkyView);
	frontCanopySkyLight *= curveSkyLight;
	directionalAmbientColor *= frontCanopySkyLight;

	IndirectLobeWeights indirectLobeWeights = (IndirectLobeWeights)0;
	// Both ambient hemispheres use the same scattering budget and surface-layer attenuation as direct light.
	float3 ambientScatteringThroughput = MultiBounceAO(material.BaseColor, material.AO) * (1.0f - specularAlbedo) * fuzzThroughput;
	indirectLobeWeights.diffuse = reflectionAlbedo * ambientScatteringThroughput;

#if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	float3 wetnessReflectance = 0.0;
	[branch] if (waterRoughnessSpecular < 1.0)
	{
		IndirectContext indirectContext = CreateIndirectLightingContext(worldSpaceNormal, worldSpaceNormal, worldSpaceViewDirection);
		wetnessReflectance = GetWetnessIndirectLobeWeights(indirectLobeWeights, wetnessNormal, waterRoughnessSpecular, indirectContext);
		ambientScatteringThroughput *= 1.0f - wetnessReflectance;
	}
#endif

	// Direct irradiance uses albedo; ambient already contains the full indirect lobe.
	float3 ambientDiffuseColor = directionalAmbientColor * indirectLobeWeights.diffuse;
	// The fuzz reflects the ambient light over the hemisphere around its normal, occluded like the blade beneath.
#if defined(PGRASS_DISTANT_LIGHTING)
	float3 fuzzAmbient = frontAmbientIrradiance * frontCanopySkyLight * material.AO;
#else
	float3 fuzzAmbient = DistantAmbientLUT.SampleLevel(SampColorSampler, GBuffer::EncodeNormal(fuzzNormal), 0).rgb * canopyAO *
	                     GetCanopySkyLight(fuzzNormal, canopyOverhead, canopyAmbientFill) * curveSkyLight * material.AO;
#endif
	ambientDiffuseColor += fuzzAmbient * fuzzAlbedo * fuzzColor;

	// The opposite hemisphere supplies diffuse transmission at every tier.
	float3 backNormal = -indirectNormal;
	float3 backAmbientColor = DistantAmbientLUT.SampleLevel(SampColorSampler, GBuffer::EncodeNormal(backNormal), 0).rgb;
	backAmbientColor = lerp(backAmbientColor, dot(backAmbientColor, float3(0.2126, 0.7152, 0.0722)), bladeType.grassTypeLightParams.w);
	ambientDiffuseColor += backAmbientColor * canopyAO * GetCanopySkyLight(backNormal, canopyOverhead, canopyAmbientFill) *
	                       transmissionAlbedo * ambientScatteringThroughput;
	float3 shadedDiffuseColor = diffuseColor.xyz + ambientDiffuseColor;
	float3 bounceColor = directionalAmbientColor * bladeType.grassBounceColor.rgb * bladeType.grassTypeLightParams.x * (1.0 - canopyHeight01) * reflectionAlbedo;
	float3 ambientBounceColor = bounceColor * (1.0f - specularAlbedo) * fuzzThroughput;
	shadedDiffuseColor += ambientBounceColor;

#if defined(SKYLIGHTING) && !defined(FAR_LOD)
	Skylighting::ApplySkylighting(shadedDiffuseColor, ambientDiffuseColor, indirectLobeWeights.diffuse, skylightingDiffuse);
#endif

	float grassLightingScale = grassFrameLight.w;
	shadedDiffuseColor *= grassLightingScale;
	// Report ambient luminance in Masks.z so the composite applies full AO to ambient light.
	float3 ambientShadedColor = (ambientDiffuseColor + ambientBounceColor) * grassLightingScale;

	// The tighter specular lobe benefits from the authored AO that diffuse intentionally softens above.
	float specOcclusion = saturate(bladeAO);
	// Apply sky-reflection AO separately from the direct-light shadow.
	float3 specularReflectDirection = reflect(-worldSpaceViewDirection, canopyNormal);
	float3 specularSky = DistantAmbientLUT.SampleLevel(SampColorSampler, GBuffer::EncodeNormal(specularReflectDirection), 0).rgb;
	float specularSkyOcclusion = SpecularOcclusion(canopyNdotV, canopyRoughness * canopyRoughness, material.AO);
#if defined(SKYLIGHTING) && !defined(FAR_LOD)
#	if defined(PGRASS_DISTANT_LIGHTING)
	specularSkyOcclusion *= skylightingDiffuse;
#	else
	specularSkyOcclusion *= Skylighting::GetSkylightingDiffuse(skylightingSH, positionMSSkylight, specularReflectDirection);
#	endif
#endif

	// Blend occluded sky reflections toward surrounding grass; downward paths see only canopy and ground.
	float canopyReflectionPath = canopyOverhead / max(specularReflectDirection.z, CanopyReflectionMinElevation);
	float canopyReflectionVisibility = exp2(-canopyReflectionPath * CanopyReflectionExtinction) *
	                                   smoothstep(-CanopyReflectionMinElevation, CanopyReflectionMinElevation, specularReflectDirection.z) * curveSkyView;
	float3 canopyReflection = lerp(Color::IrradianceToLinear(ambientDiffuseColor),
		Color::IrradianceToLinear(specularSky * Color::ReflectionNormalisationScale), canopyReflectionVisibility);
	specularColorPBR += canopyReflection * specularAlbedo * fuzzThroughput * specularSkyOcclusion;
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
#endif

	psout.Specular = float4(specularColor, psout.Diffuse.w);

	float3 outputAlbedo = indirectLobeWeights.diffuse;

	psout.Albedo = float4(outputAlbedo, psout.Diffuse.w);

#	if defined(WETNESS_EFFECTS) && !defined(LOW_LOD)
	indirectLobeWeights.specular += wetnessReflectance;
	[branch] if (waterRoughnessSpecular < 1.0)
	{
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(wetnessNormal, false));
		pbrGlossiness = saturate(1.0 - waterRoughnessSpecular);
	}
#	endif

	// Only wetness reaches the composite's reflections; the blade's own specular reflection is already in Specular.
	psout.Reflectance = float4(indirectLobeWeights.specular * specOcclusion, psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), pbrGlossiness, psout.Diffuse.w);
	psout.Masks = float4(0, 0, Color::RGBToYCoCg(ambientShadedColor).x, psout.Diffuse.w);
	psout.Masks2 = float4(0, 0, 0, psout.Diffuse.w);
	float2 screenMotionVector = MotionBlur::GetSSMotionVector(float4(cameraRelativePosition, 1), float4(previousCameraRelativePosition, 1));
	psout.MotionVectors.xy = screenMotionVector.xy;
	psout.MotionVectors.zw = float2(0, psout.Diffuse.w);

	[branch] if (debugFlags.y > 0.5f)
	{
		// Leave only the flat tier colour: no ambient, specular or reflections from the composite.
		psout.Diffuse.xyz = GetTierDebugColor();
		psout.Specular.xyz = 0.0f;
		psout.Albedo.xyz = 0.0f;
		psout.Reflectance.xyz = 0.0f;
	}

	return psout;
#endif
}
