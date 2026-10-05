#ifndef __PBR_MATH_HLSL__
#define __PBR_MATH_HLSL__

#include "Common/BRDF.hlsli"
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

	/// @brief Evaluate anisotropic GGX microfacet specular BRDF (D * Vis * F) with OpenPBR's roughness parametrization
	/// @param roughness Perceptual roughness [0,1]
	/// @param anisotropy OpenPBR specular_roughness_anisotropy [0,1]: how far the lobe stretches along the tangent
	/// @param F0 Reflectance at normal incidence
	/// @param N Surface normal
	/// @param T Unit tangent perpendicular to N; the lobe stretches along it
	/// @param V View direction
	/// @param L Light direction
	/// @param F Output Fresnel reflectance at current angle
	/// @param slopeVariance Isotropic filter variance added to both squared anisotropic roughness axes
	/// @return Specular BRDF value (D * Vis * F)
	float3 SpecularMicrofacetAnisotropic(float roughness, float anisotropy, float3 F0, float3 N, float3 T, float3 V, float3 L, out float3 F, float slopeVariance = 0.0)
	{
		F = F0;
		float NdotL = dot(N, L);
		float NdotV = dot(N, V);
		float3 halfVector = V + L;
		float halfVectorLengthSquared = dot(halfVector, halfVector);
		// Reflection needs both directions above the surface. Clamping a back-facing half vector can make D singular.
		if (NdotL <= 0.0 || NdotV <= 0.0 || halfVectorLengthSquared <= 1e-8)
			return 0.0;
		float3 H = halfVector * rsqrt(halfVectorLengthSquared);
		float NdotH = dot(N, H);
		if (NdotH <= 0.0)
			return 0.0;

		// OpenPBR: alpha_t = r^2 sqrt(2 / (1 + (1 - a)^2)), alpha_b = (1 - a) alpha_t, which keeps the average roughness.
		float alphaT = max(roughness * roughness * sqrt(2.0 / (1.0 + (1.0 - anisotropy) * (1.0 - anisotropy))), 1e-3);
		float alphaB = max((1.0 - anisotropy) * alphaT, 1e-3);
		// Filter after stretching the lobe so anisotropy cannot shrink the pixel footprint along its narrow axis.
		alphaT = sqrt(alphaT * alphaT + slopeVariance);
		alphaB = sqrt(alphaB * alphaB + slopeVariance);
		float3 B = cross(N, T);
		NdotL = saturate(NdotL);
		NdotV = saturate(NdotV) + EPSILON_DOT_CLAMP;
		float D = BRDF::D_AnisoGGX(alphaT, alphaB, saturate(NdotH), dot(T, H), dot(B, H));
		float G = BRDF::Vis_SmithJointAniso(alphaT, alphaB, NdotL, NdotV, dot(T, L), dot(B, L), dot(T, V), dot(B, V));
		F = BRDF::F_Schlick(F0, saturate(dot(V, H)));

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

	/// @brief Widen a microfacet lobe by an extra slope variance, as OpenPBR's coat roughening does
	/// @param roughness Perceptual roughness [0,1] of the lobe
	/// @param slopeVariance Added slope variance, in perceptual roughness to the fourth power
	/// @return Perceptual roughness of the widened lobe
	float AddRoughnessVariance(float roughness, float slopeVariance)
	{
		float roughness2 = roughness * roughness;
		return sqrt(sqrt(min(roughness2 * roughness2 + slopeVariance, 1.0)));
	}

	/// @brief Slope variance for geometric specular anti-aliasing (Tokuyoshi and Kaplanyan 2019)
	/// Widens unresolved highlights by the normal variation across the pixel.
	/// @param dNdx Screen-space x derivative of the specular normal
	/// @param dNdy Screen-space y derivative of the specular normal
	/// @param pixelVariance Variance of the pixel filter kernel
	/// @param maxVariance Upper bound on the added variance
	/// @return Slope variance to pass to AddRoughnessVariance
	float SpecularAntiAliasingVariance(float3 dNdx, float3 dNdy, float pixelVariance, float maxVariance)
	{
		return min(2.0 * pixelVariance * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), maxVariance);
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
		// Orient the lobe in the plane of the view and the normal.
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
		return SpecularDirectionalAlbedo(wetnessF0, roughness, NdotV);
	}
}

#endif  // __PBR_MATH_HLSL__
