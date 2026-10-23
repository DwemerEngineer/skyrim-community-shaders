#ifndef __PBR_MATH_HLSL__
#define __PBR_MATH_HLSL__

#include "Common/BRDF.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/Math.hlsli"

namespace PBR
{
	namespace Constants
	{
		static const float MinRoughness = 0.04f;
		static const float MaxRoughness = 1.0f;
		static const float MinGlintDensity = 1.0f;
		static const float MaxGlintDensity = 40.0f;
		static const float MinGlintRoughness = 0.005f;
		static const float MaxGlintRoughness = 0.3f;
		static const float MinGlintDensityRandomization = 0.0f;
		static const float MaxGlintDensityRandomization = 5.0f;
		static const float IceIOR = 1.31f;
		static const float IceF0 = 0.018f;               // IORToF0(IceIOR)
		static const float IceDepthBlur = 4.0f / 70.0f;  // Inner layer mip bias growth per game unit of depth at roughness 1, about 4 per meter
	}

	namespace Flags
	{
		static const uint HasEmissive = (1 << 0);
		static const uint HasDisplacement = (1 << 1);
		static const uint HasFeatureTexture0 = (1 << 2);
		static const uint HasFeatureTexture1 = (1 << 3);
		static const uint Subsurface = (1 << 4);
		static const uint TwoLayer = (1 << 5);
		static const uint ColoredCoat = (1 << 6);
		static const uint InterlayerParallax = (1 << 7);
		static const uint CoatNormal = (1 << 8);
		static const uint Fuzz = (1 << 9);
		static const uint HairMarschner = (1 << 10);
		static const uint Glint = (1 << 11);
		static const uint ProjectedGlint = (1 << 12);
		static const uint Foliage = (1 << 13);
		static const uint ThinSurface = (1 << 14);
		static const uint Ice = (1 << 15);
	}

	bool IsFoliageMaterial(uint flags)
	{
#if defined(LANDSCAPE) || defined(LODLANDSCAPE)
		return false;
#elif defined(TREE_ANIM)
		return (flags & (Flags::Foliage | Flags::Subsurface)) != 0;
#else
		return (flags & Flags::Foliage) != 0;
#endif
	}

	// Foliage is a thin surface with canopy-specific approximations on top.
	bool IsThinSurfaceMaterial(uint flags)
	{
#if defined(LANDSCAPE) || defined(LODLANDSCAPE)
		return false;
#else
		return IsFoliageMaterial(flags) || (flags & Flags::ThinSurface) != 0;
#endif
	}

	namespace TerrainFlags
	{
		static const uint LandTile0PBR = (1 << 0);
		static const uint LandTile1PBR = (1 << 1);
		static const uint LandTile2PBR = (1 << 2);
		static const uint LandTile3PBR = (1 << 3);
		static const uint LandTile4PBR = (1 << 4);
		static const uint LandTile5PBR = (1 << 5);
		static const uint LandTile0HasDisplacement = (1 << 6);
		static const uint LandTile1HasDisplacement = (1 << 7);
		static const uint LandTile2HasDisplacement = (1 << 8);
		static const uint LandTile3HasDisplacement = (1 << 9);
		static const uint LandTile4HasDisplacement = (1 << 10);
		static const uint LandTile5HasDisplacement = (1 << 11);
		static const uint LandTile0HasGlint = (1 << 12);
		static const uint LandTile1HasGlint = (1 << 13);
		static const uint LandTile2HasGlint = (1 << 14);
		static const uint LandTile3HasGlint = (1 << 15);
		static const uint LandTile4HasGlint = (1 << 16);
		static const uint LandTile5HasGlint = (1 << 17);
	}

	/// @brief Evaluate GGX microfacet specular BRDF (D * Vis * F)
	/// @param roughness Perceptual roughness [0,1]
	/// @param F0 Reflectance at normal incidence
	/// @param NdotL Dot product of normal and light direction
	/// @param NdotV Dot product of normal and view direction
	/// @param NdotH Dot product of normal and half vector
	/// @param VdotH Dot product of view and half vector
	/// @param F Output Fresnel reflectance at current angle
	/// @return Specular BRDF value (D * Vis * F)
	float3 SpecularMicrofacet(float roughness, float3 F0, float NdotL, float NdotV, float NdotH, float VdotH, out float3 F)
	{
		float D = BRDF::D_GGX(roughness, NdotH);
		float G = BRDF::Vis_SmithJointApprox(roughness, NdotV, NdotL);
		F = BRDF::F_Schlick(F0, VdotH);

		return D * G * F;
	}

	/// @brief Evaluate Charlie microflake specular BRDF for sheen/fabric (D * Vis * F)
	/// @param roughness Perceptual roughness [0,1]
	/// @param F0 Reflectance at normal incidence
	/// @param NdotL Dot product of normal and light direction
	/// @param NdotV Dot product of normal and view direction
	/// @param NdotH Dot product of normal and half vector
	/// @param VdotH Dot product of view and half vector
	/// @return Specular BRDF value (D * Vis * F)
	float3 SpecularMicroflakes(float roughness, float3 F0, float NdotL, float NdotV, float NdotH, float VdotH)
	{
		float D = BRDF::D_Charlie(roughness, NdotH);
		float G = BRDF::Vis_Neubelt(NdotV, NdotL);
		float3 F = BRDF::F_Schlick(F0, VdotH);

		return D * G * F;
	}

	/// @brief Directional albedo of a GGX specular lobe from the split-sum environment BRDF
	/// @param F0 Reflectance at normal incidence
	/// @param roughness Perceptual roughness [0,1]
	/// @param NdotV Dot product of normal and view direction
	/// @return Fraction of light the lobe reflects toward the viewer, used for OpenPBR albedo scaling of the layers below
	float3 SpecularDirectionalAlbedo(float3 F0, float roughness, float NdotV)
	{
		float2 specularBRDF = BRDF::EnvBRDF(roughness, NdotV);
		return F0 * specularBRDF.x + specularBRDF.y;
	}

#if defined(TRUE_PBR) && defined(LIGHTING) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	Texture2D<float2> TexIceEnergyLUT : register(t21);

	/// @brief Energy compensation for the ice lobes (Turquin 2019), baked at IOR 1.31 by IceEnergyLUTCS.hlsl
	/// @param roughness Perceptual roughness [0,1]
	/// @param NdotV Dot product of normal and view direction
	/// @return Scale for the reflected and transmitted lobes, x entering the ice and y leaving it
	/// @note Bilinear Loads need no sampler slot. An unbound table reads 0 and clamps to 1.
	float2 IceEnergyCompensation(float roughness, float NdotV)
	{
		static const int size = 32;
		float2 coord = saturate(float2(NdotV, roughness)) * (size - 1);
		int2 p = min(int2(coord), size - 2);
		float2 f = coord - p;
		float2 c00 = TexIceEnergyLUT.Load(int3(p, 0));
		float2 c10 = TexIceEnergyLUT.Load(int3(p + int2(1, 0), 0));
		float2 c01 = TexIceEnergyLUT.Load(int3(p + int2(0, 1), 0));
		float2 c11 = TexIceEnergyLUT.Load(int3(p + int2(1, 1), 0));
		return max(lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y), 1.0f);
	}
#endif

	/// @brief Share of light crossing the ice surface, which is what the compensated reflection lobe does not reflect
	/// @param roughness Perceptual roughness [0,1]
	/// @param NdotX Dot product of normal and the outside direction
	/// @param compensation Energy compensation of the crossing
	float IceInterfaceTransmittance(float roughness, float NdotX, float compensation)
	{
		return saturate(1.0f - compensation * SpecularDirectionalAlbedo(Constants::IceF0, roughness, NdotX).x);
	}

	/// @brief Length of the path to a given depth inside the ice for light refracted from an outside direction
	/// @param depth Depth below the surface in game units
	/// @param NdotX Dot product of normal and the outside direction
	/// @note The refracted cosine never drops below cos(49.8) at IOR 1.31, so the division is safe.
	float IcePathLength(float depth, float NdotX)
	{
		float cosThetaT = sqrt(1.0f - (1.0f - NdotX * NdotX) / (Constants::IceIOR * Constants::IceIOR));
		return max(depth, 0.0f) / cosThetaT;
	}

	/// @brief Beer-Lambert transmittance through ice
	/// @param absorption Absorption coefficient per game unit
	/// @param pathLength Path length in game units
	float3 IceAbsorption(float3 absorption, float pathLength)
	{
		return exp(-absorption * pathLength);
	}

	/// @brief Dominant direction of the rough transmission lobe for the view ray entering the ice
	/// @param N Surface normal, facing the viewer
	/// @param V View direction
	/// @param roughness Perceptual roughness [0,1]
	/// @note Fit to the GGX BTDF at IOR 1.31, off by 0.8 degrees on average and 2.5 at most. The peak leans toward -V as roughness grows.
	float3 GetIceTransmissionDirection(float3 N, float3 V, float roughness)
	{
		float NdotV = saturate(dot(N, V));
		float3 T = refract(-V, N, 1.0f / Constants::IceIOR);
		float a = roughness * roughness;
		float s = a / (a + 0.67f * sqrt(NdotV) + 0.17f);
		return normalize(lerp(T, -V, s));
	}

	/// @brief Rough dielectric transmission for light leaving the ice toward the viewer
	/// @param roughness Perceptual roughness [0,1]
	/// @param N Surface normal, facing the viewer
	/// @param V View direction
	/// @param L Light direction, below the surface
	/// @return Transmission BTDF times the cosine on the light side
	/// @note [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"], eq. 21 with the light inside the ice.
	float IceTransmissionLobe(float roughness, float3 N, float3 V, float3 L)
	{
		const float eta = Constants::IceIOR;
		float3 H = normalize(-(eta * L + V));
		H = dot(N, H) < 0.0f ? -H : H;
		float VdotH = dot(V, H);
		float LdotH = dot(L, H);
		float NdotV = clamp(dot(N, V), EPSILON_DOT_CLAMP, 1.0f);
		float NdotL = clamp(-dot(N, L), EPSILON_DOT_CLAMP, 1.0f);
		float D = BRDF::D_GGX(roughness, saturate(dot(N, H)));
		float visibility = BRDF::Vis_SmithJoint(roughness, NdotV, NdotL);
		float F = BRDF::F_DielectricCos(1.0f / eta, LdotH);
		float denominator = eta * LdotH + VdotH;
		float lobe = 4.0f * visibility * D * (1.0f - F) * abs(VdotH * LdotH) / max(denominator * denominator, EPSILON_DIVISION) * NdotL;
		return VdotH * LdotH < 0.0f ? lobe : 0.0f;
	}

	float3 GetThinSurfaceNormal(float3 N, float3 V)
	{
		return dot(N, V) < 0.0f ? -N : N;
	}

	// Bent vertex normals do not identify the visible face. Derivatives require uniform control flow.
	float3 GetThinSurfaceFaceNormal(float3 worldPosition, float3 V, float3 fallbackNormal)
	{
		float3 faceNormal = cross(ddx(worldPosition), ddy(worldPosition));
		float lengthSq = dot(faceNormal, faceNormal);
		faceNormal = lengthSq > 1e-12f ? faceNormal * rsqrt(lengthSq) : fallbackNormal;
		return GetThinSurfaceNormal(faceNormal, V);
	}

	void GetThinSurfaceSubsurfaceAlbedos(float3 baseColor, float3 scatteringColor, float opacity, float balance,
		out float reflectionWeight, out float3 frontTransmissionAlbedo, out float3 backTransmissionAlbedo)
	{
		float subsurfaceShare = 1.0f - saturate(opacity);
		float backShare = saturate(balance);
		// Light scattered inside the sheet is no longer available for surface reflection.
		float3 weights = float3(subsurfaceShare * (1.0f - backShare), subsurfaceShare * backShare, 1.0f - subsurfaceShare);
		float3 litFaceAlbedo = weights.z * baseColor + weights.x * scatteringColor;
#if defined(TRUE_PBR) && (defined(TREE_ANIM) || defined(LIGHTING) || defined(GRASS_LIGHTING))
		// Subsurface share and balance are linear energy shares, not gamma-encoded material colors.
#	if !defined(GRASS_LIGHTING)
		if (IsThinSurfaceMaterial(PBRFlags))
#	endif
		{
			// Both lit-face lobes leave the same side, so they must sum before encoding.
			litFaceAlbedo = Color::LinearToPBRMaterial(weights.z * Color::PBRMaterialToLinear(baseColor) + weights.x * Color::PBRMaterialToLinear(scatteringColor));
			weights = Color::LinearToPBRMaterial(weights);
		}
#endif
		reflectionWeight = weights.z;
		frontTransmissionAlbedo = saturate(litFaceAlbedo - baseColor * weights.z);
		backTransmissionAlbedo = saturate(scatteringColor * weights.y);
	}

	// Light crossing a sheet exits away from a face turned toward the light, whatever its bent normal says.
	float GetThinSurfaceBackTransmissionVisibility(float3 faceNormal, float3 L)
	{
		return 1.0f - saturate(4.0f * dot(faceNormal, L));
	}

	// The squared denominator preserves lobe energy as wrap broadens the terminator.
	float2 GetThinSurfaceDiffuseCosines(float NdotL, float wrap)
	{
		float normalization = 1.0f / ((1.0f + wrap) * (1.0f + wrap));
		return float2(saturate((NdotL + wrap) * normalization), saturate((wrap - NdotL) * normalization));
	}

	/** @brief Evaluates thin-surface diffuse lobes with separate reflection and transmission shadows. */
	void GetThinSurfaceDirectScattering(float3 reflectionAlbedo, float3 transmissionAlbedo, float3 irradiance,
		float NdotL, float wrap, float reflectionShadow, float transmissionShadow,
		out float3 reflection, out float3 transmission)
	{
		float2 diffuseCosines = GetThinSurfaceDiffuseCosines(NdotL, wrap);
		reflection = reflectionAlbedo * diffuseCosines.x * irradiance * reflectionShadow;
		transmission = transmissionAlbedo * diffuseCosines.y * irradiance * transmissionShadow;
	}

	void GetThinSurfaceDirectScattering(float3 reflectionAlbedo, float3 frontTransmissionAlbedo, float3 backTransmissionAlbedo,
		float3 shineThroughAlbedo, float opacity, float3 irradiance, float NdotL, float wrap, float reflectionShadow, float2 transmissionShadows,
		out float3 reflection, out float3 transmission)
	{
		float3 transmittance = (1.0f - saturate(opacity)).xxx;
#if defined(TRUE_PBR) && (defined(TREE_ANIM) || defined(LIGHTING) || defined(GRASS_LIGHTING))
#	if !defined(GRASS_LIGHTING)
		if (IsThinSurfaceMaterial(PBRFlags))
#	endif
			transmittance = Color::LinearToPBRMaterial(transmittance);
#endif
		// Share the wrapped shading response so flat sheet boundaries do not split the transmission lobe.
		float3 transmissionAlbedo = saturate(shineThroughAlbedo * transmittance + backTransmissionAlbedo);
		GetThinSurfaceDirectScattering(reflectionAlbedo, transmissionAlbedo, irradiance,
			NdotL, wrap, reflectionShadow, transmissionShadows.y, reflection, transmission);
		transmission += frontTransmissionAlbedo * saturate(NdotL) * irradiance * transmissionShadows.x;
	}

	// Normal-variance filtering from Tokuyoshi and Kaplanyan (2019).
	float FilterFoliageRoughness(float roughness, float3 normalDx, float3 normalDy)
	{
		float variance = 0.25f * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
		float kernelRoughnessSq = min(2.0f * variance, 0.18f);
		float roughnessSq = roughness * roughness;
		return sqrt(sqrt(saturate(roughnessSq * roughnessSq + kernelRoughnessSq)));
	}

	// Approximate canopy transmission using receiver opacity when occluder material is unavailable.
	float GetFoliageShadowVisibility(float shadow, float opacity)
	{
		return lerp(1.0f - saturate(opacity), 1.0f, saturate(shadow));
	}

	/// @brief Directional albedo of the OpenPBR fuzz layer (Zeltner et al. 2022 sheen)
	/// @param NdotV Dot product of normal and view direction
	/// @param roughness Fuzz roughness [0.01,1]: low is fibre-like, high is dusty
	/// @return Fraction of light the fuzz reflects; the layers below receive the rest
	/// @note Rational fits from the MaterialX reference implementation (mx_microfacet_sheen.glsl).
	float FuzzDirectionalAlbedo(float NdotV, float roughness)
	{
		float s = roughness * (0.0206607 + 1.58491 * roughness) / (0.0379424 + roughness * (1.32227 + roughness));
		float m = roughness * (-0.193854 + roughness * (-1.14885 + roughness * (1.7932 - 0.95943 * roughness * roughness))) / (0.046391 + roughness);
		float o = roughness * (0.000654023 + (-0.0207818 + 0.119681 * roughness) * roughness) / (1.26264 + roughness * (-1.92021 + roughness));
		float d = (NdotV - m) / s;
		return exp(-0.5 * d * d) / (s * sqrt(Math::TAU)) + o;
	}

	/// @brief Evaluate the OpenPBR fuzz lobe (Zeltner et al. 2022 sheen) as a linearly transformed cosine
	/// @param L Light direction
	/// @param V View direction
	/// @param N Surface normal, facing the viewer
	/// @param NdotV Dot product of normal and view direction
	/// @param roughness Fuzz roughness [0.01,1]
	/// @return Normalized lobe value with the cosine included; scale by FuzzDirectionalAlbedo for the reflection
	/// @note Rational fits from the MaterialX reference implementation (mx_microfacet_sheen.glsl).
	float FuzzLobe(float3 L, float3 V, float3 N, float NdotV, float roughness)
	{
		float3 X = V - N * NdotV;
		float lengthSquared = dot(X, X);
		X = lengthSquared > 1e-8 ? X * rsqrt(lengthSquared) : normalize(abs(N.z) < 0.999 ? cross(float3(0, 0, 1), N) : float3(1, 0, 0));
		float3 Y = cross(N, X);
		float3 w = float3(dot(L, X), dot(L, Y), dot(L, N));

		float aInv = (2.58126 * NdotV + 0.813703 * roughness) * roughness / (1.0 + 0.310327 * NdotV * NdotV + 2.60994 * NdotV * roughness);
		float bInv = sqrt(1.0 - NdotV) * (roughness - 1.0) * roughness * roughness * roughness /
		             (0.0000254053 + 1.71228 * NdotV - 1.71506 * NdotV * roughness + 1.34174 * roughness * roughness);
		float3 wo = float3(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
		float scale = aInv / dot(wo, wo);
		return max(wo.z, 0.0) * Math::INV_PI * scale * scale;
	}

	/// @brief Evaluate the fuzz lobe on the viewer-side face of a thin-walled surface lit from behind
	/// @param L Light direction, behind the surface
	/// @param V View direction
	/// @param N Surface normal, facing the viewer
	/// @param NdotV Dot product of normal and view direction
	/// @param roughness Fuzz roughness [0.01,1]
	/// @return Lobe value as FuzzLobe, zero for light in front; scale by the transmitted share of the light
	/// @note OpenPBR thin-walled surfaces carry fuzz on both faces. Light transmitted through the surface reaches the
	///       viewer-side fuzz continuing from the light, so that face sees it from the light's mirror image.
	float FuzzLobeTransmitted(float3 L, float3 V, float3 N, float NdotV, float roughness)
	{
		return FuzzLobe(reflect(L, N), V, N, NdotV, roughness);
	}

	/// @brief Calculate index of refraction for hair using Marschner model
	/// @return Effective IOR for hair (approximately 1.55)
	float HairIOR()
	{
		const float n = 1.55;
		const float a = 1;

		float ior1 = 2 * (n - 1) * (a * a) - n + 2;
		float ior2 = 2 * (n - 1) / (a * a) - n + 2;
		return 0.5f * ((ior1 + ior2) + 0.5f * (ior1 - ior2));  //assume cos2PhiH = 0.5f
	}

	/// @brief Convert index of refraction to F0 (reflectance at normal incidence)
	/// @param IOR Index of refraction
	/// @return F0 reflectance value
	float IORToF0(float IOR)
	{
		return pow((1 - IOR) / (1 + IOR), 2);
	}

	/// @brief Gaussian distribution for hair scattering
	/// @param B Standard deviation (roughness parameter)
	/// @param Theta Angle offset from peak
	/// @return Gaussian weight
	inline float HairGaussian(float B, float Theta)
	{
		// Guard against division by zero: clamp B to a minimum value
		float B_safe = max(B, EPSILON_DIVISION);
		return exp(-0.5 * Theta * Theta / (B_safe * B_safe)) / (sqrt(Math::TAU) * B_safe);
	}

	/// @brief Calculate wetness specular contribution for direct lighting
	/// @param N Surface normal
	/// @param V View direction
	/// @param L Light direction
	/// @param lightColor Light color/intensity
	/// @param roughness Surface roughness
	/// @return Wetness specular color contribution
	float3 GetWetnessDirectLightSpecularInput(float3 N, float3 V, float3 L, float3 lightColor, float roughness)
	{
		const float wetnessF0 = 0.02;

		float3 H = normalize(V + L);
		float NdotL = clamp(dot(N, L), EPSILON_DOT_CLAMP, 1);
		float NdotV = saturate(abs(dot(N, V)) + EPSILON_DOT_CLAMP);
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(V, H));

		float3 F;
		float3 Fr = SpecularMicrofacet(roughness, wetnessF0, NdotL, NdotV, NdotH, VdotH, F);

		return Fr * lightColor * NdotL;
	}

	/// @brief Calculate wetness specular lobe weight for indirect lighting
	/// @param N Surface normal
	/// @param V View direction
	/// @param roughness Surface roughness
	/// @return Wetness specular lobe weight
	float3 GetWetnessIndirectSpecularLobeWeight(float3 N, float3 V, float roughness)
	{
		const float wetnessF0 = 0.02;

		float NdotV = saturate(abs(dot(N, V)) + EPSILON_DOT_CLAMP);
		float2 specularBRDF = BRDF::EnvBRDF(roughness, NdotV);
		float3 specularLobeWeight = wetnessF0 * specularBRDF.x + specularBRDF.y;

		return specularLobeWeight;
	}
}

#endif  // __PBR_MATH_HLSL__
