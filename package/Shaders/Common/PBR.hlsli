#ifndef __PBR_DEPENDENCY_HLSL__
#define __PBR_DEPENDENCY_HLSL__
#include "Common/LightingCommon.hlsli"

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/PBRMath.hlsli"
#include "Common/Random.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"

namespace PBR
{
#if defined(GLINT)
	float3 SpecularMicrofacetWithGlint(float noise, float roughness, float3 F0, float NdotL, float NdotV, float NdotH, float VdotH, float glintH,
		float logDensity, float microfacetRoughness, float densityRandomization, Glints::GlintCachedVars glintCache,
		out float3 F)
	{
		float D = BRDF::D_GGX(roughness, NdotH);
		[branch] if (logDensity > 1.1)
		{
			float D_max = BRDF::D_GGX(roughness, 1);
			D = Glints::SampleGlints2023NDF(noise, logDensity, microfacetRoughness, densityRandomization, glintCache, glintH, D, D_max).x;
		}
		float G = BRDF::Vis_SmithJointApprox(roughness, NdotV, NdotL);
		F = BRDF::F_Schlick(F0, VdotH);

		return D * G * F;
	}
#endif

	float3 GetHairDiffuseColorMarschner(float3 N, float3 V, float3 L, float NdotL, float NdotV, float VdotL, float backlit, float area, MaterialProperties material)
	{
		float3 S = 0;

		float cosThetaL = sqrt(max(0, 1 - NdotL * NdotL));
		float cosThetaV = sqrt(max(0, 1 - NdotV * NdotV));
		float cosThetaD = sqrt((1 + cosThetaL * cosThetaV + NdotV * NdotL) / 2.0);

		const float3 Lp = L - NdotL * N;
		const float3 Vp = V - NdotV * N;
		const float cosPhi = dot(Lp, Vp) * rsqrt(dot(Lp, Lp) * dot(Vp, Vp) + EPSILON_DIVISION);
		const float cosHalfPhi = sqrt(saturate(0.5 + 0.5 * cosPhi));

		float n_prime = 1.19 / cosThetaD + 0.36 * cosThetaD;

		const float Shift = 0.0499f;
		const float Alpha[] = {
			-Shift * 2,
			Shift,
			Shift * 4
		};
		float B[] = {
			area + material.Roughness,
			area + material.Roughness / 2,
			area + material.Roughness * 2
		};

		float hairIOR = HairIOR();
		float F0 = IORToF0(hairIOR);

		float3 Tp;
		float Mp, Np, Fp, a, h, f;
		float ThetaH = NdotL + NdotV;
		// R
		Mp = HairGaussian(B[0], ThetaH - Alpha[0]);
		Np = 0.25 * cosHalfPhi;
		Fp = BRDF::F_Schlick(F0, sqrt(saturate(0.5 + 0.5 * VdotL))).x;
		S += (Mp * Np) * (Fp * lerp(1, backlit, saturate(-VdotL)));

		// TT
		Mp = HairGaussian(B[1], ThetaH - Alpha[1]);
		a = (1.55f / hairIOR) * rcp(n_prime);
		h = cosHalfPhi * (1 + a * (0.6 - 0.8 * cosPhi));
		f = BRDF::F_Schlick(F0, cosThetaD * sqrt(saturate(1 - h * h))).x;
		Fp = (1 - f) * (1 - f);
		Tp = pow(abs(material.BaseColor), 0.5 * sqrt(1 - (h * a) * (h * a)) / cosThetaD);
		Np = exp(-3.65 * cosPhi - 3.98);
		S += (Mp * Np) * (Fp * Tp) * backlit;

		// TRT
		Mp = HairGaussian(B[2], ThetaH - Alpha[2]);
		f = BRDF::F_Schlick(F0, cosThetaD * 0.5f).x;
		Fp = (1 - f) * (1 - f) * f;
		Tp = pow(abs(material.BaseColor), 0.8 / cosThetaD);
		Np = exp(17 * cosPhi - 16.78);
		S += (Mp * Np) * (Fp * Tp);

		return S;
	}

	float3 GetHairDiffuseAttenuationKajiyaKay(float3 N, float3 V, float3 L, float NdotL, float NdotV, float shadow, MaterialProperties material)
	{
		float3 S = 0;

		float diffuseKajiya = 1 - abs(NdotL);

		float3 fakeN = normalize(V - N * NdotV);
		const float wrap = 1;
		float wrappedNdotL = saturate((dot(fakeN, L) + wrap) / ((1 + wrap) * (1 + wrap)));
		float diffuseScatter = (1 / Math::PI) * lerp(wrappedNdotL, diffuseKajiya, 0.33);
		float luma = Color::RGBToLuminance(material.BaseColor);
		float3 scatterTint = pow(material.BaseColor / max(luma, 1e-5), 1 - shadow);
		S += sqrt(material.BaseColor) * diffuseScatter * scatterTint;

		return S;
	}

	float3 GetHairColorMarschner(float3 N, float3 V, float3 L, float NdotL, float NdotV, float VdotL, float shadow, float backlit, float area, MaterialProperties material)
	{
		float3 color = 0;

		color += GetHairDiffuseColorMarschner(N, V, L, NdotL, NdotV, VdotL, backlit, area, material);
		color += GetHairDiffuseAttenuationKajiyaKay(N, V, L, NdotL, NdotV, shadow, material);

		return color;
	}

	struct ThinSurface
	{
		float3 reflectionAlbedo;
		float3 frontTransmissionAlbedo;
		float3 backTransmissionAlbedo;
		float3 specularAlbedo;
		float3 fuzzColor;
		float fuzzAlbedo;
		float fuzzRoughness;
	};

	ThinSurface CreateThinSurface(MaterialProperties material, float NdotV, bool doSpecular, bool reflectionIncludesBaseColor = true)
	{
		ThinSurface surface = (ThinSurface)0;
		surface.reflectionAlbedo = reflectionIncludesBaseColor ? saturate(material.BaseColor) : 1.0f;
		[branch] if ((PBRFlags & Flags::Subsurface) != 0)
		{
			float reflectionWeight;
			GetThinSurfaceSubsurfaceAlbedos(saturate(material.BaseColor), saturate(material.SubsurfaceColor), material.Thickness, material.SubsurfaceBalance,
				reflectionWeight, surface.frontTransmissionAlbedo, surface.backTransmissionAlbedo);
			surface.reflectionAlbedo *= reflectionWeight;
		}

		[branch] if (doSpecular)
		{
			surface.specularAlbedo = saturate(material.ThinSurfaceSpecularWeight) * saturate(SpecularDirectionalAlbedo(material.F0, material.Roughness, NdotV));
			[branch] if ((PBRFlags & Flags::Fuzz) != 0)
			{
				surface.fuzzRoughness = clamp(material.Roughness, 0.01f, 1.0f);
				surface.fuzzAlbedo = saturate(saturate(material.FuzzWeight) * FuzzDirectionalAlbedo(NdotV, surface.fuzzRoughness));
				surface.fuzzColor = saturate(material.FuzzColor);
			}
		}
		return surface;
	}

	void GetDirectLightInputThinSurface(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material,
		ThinSurface surface, bool doSpecular, float wrap, float glintH = 0.0f)
	{
		lightingOutput = (DirectLightingOutput)0;
		float3 N = context.worldNormal;
		float3 V = context.viewDir;
		float3 L = context.lightDir;
		float NdotL = dot(N, L);
		float3 specularNormal = GetThinSurfaceNormal(N, V);
		float NdotV = clamp(dot(specularNormal, V), EPSILON_DOT_CLAMP, 1.0f);
		float specularNdotL = saturate(dot(specularNormal, L));
		float fuzzThroughput = 1.0f - surface.fuzzAlbedo;
		float3 scatteringThroughput = (1.0f - surface.specularAlbedo) * fuzzThroughput;
		float3 irradiance = context.lightColor * BRDF::Diffuse_Lambert() * scatteringThroughput;
		float3 shineThroughAlbedo = (PBRFlags & Flags::Subsurface) != 0 ? material.BaseColor : 0.0f;
		GetThinSurfaceDirectScattering(surface.reflectionAlbedo, surface.frontTransmissionAlbedo, surface.backTransmissionAlbedo, shineThroughAlbedo,
			material.Thickness, irradiance, NdotL, wrap, context.detailedShadow, float2(context.softShadow, context.softShadow * context.backTransmissionVisibility),
			lightingOutput.diffuse, lightingOutput.transmission);

		[branch] if (doSpecular)
		{
			float3 H = context.halfVector;
			float NdotH = saturate(dot(specularNormal, H));
			float VdotH = saturate(dot(V, H));
			float D = BRDF::D_GGX(material.Roughness, NdotH);
#if defined(GLINT)
			[branch] if (material.GlintLogMicrofacetDensity > 1.1f)
			{
				float Dmax = BRDF::D_GGX(material.Roughness, 1.0f);
				D = Glints::SampleGlints2023NDF(material.Noise, material.GlintLogMicrofacetDensity, material.GlintMicrofacetRoughness,
					material.GlintDensityRandomization, material.GlintCache, glintH, D, Dmax)
				        .x;
			}
#endif
			float visibility = BRDF::Vis_SmithJoint(material.Roughness, NdotV, max(specularNdotL, EPSILON_DOT_CLAMP));
			float3 F = BRDF::F_Schlick(material.F0, VdotH);
			float3 detailedLightColor = context.lightColor * context.detailedShadow;
			lightingOutput.specular = saturate(material.ThinSurfaceSpecularWeight) * D * visibility * F * detailedLightColor * specularNdotL * fuzzThroughput;
			[branch] if (surface.fuzzAlbedo > 0.0f)
			{
				float3 fuzzReflection = FuzzLobe(L, V, specularNormal, NdotV, surface.fuzzRoughness) * detailedLightColor;
				float3 fuzzTransmission = surface.backTransmissionAlbedo * FuzzLobeTransmitted(L, V, specularNormal, NdotV, surface.fuzzRoughness) *
				                          context.lightColor * context.softShadow * context.backTransmissionVisibility;
				lightingOutput.specular += surface.fuzzAlbedo * surface.fuzzColor * (fuzzReflection + fuzzTransmission);
			}
		}
	}

	void GetIndirectLobeWeightsThinSurface(out IndirectLobeWeights lobeWeights, IndirectContext context, MaterialProperties material, ThinSurface surface)
	{
		lobeWeights = (IndirectLobeWeights)0;
		float3 diffuseAO = MultiBounceAO(material.BaseColor, material.AO);
		if (IsThinSurfaceMaterial(PBRFlags))
			diffuseAO = saturate(diffuseAO);
		float3 scatteringThroughput = diffuseAO * (1.0f - surface.specularAlbedo) * (1.0f - surface.fuzzAlbedo);
		lobeWeights.diffuse = surface.reflectionAlbedo * scatteringThroughput + surface.fuzzAlbedo * surface.fuzzColor * material.AO;
		lobeWeights.frontTransmission = surface.frontTransmissionAlbedo * scatteringThroughput;
		lobeWeights.backTransmission = surface.backTransmissionAlbedo * scatteringThroughput;
		float NdotV = clamp(abs(dot(context.worldNormal, context.viewDir)), EPSILON_DOT_CLAMP, 1.0f);
		lobeWeights.specular = surface.specularAlbedo * (1.0f - surface.fuzzAlbedo) *
		                       SpecularOcclusion(NdotV, material.Roughness * material.Roughness, material.AO);
	}

	void GetDirectLightInput(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material, float3x3 tbnTr, float2 uv)
	{
		lightingOutput = (DirectLightingOutput)0;
		if (IsThinSurfaceMaterial(PBRFlags)) {
			// Direct diffuse receives BaseColor at the call site.
			ThinSurface surface = CreateThinSurface(material, saturate(abs(dot(context.worldNormal, context.viewDir))), true, false);
			GetDirectLightInputThinSurface(lightingOutput, context, material, surface, true, 0.0f, mul(tbnTr, context.halfVector).x);
			[branch] if (SharedData::truePBRSettings.EnableMicroShadows != 0)
			{
				float microShadow = lerp(1.0f, ApproximateDirectOcculusion(material.AO, dot(context.worldNormal, context.lightDir)), SharedData::truePBRSettings.MicroShadowStrength);
				lightingOutput.diffuse *= microShadow;
				lightingOutput.specular *= microShadow;
			}
			return;
		}
		const float3 detailedLightColor = context.lightColor * context.detailedShadow;
		const float3 softLightColor = context.lightColor * context.softShadow;

		const float3 N = context.worldNormal;
		const float3 V = context.viewDir;
		const float3 L = context.lightDir;
		const float3 H = context.halfVector;

		const float3 coatN = context.coatWorldNormal;
		const float3 coatV = context.coatViewDir;
		const float3 coatL = context.coatLightDir;
		const float3 coatH = context.coatHalfVector;

		float NdotL = dot(N, L);
		float NdotV = dot(N, V);
		float VdotL = dot(V, L);
		float NdotH = dot(N, H);
		float VdotH = dot(V, H);

		float satNdotL = clamp(NdotL, EPSILON_DOT_CLAMP, 1);
		float satNdotV = saturate(abs(NdotV) + EPSILON_DOT_CLAMP);
		float satVdotL = saturate(VdotL);
		float satNdotH = saturate(NdotH);
		float satVdotH = saturate(VdotH);

#if defined(TREE_ANIM)
		float specularNdotL = saturate(NdotL);
		float specularNdotV = clamp(NdotV, EPSILON_DOT_CLAMP, 1.0f);
#else
		float specularNdotL = satNdotL;
		float specularNdotV = satNdotV;
#endif

#if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
		[branch] if ((PBRFlags & Flags::HairMarschner) != 0)
		{
			lightingOutput.transmission += softLightColor * GetHairColorMarschner(N, V, L, NdotL, NdotV, VdotL, 0, 1, 0, material);
		}
		else
#endif
		{
			float3 F;  // Fresnel reflectance at current (V,H) angle
#if defined(GLINT)
			float3 Fr = SpecularMicrofacetWithGlint(material.Noise, material.Roughness, material.F0, max(specularNdotL, EPSILON_DOT_CLAMP), specularNdotV, satNdotH, satVdotH, mul(tbnTr, H).x,
				material.GlintLogMicrofacetDensity, material.GlintMicrofacetRoughness, material.GlintDensityRandomization, material.GlintCache, F);
#else
			float3 Fr = SpecularMicrofacet(material.Roughness, material.F0, max(specularNdotL, EPSILON_DOT_CLAMP), specularNdotV, satNdotH, satVdotH, F);
#endif
#if defined(TREE_ANIM)
			float3 kD = 1.0f - saturate(SpecularDirectionalAlbedo(material.F0, material.Roughness, saturate(abs(NdotV))));
#else
			float3 kD = 1 - F;
#endif

			lightingOutput.diffuse += detailedLightColor * satNdotL * BRDF::Diffuse_Lambert() * kD;
			lightingOutput.specular += Fr * detailedLightColor * specularNdotL;

#if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
			[branch] if ((PBRFlags & Flags::Fuzz) != 0)
			{
				float3 fuzzSpecular = SpecularMicroflakes(material.Roughness, material.FuzzColor, max(specularNdotL, EPSILON_DOT_CLAMP), specularNdotV, satNdotH, satVdotH) * detailedLightColor * specularNdotL;
				lightingOutput.specular = lerp(lightingOutput.specular, fuzzSpecular, material.FuzzWeight);
			}

#	if !defined(TREE_ANIM)
			[branch] if ((PBRFlags & Flags::Subsurface) != 0)
			{
				const float subsurfacePower = 12.234;
				float forwardScatter = exp2(saturate(-VdotL) * subsurfacePower - subsurfacePower);
				float backScatter = saturate(satNdotL * material.Thickness + (1.0 - material.Thickness)) * 0.5;
				float subsurface = lerp(backScatter, 1, forwardScatter) * (1.0 - material.Thickness);
				lightingOutput.transmission += material.SubsurfaceColor * subsurface * softLightColor * BRDF::Diffuse_Lambert() * kD;
			}
			else
#	endif
				if ((PBRFlags & Flags::TwoLayer) != 0)
			{
				float coatNdotL = satNdotL;
				float coatNdotV = satNdotV;
				float coatNdotH = satNdotH;
				float coatVdotH = satVdotH;
				[branch] if ((PBRFlags & Flags::CoatNormal) != 0)
				{
					coatNdotL = clamp(dot(coatN, coatL), EPSILON_DOT_CLAMP, 1);
					coatNdotV = saturate(abs(dot(coatN, coatV)) + EPSILON_DOT_CLAMP);
					coatNdotH = saturate(dot(coatN, coatH));
					coatVdotH = saturate(dot(coatV, coatH));
				}

				float3 coatF;
				float3 coatFr = SpecularMicrofacet(material.CoatRoughness, material.CoatF0, coatNdotL, coatNdotV, coatNdotH, coatVdotH, coatF);

				float3 layerAttenuation = 1 - coatF * material.CoatStrength;
				lightingOutput.diffuse *= layerAttenuation;
				lightingOutput.specular *= layerAttenuation;

				lightingOutput.coatDiffuse += context.coatLightColor * coatNdotL * BRDF::Diffuse_Lambert();
				lightingOutput.specular += coatFr * context.coatLightColor * coatNdotL * material.CoatStrength;
			}
#endif
		}

		[branch] if (SharedData::truePBRSettings.EnableMicroShadows != 0)
		{
			float microShadow = lerp(1.0, ApproximateDirectOcculusion(material.AO, NdotL), SharedData::truePBRSettings.MicroShadowStrength);
			lightingOutput.diffuse *= microShadow;
			lightingOutput.specular *= microShadow;
			lightingOutput.coatDiffuse *= microShadow;
		}
	}

	void GetIndirectLobeWeights(out IndirectLobeWeights lobeWeights, IndirectContext context, MaterialProperties material)
	{
		lobeWeights = (IndirectLobeWeights)0;
		if (IsThinSurfaceMaterial(PBRFlags)) {
			ThinSurface surface = CreateThinSurface(material, saturate(abs(dot(context.worldNormal, context.viewDir))), true);
			GetIndirectLobeWeightsThinSurface(lobeWeights, context, material, surface);
			return;
		}

		const float3 N = context.worldNormal;
		const float3 V = context.viewDir;

#if defined(TREE_ANIM)
		float NdotV = saturate(dot(GetThinSurfaceNormal(N, V), V));
#else
		float NdotV = saturate(dot(N, V));
#endif

#if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
		[branch] if ((PBRFlags & Flags::HairMarschner) != 0)
		{
			float3 L = normalize(V - N * dot(V, N));
			float NdotL = dot(N, L);
			float VdotL = dot(V, L);
			lobeWeights.diffuse = GetHairColorMarschner(N, V, L, NdotL, NdotV, VdotL, 1, 0, 0.2, material);
		}
		else
#endif
		{
			lobeWeights.diffuse = material.BaseColor;

#if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
#	if !defined(TREE_ANIM)
			[branch] if ((PBRFlags & Flags::Subsurface) != 0)
			{
				lobeWeights.diffuse += material.SubsurfaceColor * (1 - material.Thickness) / Math::PI;
			}
#	endif
			[branch] if ((PBRFlags & Flags::Fuzz) != 0)
			{
				lobeWeights.diffuse += material.FuzzColor * material.FuzzWeight;
			}
#endif
#if defined(TREE_ANIM)
			lobeWeights.specular = saturate(SpecularDirectionalAlbedo(material.F0, material.Roughness, NdotV));
#else
			lobeWeights.specular = SpecularDirectionalAlbedo(material.F0, material.Roughness, NdotV);
#endif

			// Energy conservation: diffuse receives only what specular does not reflect
			lobeWeights.diffuse *= 1 - lobeWeights.specular;

#if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
			[branch] if ((PBRFlags & Flags::TwoLayer) != 0)
			{
				float2 coatSpecularBRDF = BRDF::EnvBRDF(material.CoatRoughness, NdotV);
				float3 coatSpecularLobeSpecular = material.CoatF0 * coatSpecularBRDF.x + coatSpecularBRDF.y;

				float3 layerAttenuation = 1 - coatSpecularLobeSpecular * material.CoatStrength;
				lobeWeights.diffuse *= layerAttenuation;
				lobeWeights.specular *= layerAttenuation;

				[branch] if ((PBRFlags & Flags::ColoredCoat) != 0)
				{
					float3 coatDiffuseLobeWeight = material.CoatColor * (1 - coatSpecularLobeSpecular);
					lobeWeights.diffuse += coatDiffuseLobeWeight * material.CoatStrength;
				}
				lobeWeights.specular += coatSpecularLobeSpecular * material.CoatStrength;
			}
#endif
		}

		// Apply ambient occlusion with multi-bounce approximation
		lobeWeights.diffuse *= MultiBounceAO(material.BaseColor, material.AO);
		float alpha = material.Roughness * material.Roughness;
		lobeWeights.specular *= SpecularOcclusion(NdotV, alpha, material.AO);
	}

	/**
	 * World-space change in surface position per unit of UV, exact while UVs are affine across each triangle.
	 * @return False when the UV mapping is degenerate
	 */
	bool GetSurfaceUVAxes(float3 worldPosition, float2 uv, out float3 dPdu, out float3 dPdv)
	{
		const float3 dpdx = ddx(worldPosition);
		const float3 dpdy = ddy(worldPosition);
		const float2 duvdx = ddx(uv);
		const float2 duvdy = ddy(uv);

		// Inverse of the screen-to-UV Jacobian; the sign keeps mirrored UVs pointing the right way
		const float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
		dPdu = 0.0;
		dPdv = 0.0;
		if (abs(determinant) < 1e-20)
			return false;
		dPdu = (dpdx * duvdy.y - dpdy * duvdx.y) / determinant;
		dPdv = (dpdy * duvdx.x - dpdx * duvdy.x) / determinant;
		return true;
	}

	/**
	 * The surface's model space relative to the world, from the screen-space change of both positions across the
	 * triangle, which is exact for the rigid, uniformly scaled transforms that place shapes.
	 * @param worldNormal World-space normal on the front side of the surface
	 * @param modelNormal Front-face geometric normal in model space
	 * @param modelToWorld Scale from model to world units
	 * @param worldToModel Rotation taking world directions to model space, as mul(direction, worldToModel)
	 * @return False when the surface is degenerate on screen
	 */
	bool GetModelSurface(float3 worldPosition, float3 modelPosition, float3 worldNormal, out float3 modelNormal, out float modelToWorld, out float3x3 worldToModel)
	{
		const float3 worldDdx = ddx(worldPosition);
		const float3 worldDdy = ddy(worldPosition);
		const float3 modelDdx = ddx(modelPosition);
		const float3 modelDdy = ddy(modelPosition);
		const float3 worldCross = cross(worldDdx, worldDdy);
		const float3 modelCross = cross(modelDdx, modelDdy);

		modelNormal = 0.0;
		modelToWorld = 0.0;
		worldToModel = 0.0;
		const bool valid = dot(modelCross, modelCross) > 1e-20 && dot(worldCross, worldCross) > 1e-20;
		if (valid) {
			// Model and world windings agree, so the world winding against the front normal orients both normals
			const float side = dot(worldCross, worldNormal) < 0.0 ? -1.0 : 1.0;
			modelNormal = normalize(modelCross) * side;
			const float3 worldFrontNormal = normalize(worldCross) * side;
			modelToWorld = sqrt(dot(worldDdx, worldDdx) / dot(modelDdx, modelDdx));

			// Matching orthonormal frames on the surface in both spaces
			const float3 modelTangent = normalize(modelDdx);
			const float3 worldTangent = normalize(worldDdx);
			const float3x3 modelFrame = float3x3(modelTangent, cross(modelNormal, modelTangent), modelNormal);
			const float3x3 worldFrame = float3x3(worldTangent, cross(worldFrontNormal, worldTangent), worldFrontNormal);
			worldToModel = mul(transpose(worldFrame), modelFrame);
		}
		return valid;
	}

	/** A wall of windows that look into one shared room, in the model space of the shape. */
	struct InteriorWall
	{
		float3 normal;  // Out of the front face being shaded
		float3 right;   // Across the wall; up is normal x right
		float4 rect;    // The room's front wall as right min, right max, up min, up max
		float depth;    // Distance to the far side of the room, or 0 when the room was not fitted to the building
	};

	/**
	 * Finds the wall of windows a surface lies on, from the table in the window map's rows below its UV rows. Each row
	 * holds a wall facing out of the building as texels (normal, plane offset), window band (right min, right max, up min,
	 * up max), (right axis, depth behind the wall), room behind the wall (as the band), (depth in front of the wall),
	 * room in front of the wall.
	 * @param modelNormal Front-face normal in model space
	 */
	bool FindInteriorWall(Texture2D<float4> windowMap, uint2 windowMapSize, float3 modelPosition, float3 modelNormal, out InteriorWall wall)
	{
		// Covers sills and frames a little proud of the glass
		const float tolerance = 8.0;

		wall = (InteriorWall)0;
		bool found = false;
		[loop] for (uint row = windowMapSize.x / 2; row < windowMapSize.y && !found; row++)
		{
			const float4 plane = windowMap.Load(int3(0, row, 0));
			const float facing = dot(plane.xyz, modelNormal);
			if (abs(facing) > 0.95 && abs(dot(plane.xyz, modelPosition) - plane.w) < tolerance) {
				const float4 band = windowMap.Load(int3(1, row, 0));
				const float4 axis = windowMap.Load(int3(2, row, 0));
				const float2 position = float2(dot(axis.xyz, modelPosition), dot(cross(plane.xyz, axis.xyz), modelPosition));
				if (all(position > band.xz - tolerance) && all(position < band.yw + tolerance)) {
					// A face looking into the building has its room on the wall's outer side, seen mirrored
					const bool outside = facing > 0.0;
					const float4 rect = windowMap.Load(int3(outside ? 3 : 5, row, 0));
					wall.normal = outside ? plane.xyz : -plane.xyz;
					wall.right = outside ? axis.xyz : -axis.xyz;
					wall.rect = outside ? rect : float4(-rect.y, -rect.x, rect.zw);
					wall.depth = outside ? axis.w : windowMap.Load(int3(4, row, 0)).x;
					found = true;
				}
			}
		}
		return found;
	}

	/** A window behind which one room is shown, in an upright frame across the wall. */
	struct InteriorWindow
	{
		float2 uv;     // Position across the window, 0-1, x to the right and y down
		float2 uvDdx;  // Screen-space gradients of uv
		float2 uvDdy;
		float2 halfSize;       // Half width and height in game units, or 0 when unknown
		float3 viewDirection;  // Towards the camera, in the window frame (right, down, out of the wall)
		uint hash;             // Picks or varies the room behind the window
		float depth;           // Depth of a room fitted to the building in game units, or 0 for the default room
		float3 centre;         // Camera-relative world position of the room's front wall centre
		float3 right;          // World directions of the frame's x and of up, against its y
		float3 up;
	};

	/** Upright frame across a wall from its normal and world up, falling back to the tangent on floors and ceilings. */
	void GetInteriorFrame(float3x3 tbnTr, out float3 right, out float3 up, out float3 normal)
	{
		normal = normalize(tbnTr[2]);
		right = cross(float3(0.0, 0.0, 1.0), normal);
		right = dot(right, right) > 0.01 ? normalize(right) : normalize(tbnTr[0] - normal * dot(tbnTr[0], normal));
		up = cross(normal, right);
	}

	/**
	 * Finds the window at a surface position. Rooms are oriented by the wall and world up rather than by the texture,
	 * so mirrored or rotated UVs still show an upright room, and every piece of a window resolves to the same window.
	 * @param worldPosition Camera-relative surface position
	 * @param cameraPosition Camera position, to place the window in the world
	 * @param rect The whole window's UV rectangle (min u, min v, max u, max v) in this piece's frame relative to its
	 * tile, from the window map, or zero to use the whole UV tile
	 * @param hasUVAxes Whether dPdu and dPdv are valid
	 * @param viewDirection World-space direction from the surface towards the camera
	 */
	InteriorWindow GetInteriorWindow(float3 worldPosition, float3 cameraPosition, float2 uv, float4 rect, bool hasUVAxes, float3 dPdu, float3 dPdv, float3x3 tbnTr, float3 viewDirection)
	{
		const float2 tile = floor(uv);
		const float4 windowRect = float4(tile, tile) + (all(rect.zw > rect.xy) ? rect : float4(0.0, 0.0, 1.0, 1.0));

		float3 right, up, normal;
		GetInteriorFrame(tbnTr, right, up, normal);

		InteriorWindow window = (InteriorWindow)0;
		window.viewDirection = float3(dot(viewDirection, right), -dot(viewDirection, up), dot(viewDirection, normal));
		window.centre = worldPosition;
		window.right = right;
		window.up = up;

		if (hasUVAxes) {
			// Window centre relative to this pixel, and the window's extent along the upright axes
			const float2 centreUV = 0.5 * (windowRect.xy + windowRect.zw);
			const float3 toCentre = dPdu * (centreUV.x - uv.x) + dPdv * (centreUV.y - uv.y);
			const float3 halfU = dPdu * 0.5 * (windowRect.z - windowRect.x);
			const float3 halfV = dPdv * 0.5 * (windowRect.w - windowRect.y);
			window.halfSize = max(float2(abs(dot(halfU, right)) + abs(dot(halfV, right)), abs(dot(halfU, up)) + abs(dot(halfV, up))), 1e-3);

			window.uv = saturate(float2(-dot(toCentre, right), dot(toCentre, up)) / window.halfSize * 0.5 + 0.5);
			const float3 dpdx = ddx(worldPosition);
			const float3 dpdy = ddy(worldPosition);
			window.uvDdx = float2(dot(dpdx, right), -dot(dpdx, up)) / window.halfSize * 0.5;
			window.uvDdy = float2(dot(dpdy, right), -dot(dpdy, up)) / window.halfSize * 0.5;

			// Seeded by where the window is, so all of its pieces pick the same room
			window.centre = worldPosition + toCentre;
			window.hash = Random::murmur3(uint3(int3(floor((window.centre + cameraPosition) / 32.0))));
		} else {
			window.uv = saturate((uv - windowRect.xy) / (windowRect.zw - windowRect.xy));
			window.uvDdx = ddx(uv);
			window.uvDdy = ddy(uv);
			window.halfSize = 0.0;
			window.hash = Random::pcg2d(uint2(int2(tile))).x;
		}
		return window;
	}

	/**
	 * The room behind a wall of windows, so every window shows its own part of it. Room images are flat views, so the
	 * wall's room space is divided into rooms as wide as they are high, their shape; a room cubemap holds the whole
	 * room, so the whole wall looks into it. Works in the model's own frame, which the room space was fitted in, so the
	 * view stays true to the building's walls.
	 * @param worldPosition Camera-relative surface position
	 * @param cameraPosition Camera position, to place the wall in the world
	 * @param modelToWorld Scale from model to world units
	 * @param worldToModel Rotation taking world directions to model space
	 * @param roomSize Height of rooms not fitted to the building, in game units
	 * @param splitRooms Whether to divide the wall into rooms as wide as they are high
	 * @param viewDirection World-space direction from the surface towards the camera
	 */
	InteriorWindow GetInteriorWallWindow(float3 worldPosition, float3 cameraPosition, float3 modelPosition, float modelToWorld, float3x3 worldToModel, InteriorWall wall, float roomSize, bool splitRooms, float3 viewDirection)
	{
		const float3 up = cross(wall.normal, wall.right);

		InteriorWindow window = (InteriorWindow)0;
		const float3 modelView = mul(viewDirection, worldToModel);
		window.viewDirection = float3(dot(modelView, wall.right), -dot(modelView, up), dot(modelView, wall.normal));

		const float2 position = float2(dot(wall.right, modelPosition), dot(up, modelPosition));
		float4 rect = wall.rect;
		if (wall.depth <= 0.0) {
			// Unfitted rooms are at least roomSize high, centred on the windows
			const float grow = max(0.5 * (roomSize / modelToWorld - (rect.w - rect.z)), 0.0);
			rect.zw += float2(-grow, grow);
		}
		if (splitRooms) {
			const float height = max(rect.w - rect.z, 1e-3);
			const float rooms = max(round((rect.y - rect.x) / height), 1.0);
			const float roomWidth = (rect.y - rect.x) / rooms;
			rect.x += clamp(floor((position.x - rect.x) / roomWidth), 0.0, rooms - 1.0) * roomWidth;
			rect.y = rect.x + roomWidth;
		}

		// Offset from the room's front wall centre along its right and up axes, in model units
		const float2 offset = position - 0.5 * (rect.xz + rect.yw);
		const float2 halfSize = max(0.5 * (rect.yw - rect.xz), 1e-3);
		window.uv = saturate(float2(offset.x, -offset.y) / halfSize * 0.5 + 0.5);

		const float3 dpdx = ddx(modelPosition);
		const float3 dpdy = ddy(modelPosition);
		window.uvDdx = float2(dot(dpdx, wall.right), -dot(dpdx, up)) / halfSize * 0.5;
		window.uvDdy = float2(dot(dpdy, wall.right), -dot(dpdy, up)) / halfSize * 0.5;

		window.halfSize = halfSize * modelToWorld;
		window.depth = wall.depth * modelToWorld;

		// Seeded by where the room is, so all of its windows pick the same room
		const float3x3 modelToWorldRotation = transpose(worldToModel);
		window.centre = worldPosition - mul(wall.right * offset.x + up * offset.y, modelToWorldRotation) * modelToWorld;
		window.right = mul(wall.right, modelToWorldRotation);
		window.up = mul(up, modelToWorldRotation);
		window.hash = Random::murmur3(uint3(int3(floor((window.centre + cameraPosition) / 32.0))));
		return window;
	}

	/**
	 * Traces the view ray from the window to the wall of its room that it exits through, in game units. A window fitted
	 * to the building is the room's whole front wall. Otherwise the window is centred in the front wall, which is roomSize
	 * wide and high but never smaller than the window, so small windows show part of a full-size room.
	 * @param roomDepth Room depth in units of the front wall's mean half size, the depth the room images were made for
	 * @param depthScale Scale on that depth, which never takes a fitted room past the far side of the building
	 * @param windowScale Window size relative to the front wall
	 * @return Exit point with x and y in [-1, 1] across the front wall and z in [-1, 0] from the window to the back wall
	 */
	float3 TraceInteriorRoom(InteriorWindow window, float roomSize, float roomDepth, float depthScale, out float2 windowScale)
	{
		const float2 roomHalfSize = window.depth > 0.0 ? window.halfSize : max(0.5 * roomSize, window.halfSize);
		const float2 halfSize = any(window.halfSize <= 0.0) ? roomHalfSize : window.halfSize;
		const float imageDepth = depthScale * roomDepth * sqrt(roomHalfSize.x * roomHalfSize.y);
		const float depth = window.depth > 0.0 ? min(window.depth, imageDepth) : imageDepth;
		windowScale = halfSize / roomHalfSize;

		const float3 rayOrigin = float3((window.uv * 2.0 - 1.0) * halfSize, 0.0);
		float3 rayDir = -window.viewDirection;
		rayDir.x = abs(rayDir.x) < 1e-5 ? 1e-5 : rayDir.x;
		rayDir.y = abs(rayDir.y) < 1e-5 ? 1e-5 : rayDir.y;
		rayDir.z = min(rayDir.z, -1e-5);

		const float3 invDir = rcp(rayDir);
		const float2 sideWallDist = roomHalfSize * abs(invDir.xy) - rayOrigin.xy * invDir.xy;
		const float backWallDist = depth * abs(invDir.z);
		const float3 hit = rayOrigin + rayDir * min(min(sideWallDist.x, sideWallDist.y), backWallDist);
		return float3(hit.xy / roomHalfSize, hit.z / depth);
	}

	/** Room image depth in alpha is log(forward distance / nearest distance) / log(InteriorMaxDepthRatio) * InteriorDepthAlphaScale. */
	static const float InteriorMaxDepthRatio = 64.0;
	static const float InteriorDepthAlphaScale = 0.98;
	/** Room images with depth are 120 degree views, wide enough for windows seen at an angle. */
	static const float InteriorDepthImageTanHalfFov = 1.7320508;
	/**
	 * The nearest surfaces in a room image with depth lie this far behind the window, in game units, so the room it shows
	 * has the same size behind every window however big the space fitted behind the wall is.
	 */
	static const float InteriorDepthNearOffset = 96.0;
	/** Surfaces in a room image with depth are this thick, relative to their distance, so rays pass behind them. */
	static const float InteriorDepthThickness = 0.35;

	/** One room image cell of an atlas, sampled from an image position in [-1, 1] kept half a texel inside the cell. */
	float4 SampleInteriorCell(Texture2D<float4> atlas, SamplerState atlasSampler, float2 roomCell, float2 atlasScale, float2 cellDim, float2 imagePosition, float level)
	{
		const float2 inset = 0.5 / cellDim;
		return atlas.SampleLevel(atlasSampler, (roomCell + clamp(imagePosition * 0.5 + 0.5, inset, 1.0 - inset)) * atlasScale, level);
	}

	/**
	 * Forward distance from the image's camera to the surface at an image position, in game units, pressed smoothly so
	 * nothing lies more than depthRange behind the window.
	 */
	float GetInteriorSurfaceDistance(Texture2D<float4> atlas, SamplerState atlasSampler, float2 roomCell, float2 atlasScale, float2 cellDim, float2 imagePosition, float level, float cameraDistance, float nearDistance, float depthRange, float depthScale)
	{
		const float alpha = SampleInteriorCell(atlas, atlasSampler, roomCell, atlasScale, cellDim, imagePosition, level).a;
		const float ratio = exp2(alpha / InteriorDepthAlphaScale * log2(InteriorMaxDepthRatio));
		const float distance = nearDistance * (1.0 + (ratio - 1.0) * depthScale);
		return cameraDistance + depthRange * (1.0 - exp(-(distance - cameraDistance) / depthRange));
	}

	/** Whether a point at a distance along the image's view lies on or within the thickness of the surface there. */
	bool IsInsideInteriorSurface(float distance, float surfaceDistance)
	{
		return distance >= surfaceDistance && distance <= surfaceDistance * (1.0 + InteriorDepthThickness);
	}

	/**
	 * Marches the view ray through a room image with depth, a true perspective view with each pixel's distance in alpha.
	 * The image's camera sits in front of the window where its view just covers the room's front wall, and the scene is
	 * scaled about it so the nearest surfaces lie a little behind the window with everything else in its own proportions
	 * behind them, pressed smoothly into a room about as deep as it is wide so far surfaces still move with the viewer.
	 * Surfaces have a thickness, so rays passing behind something near find what lies beyond it instead of stretching
	 * its edge.
	 * @param depthScale Scale on the depth behind the nearest surfaces
	 * @param windowScale Window size relative to the front wall
	 * @param distanceScale Camera distance over the distance of the surface hit, to shrink texture gradients with distance
	 * @return Image position of the surface hit, in [-1, 1] with x right and y down
	 */
	float2 TraceInteriorDepth(Texture2D<float4> atlas, SamplerState atlasSampler, float2 roomCell, float2 atlasScale, float2 cellDim, InteriorWindow window, float roomSize, float depthScale, out float2 windowScale, out float distanceScale)
	{
		// Enough that a step is shorter than furniture is thick, which coarser marches show doubled where they cross it
		const uint steps = 64;
		const uint refinements = 4;

		const float2 roomHalfSize = window.depth > 0.0 ? window.halfSize : max(0.5 * roomSize, window.halfSize);
		const float2 halfSize = any(window.halfSize <= 0.0) ? roomHalfSize : window.halfSize;
		windowScale = halfSize / roomHalfSize;
		const float meanHalfSize = sqrt(roomHalfSize.x * roomHalfSize.y);
		const float cameraDistance = meanHalfSize / InteriorDepthImageTanHalfFov;
		const float nearDistance = cameraDistance + InteriorDepthNearOffset;
		// Near surfaces keep their depth; far ones are pressed towards a depth set relative to the room's width, as interior
		// mapping's rooms are about as deep as they are wide, or a deep hall's far end would follow the viewer like a backdrop
		const float depthRange = max(SharedData::truePBRSettings.InteriorDepthRange, 0.05) * 2.0 * meanHalfSize;
		const float maxDistance = cameraDistance + depthRange;

		const float3 rayOrigin = float3((window.uv * 2.0 - 1.0) * halfSize, 0.0);
		float3 rayDir = -window.viewDirection;
		rayDir.z = min(rayDir.z, -1e-3);

		// Depth is only read for occlusion, so a mip near the window's own footprint keeps the march stable
		const float footprint = max(length(window.uvDdx * windowScale * cellDim), length(window.uvDdy * windowScale * cellDim));
		const float level = max(log2(max(footprint, 1e-4)), 0.0) + 1.0;

		// Steps are even in inverse distance, like the image's own resolution of depth
		float frontDistance = cameraDistance;
		float farDistance = maxDistance;
		bool hit = false;
		[loop] for (uint step = 1; step <= steps && !hit; step++)
		{
			const float distance = rcp(lerp(rcp(cameraDistance), rcp(maxDistance), step / (float)steps));
			const float3 position = rayOrigin + rayDir * ((cameraDistance - distance) / rayDir.z);
			const float2 imagePosition = position.xy / roomHalfSize * (cameraDistance / distance);
			if (IsInsideInteriorSurface(distance, GetInteriorSurfaceDistance(atlas, atlasSampler, roomCell, atlasScale, cellDim, imagePosition, level, cameraDistance, nearDistance, depthRange, depthScale))) {
				farDistance = distance;
				hit = true;
			} else {
				frontDistance = distance;
			}
		}

		[unroll] for (uint refinement = 0; refinement < refinements; refinement++)
		{
			const float distance = rcp(0.5 * (rcp(frontDistance) + rcp(farDistance)));
			const float3 position = rayOrigin + rayDir * ((cameraDistance - distance) / rayDir.z);
			const float2 imagePosition = position.xy / roomHalfSize * (cameraDistance / distance);
			if (IsInsideInteriorSurface(distance, GetInteriorSurfaceDistance(atlas, atlasSampler, roomCell, atlasScale, cellDim, imagePosition, level, cameraDistance, nearDistance, depthRange, depthScale)))
				farDistance = distance;
			else
				frontDistance = distance;
		}

		const float3 position = rayOrigin + rayDir * ((cameraDistance - farDistance) / rayDir.z);
		distanceScale = cameraDistance / farDistance;
		return clamp(position.xy / roomHalfSize * distanceScale, -1.0, 1.0);
	}

	/**
	 * Interior mapping from an atlas of one-point perspective room images. Images with depth in alpha are 120 degree
	 * views marched for parallax; images without (opaque alpha) are 90 degree views from one window half-width in front
	 * of the window, mapped onto a box room by their walls.
	 * @param params Atlas grid packed as (rows - 1) * 16 + (columns - 1) in x, back wall width as a fraction of the
	 * image in y, room depth relative to the depth the images were rendered with in z
	 * @param roomSize Width and height of the room's front wall in game units
	 * @param roomAverage Average color of the selected room image
	 */
	float3 SampleInteriorMapping(Texture2D<float4> atlas, SamplerState atlasSampler, InteriorWindow window, float4 params, float roomSize, out float3 roomAverage)
	{
		const uint atlasGrid = uint(max(params.x, 0.0) + 0.5);
		const uint2 atlasSize = uint2(atlasGrid & 15, (atlasGrid >> 4) & 15) + 1;
		const float backWallScale = clamp(params.y, 0.05, 0.95);
		const float depthScale = clamp(params.z, 0.1, 4.0);
		// Back wall depth of the rendered images, in window half-widths from the window
		const float imageDepth = (1.0 - backWallScale) / backWallScale;

		const uint roomIndex = window.hash % (atlasSize.x * atlasSize.y);
		const float2 roomCell = float2(roomIndex % atlasSize.x, roomIndex / atlasSize.x);
		const float2 atlasScale = rcp(float2(atlasSize));

		float2 atlasDim;
		float atlasMips;
		atlas.GetDimensions(0, atlasDim.x, atlasDim.y, atlasMips);
		const float2 cellDim = atlasDim * atlasScale;
		const float4 average = atlas.SampleLevel(atlasSampler, (roomCell + 0.5) * atlasScale, min(log2(max(cellDim.x, cellDim.y)), atlasMips - 1.0));
		roomAverage = average.xyz;

		float2 windowScale;
		float2 roomUV;
		float gradientScale = 1.0;
		[branch] if (average.w < InteriorDepthAlphaScale + 0.01)
		{
			roomUV = TraceInteriorDepth(atlas, atlasSampler, roomCell, atlasScale, cellDim, window, roomSize, depthScale, windowScale, gradientScale) * 0.5 + 0.5;
		}
		else
		{
			const float3 hit = TraceInteriorRoom(window, roomSize, imageDepth, depthScale, windowScale);
			// Depth is remapped to the images' own depth so their walls stay aligned at any room depth
			roomUV = hit.xy / (1.0 - hit.z * imageDepth) * 0.5 + 0.5;
		}

		// Inset by half a texel so bilinear filtering does not bleed into neighbouring rooms
		const float2 inset = 0.5 / cellDim;
		roomUV = clamp(roomUV, inset, 1.0 - inset);

		const float2 gradient = windowScale * gradientScale * atlasScale;
		return atlas.SampleGrad(atlasSampler, (roomCell + roomUV) * atlasScale, window.uvDdx * gradient, window.uvDdy * gradient).xyz;
	}

	/**
	 * Room cubemap depth in alpha is the distance from the capture point in game units, as
	 * log(distance / InteriorCubeMinDistance) / log(InteriorCubeDistanceRange) * InteriorDepthAlphaScale.
	 */
	static const float InteriorCubeMinDistance = 16.0;
	static const float InteriorCubeDistanceRange = 512.0;
	/** The capture point stands this far inside the wall: a first-person eye stays about this far from a wall the player stands against. */
	static const float InteriorCubeCaptureInset = 32.0;

	/** Distance from the capture point to the surface a room cubemap with depth shows in a room direction, in game units. */
	float GetInteriorCubeDistance(TextureCube<float4> cubemap, SamplerState cubemapSampler, float3 direction, float level)
	{
		const float alpha = cubemap.SampleLevel(cubemapSampler, direction, level).a;
		return InteriorCubeMinDistance * exp2(alpha / InteriorDepthAlphaScale * log2(InteriorCubeDistanceRange));
	}

	/**
	 * Projects the view ray onto the room a cubemap with depth was captured in, as a box measured from the capture point
	 * to its walls, floor and ceiling. The capture is taken standing against a wall, so the box's front is the window wall,
	 * centred on it with the floor on its lower edge, and widened to the wall where the wall is wider than the room. Rays
	 * end on the box, so furniture lies flat on the walls but views stay stable at any angle.
	 * @param level Mip to measure the room from, a few texels across so small details do not move the walls
	 * @return Room direction (x right, y up, z towards the back wall) from the capture point to where the ray meets the box
	 */
	float3 ProjectInteriorCube(TextureCube<float4> cubemap, SamplerState cubemapSampler, InteriorWindow window, float roomSize, float level)
	{
		const float right = GetInteriorCubeDistance(cubemap, cubemapSampler, float3(1, 0, 0), level);
		const float left = GetInteriorCubeDistance(cubemap, cubemapSampler, float3(-1, 0, 0), level);
		const float up = GetInteriorCubeDistance(cubemap, cubemapSampler, float3(0, 1, 0), level);
		const float down = GetInteriorCubeDistance(cubemap, cubemapSampler, float3(0, -1, 0), level);
		const float back = GetInteriorCubeDistance(cubemap, cubemapSampler, float3(0, 0, 1), level);

		const float2 roomHalfSize = window.depth > 0.0 ? window.halfSize : max(0.5 * roomSize, window.halfSize);
		const float2 halfSize = any(window.halfSize <= 0.0) ? roomHalfSize : window.halfSize;
		// In the window frame y points down, and z out of the wall
		const float3 capturePoint = float3(0.5 * (left - right), roomHalfSize.y - down, -InteriorCubeCaptureInset);
		const float3 boxMin = float3(min(capturePoint.x - left, -roomHalfSize.x), min(capturePoint.y - up, -roomHalfSize.y), -InteriorCubeCaptureInset - back);
		const float3 boxMax = float3(max(capturePoint.x + right, roomHalfSize.x), roomHalfSize.y, 0.0);

		const float3 rayOrigin = float3((window.uv * 2.0 - 1.0) * halfSize, 0.0);
		float3 rayDir = -window.viewDirection;
		rayDir.z = min(rayDir.z, -1e-3);
		const float3 exitT = ((rayDir > 0.0 ? boxMax : boxMin) - rayOrigin) / rayDir;
		const float t = min(min(exitT.x, exitT.y), exitT.z);

		const float3 offset = rayOrigin + rayDir * t - capturePoint;
		return float3(offset.x, -offset.y, -offset.z);
	}

	/**
	 * A room captured inside an interior, placed in the exterior world through the door between them, with its cube in
	 * the room array. Matches InteriorCaptures::Capture.
	 */
	struct InteriorCapture
	{
		float4 position;  // Capture point in the world, and its cube in the room array, negative when unused
		float4 right;     // Room right in the world, and the distance to its left wall
		float4 forward;   // Room forward in the world, towards its back wall, and the distance to its right wall
		float4 extent;    // Distances to the floor, the ceiling, the wall behind the capture point and the back wall
	};

	/** Windows farther than this from every captured room, in game units, keep their material's room. */
	static const float InteriorCaptureRange = 1024.0;

	/** A world direction in a capture's room space: x right, y up, z towards the back wall. */
	float3 ToCaptureRoom(InteriorCapture capture, float3 direction)
	{
		return float3(dot(direction, capture.right.xyz), direction.z, dot(direction, capture.forward.xyz));
	}

	/**
	 * The captured room nearest a window's wall, measured to the room's walls so a window inside a room picks it.
	 * @param centre World position of the window's wall centre
	 * @return False when no captured room is within InteriorCaptureRange
	 */
	bool FindInteriorCapture(StructuredBuffer<InteriorCapture> captures, float3 centre, out InteriorCapture best)
	{
		uint count, stride;
		captures.GetDimensions(count, stride);

		best = (InteriorCapture)0;
		float bestScore = InteriorCaptureRange;
		[loop] for (uint index = 0; index < count; index++)
		{
			const InteriorCapture capture = captures[index];
			if (capture.position.w < 0.0)
				continue;
			const float3 position = ToCaptureRoom(capture, centre - capture.position.xyz);
			const float3 boxMin = float3(-capture.right.w, -capture.extent.x, -capture.extent.z);
			const float3 boxMax = float3(capture.forward.w, capture.extent.y, capture.extent.w);
			// Rooms the wall is inside tie at zero, and the nearest capture point wins them
			const float score = length(max(max(boxMin - position, position - boxMax), 0.0)) + 1e-3 * length(position);
			if (score < bestScore) {
				bestScore = score;
				best = capture;
			}
		}
		return bestScore < InteriorCaptureRange;
	}

	/**
	 * Distance from a room image's camera to the captured surface in a room direction, as GetInteriorSurfaceDistance has
	 * it: relative to the capture's nearest walls, which stand for an image's nearest surfaces, and pressed smoothly so
	 * nothing lies more than depthRange behind the window.
	 */
	float GetInteriorCaptureSurfaceDistance(TextureCubeArray<float4> rooms, SamplerState roomSampler, float3 direction, float cube, float level, float captureNear, float cameraDistance, float nearDistance, float depthRange)
	{
		const float alpha = rooms.SampleLevel(roomSampler, float4(direction, cube), level).a;
		const float captured = InteriorCubeMinDistance * exp2(alpha / InteriorDepthAlphaScale * log2(InteriorCubeDistanceRange));
		const float distance = nearDistance * max(captured / captureNear, 1.0);
		return cameraDistance + depthRange * (1.0 - exp(-(distance - cameraDistance) / depthRange));
	}

	/**
	 * Looks through a window into a captured room the way TraceInteriorDepth looks into a room image: from a camera in
	 * front of the window, where viewers stand, with the room's nearest walls a little behind the glass and its depth
	 * pressed into the room depth setting, so furniture keeps its depth while views stay close to what the capture saw.
	 * The room faces the window whichever wall it was captured from.
	 * @param roomSize Width and height of rooms not fitted to the building, in game units
	 */
	float3 SampleInteriorCapture(TextureCubeArray<float4> rooms, SamplerState roomSampler, InteriorCapture capture, InteriorWindow window, float roomSize, out float3 roomAverage)
	{
		// As TraceInteriorDepth
		const uint steps = 64;
		const uint refinements = 4;

		float roomsSize, roomsHeight, roomsElements, roomsMips;
		rooms.GetDimensions(0, roomsSize, roomsHeight, roomsElements, roomsMips);
		const float cube = capture.position.w;
		// Depth is read from a mip about 256 texels across, so small details do not catch the view
		const float depthLevel = max(log2(roomsSize / 256.0), 0.0);

		const float2 roomHalfSize = window.depth > 0.0 ? window.halfSize : max(0.5 * roomSize, window.halfSize);
		const float2 halfSize = any(window.halfSize <= 0.0) ? roomHalfSize : window.halfSize;
		const float meanHalfSize = sqrt(roomHalfSize.x * roomHalfSize.y);
		const float cameraDistance = meanHalfSize / InteriorDepthImageTanHalfFov;
		const float nearDistance = cameraDistance + InteriorDepthNearOffset;
		const float depthRange = max(SharedData::truePBRSettings.InteriorDepthRange, 0.05) * 2.0 * meanHalfSize;
		const float maxDistance = cameraDistance + depthRange;
		const float captureNear = max(min(min(capture.right.w, capture.forward.w), min(capture.extent.x, capture.extent.y)), InteriorCubeMinDistance);

		const float3 rayOrigin = float3((window.uv * 2.0 - 1.0) * halfSize, 0.0);
		float3 rayDir = -window.viewDirection;
		rayDir.z = min(rayDir.z, -1e-3);

		// Steps are even in inverse distance from the camera; the room lies along -z from it, y up against the frame's y
		float frontDistance = cameraDistance;
		float farDistance = maxDistance;
		bool hit = false;
		[loop] for (uint step = 1; step <= steps && !hit; step++)
		{
			const float distance = rcp(lerp(rcp(cameraDistance), rcp(maxDistance), step / (float)steps));
			const float3 offset = rayOrigin + rayDir * ((cameraDistance - distance) / rayDir.z) - float3(0.0, 0.0, cameraDistance);
			const float3 direction = float3(offset.x, -offset.y, -offset.z);
			if (IsInsideInteriorSurface(length(offset), GetInteriorCaptureSurfaceDistance(rooms, roomSampler, direction, cube, depthLevel, captureNear, cameraDistance, nearDistance, depthRange))) {
				farDistance = distance;
				hit = true;
			} else {
				frontDistance = distance;
			}
		}

		[unroll] for (uint refinement = 0; refinement < refinements; refinement++)
		{
			const float distance = rcp(0.5 * (rcp(frontDistance) + rcp(farDistance)));
			const float3 offset = rayOrigin + rayDir * ((cameraDistance - distance) / rayDir.z) - float3(0.0, 0.0, cameraDistance);
			const float3 direction = float3(offset.x, -offset.y, -offset.z);
			if (IsInsideInteriorSurface(length(offset), GetInteriorCaptureSurfaceDistance(rooms, roomSampler, direction, cube, depthLevel, captureNear, cameraDistance, nearDistance, depthRange)))
				farDistance = distance;
			else
				frontDistance = distance;
		}

		const float3 offset = rayOrigin + rayDir * ((cameraDistance - farDistance) / rayDir.z) - float3(0.0, 0.0, cameraDistance);
		const float3 direction = float3(offset.x, -offset.y, -offset.z);
		// Color detail follows how fast the direction changes across the screen, limited where it jumps at edges
		const float3 unit = normalize(direction);
		const float footprint = max(length(ddx(unit)), length(ddy(unit))) * roomsSize * 0.5;
		const float3 color = rooms.SampleLevel(roomSampler, float4(direction, cube), clamp(log2(max(footprint, 1e-4)), 0.0, 4.0)).xyz;

		// Two opposite corners of the 1x1 mip each average three faces
		const float lastMip = roomsMips - 1.0;
		roomAverage = 0.5 * (rooms.SampleLevel(roomSampler, float4(1.0, 1.0, 1.0, cube), lastMip).xyz + rooms.SampleLevel(roomSampler, float4(-1.0, -1.0, -1.0, cube), lastMip).xyz);
		return color;
	}

	/**
	 * Cubemap interior mapping. Room cubemaps with depth in alpha, as the room export writes, rebuild the room they were
	 * captured in behind the wall as a box measured from that depth; cubemaps without (opaque alpha) are the classic form, rendered from the centre of a
	 * cube room. Either way +X is right, +Y up and +Z towards the back wall.
	 * @param params Room depth relative to a cube room in z, for cubemaps without depth
	 * @param roomSize Width and height of the room's front wall in game units
	 * @param roomAverage Average color of the cubemap
	 */
	float3 SampleInteriorCubemap(TextureCube<float4> cubemap, SamplerState cubemapSampler, InteriorWindow window, float4 params, float roomSize, out float3 roomAverage)
	{
		float2 cubemapSize;
		float cubemapMips;
		cubemap.GetDimensions(0, cubemapSize.x, cubemapSize.y, cubemapMips);
		// Two opposite corners of the 1x1 mip each average three faces
		const float lastMip = cubemapMips - 1.0;
		const float4 averageA = cubemap.SampleLevel(cubemapSampler, float3(1, 1, 1), lastMip);
		const float4 averageB = cubemap.SampleLevel(cubemapSampler, float3(-1, -1, -1), lastMip);
		roomAverage = 0.5 * (averageA.xyz + averageB.xyz);

		float3 color = 0.0;
		[branch] if (0.5 * (averageA.w + averageB.w) < InteriorDepthAlphaScale + 0.01)
		{
			const float3 direction = ProjectInteriorCube(cubemap, cubemapSampler, window, roomSize, max(log2(cubemapSize.x / 32.0), 0.0));
			// Color detail follows how fast the direction changes across the screen, limited where it jumps at edges
			const float3 unit = normalize(direction);
			const float footprint = max(length(ddx(unit)), length(ddy(unit))) * cubemapSize.x * 0.5;
			color = cubemap.SampleLevel(cubemapSampler, direction, clamp(log2(max(footprint, 1e-4)), 0.0, 4.0)).xyz;
		}
		else
		{
			const float depthScale = clamp(params.z, 0.1, 4.0);
			float2 windowScale;
			const float3 hit = TraceInteriorRoom(window, roomSize, 2.0, depthScale, windowScale);

			// Tangent v points down the texture, so it is flipped to the cubemap's up
			float3 direction = float3(hit.x, -hit.y, -2.0 * hit.z - 1.0);
			// Mirror alternate windows so one cubemap repeats less visibly
			if (window.hash & 1)
				direction.x = -direction.x;

			// Gradients from the window plane avoid the derivative jumps at room corners
			color = cubemap.SampleGrad(cubemapSampler, direction, float3(window.uvDdx * windowScale * 2.0, 0.0), float3(window.uvDdy * windowScale * 2.0, 0.0)).xyz;
		}
		return color;
	}
}

#endif  // __PBR_DEPENDENCY_HLSL__
