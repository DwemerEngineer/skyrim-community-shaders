#ifndef FOLIAGE_LIGHTING_HLSLI
#define FOLIAGE_LIGHTING_HLSLI

#include "Common/Math.hlsli"
#include "Common/PBRMath.hlsli"

#if defined(TRUE_PBR)
#	include "Common/LightingCommon.hlsli"
#	include "Common/Shading.hlsli"

namespace PBR
{
	struct FoliageSurface
	{
		float3 reflectionAlbedo;
		float3 frontTransmissionAlbedo;
		float3 backTransmissionAlbedo;
		float3 specularAlbedo;
		float3 fuzzColor;
		float fuzzAlbedo;
		float fuzzRoughness;
	};

	FoliageSurface CreateFoliageSurface(MaterialProperties material, float NdotV, bool doSpecular, bool reflectionIncludesBaseColor = true)
	{
		FoliageSurface surface = (FoliageSurface)0;
		surface.reflectionAlbedo = reflectionIncludesBaseColor ? saturate(material.BaseColor) : 1.0f;
		[branch] if ((PBRFlags & Flags::Subsurface) != 0)
		{
			float reflectionWeight;
			GetFoliageSubsurfaceAlbedos(saturate(material.BaseColor), saturate(material.SubsurfaceColor), material.Thickness, material.SubsurfaceBalance,
				reflectionWeight, surface.frontTransmissionAlbedo, surface.backTransmissionAlbedo);
			surface.reflectionAlbedo *= reflectionWeight;
		}

		[branch] if (doSpecular)
		{
			surface.specularAlbedo = saturate(material.FoliageSpecularWeight) * saturate(SpecularDirectionalAlbedo(material.F0, material.Roughness, NdotV));
			[branch] if ((PBRFlags & Flags::Fuzz) != 0)
			{
				surface.fuzzRoughness = clamp(material.Roughness, 0.01f, 1.0f);
				surface.fuzzAlbedo = saturate(saturate(material.FuzzWeight) * FuzzDirectionalAlbedo(NdotV, surface.fuzzRoughness));
				surface.fuzzColor = saturate(material.FuzzColor);
			}
		}
		return surface;
	}

	void GetDirectLightInputFoliage(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material,
		FoliageSurface surface, bool doSpecular, float wrap, float glintH = 0.0f)
	{
		lightingOutput = (DirectLightingOutput)0;
		float3 N = context.worldNormal;
		float3 V = context.viewDir;
		float3 L = context.lightDir;
		float NdotL = dot(N, L);
		float3 specularNormal = GetFoliageNormal(N, V);
		float NdotV = clamp(dot(specularNormal, V), EPSILON_DOT_CLAMP, 1.0f);
		float specularNdotL = saturate(dot(specularNormal, L));
		float fuzzThroughput = 1.0f - surface.fuzzAlbedo;
		float3 scatteringThroughput = (1.0f - surface.specularAlbedo) * fuzzThroughput;
		float3 irradiance = context.lightColor * BRDF::Diffuse_Lambert() * scatteringThroughput;
		float3 shineThroughAlbedo = (PBRFlags & Flags::Subsurface) != 0 ? material.BaseColor : 0.0f;
		GetFoliageDirectScattering(surface.reflectionAlbedo, surface.frontTransmissionAlbedo, surface.backTransmissionAlbedo, shineThroughAlbedo,
			material.Thickness, irradiance, NdotL, wrap, context.detailedShadow, float2(context.softShadow, context.softShadow * context.backTransmissionVisibility),
			lightingOutput.diffuse, lightingOutput.transmission);

		[branch] if (doSpecular)
		{
			float3 H = context.halfVector;
			float NdotH = saturate(dot(specularNormal, H));
			float VdotH = saturate(dot(V, H));
			float D = BRDF::D_GGX(material.Roughness, NdotH);
#	if defined(GLINT)
			[branch] if (material.GlintLogMicrofacetDensity > 1.1f)
			{
				float Dmax = BRDF::D_GGX(material.Roughness, 1.0f);
				D = Glints::SampleGlints2023NDF(material.Noise, material.GlintLogMicrofacetDensity, material.GlintMicrofacetRoughness,
					material.GlintDensityRandomization, material.GlintCache, glintH, D, Dmax)
				        .x;
			}
#	endif
			float visibility = BRDF::Vis_SmithJoint(material.Roughness, NdotV, max(specularNdotL, EPSILON_DOT_CLAMP));
			float3 F = BRDF::F_Schlick(material.F0, VdotH);
			float3 detailedLightColor = context.lightColor * context.detailedShadow;
			lightingOutput.specular = saturate(material.FoliageSpecularWeight) * D * visibility * F * detailedLightColor * specularNdotL * fuzzThroughput;
			[branch] if (surface.fuzzAlbedo > 0.0f)
			{
				float3 fuzzReflection = FuzzLobe(L, V, specularNormal, NdotV, surface.fuzzRoughness) * detailedLightColor;
				float3 fuzzTransmission = surface.backTransmissionAlbedo * FuzzLobeTransmitted(L, V, specularNormal, NdotV, surface.fuzzRoughness) *
				                          context.lightColor * context.softShadow * context.backTransmissionVisibility;
				lightingOutput.specular += surface.fuzzAlbedo * surface.fuzzColor * (fuzzReflection + fuzzTransmission);
			}
		}
	}

	void GetIndirectLobeWeightsFoliage(out IndirectLobeWeights lobeWeights, IndirectContext context, MaterialProperties material, FoliageSurface surface)
	{
		lobeWeights = (IndirectLobeWeights)0;
		float3 diffuseAO = MultiBounceAO(material.BaseColor, material.AO);
		if (IsFoliageMaterial(PBRFlags))
			diffuseAO = saturate(diffuseAO);
		float3 scatteringThroughput = diffuseAO * (1.0f - surface.specularAlbedo) * (1.0f - surface.fuzzAlbedo);
		lobeWeights.diffuse = surface.reflectionAlbedo * scatteringThroughput + surface.fuzzAlbedo * surface.fuzzColor * material.AO;
		lobeWeights.frontTransmission = surface.frontTransmissionAlbedo * scatteringThroughput;
		lobeWeights.backTransmission = surface.backTransmissionAlbedo * scatteringThroughput;
		float NdotV = clamp(abs(dot(context.worldNormal, context.viewDir)), EPSILON_DOT_CLAMP, 1.0f);
		lobeWeights.specular = surface.specularAlbedo * (1.0f - surface.fuzzAlbedo) *
		                       SpecularOcclusion(NdotV, material.Roughness * material.Roughness, material.AO);
	}
}
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif
#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

float3 GetFoliageTransmissionLighting(float3 ambientColor, float3 transmissionWeight, float3 transmissionNormal,
	float3 worldPosition, float vertexAO, float opacity, out float3 transmittedAmbientColor
#if defined(IBL) && defined(LIGHTING)
	,
	SamplerState iblSampler, bool useStaticIBL
#endif
#if defined(SKYLIGHTING)
	,
	sh2 skylightingSH
#endif
)
{
#if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
#	if defined(LIGHTING)
		if (useStaticIBL)
			ambientColor = ImageBasedLighting::GetStaticDiffuseIBL(transmissionNormal, iblSampler);
		else
#	endif
			ambientColor = ImageBasedLighting::GetDiffuseIBL(ambientColor, -transmissionNormal);
	}
#endif
	transmittedAmbientColor = transmissionWeight * ambientColor;
	float3 diffuseColor = transmittedAmbientColor;
#if defined(SKYLIGHTING)
	// The ordinary diffuse helper biases downward normals upward. Subsurface needs the requested hemisphere.
	float skylightingDiffuse = SharedData::InInterior ? 1.0f :
	                                                    saturate(Skylighting::EvaluateDiffuse(skylightingSH, transmissionNormal, Skylighting::GetFadeOutFactor(worldPosition)) /
																 max(vertexAO, EPSILON_DIVISION));
	skylightingDiffuse = PBR::GetFoliageShadowVisibility(skylightingDiffuse, opacity);
	Skylighting::ApplySkylighting(diffuseColor, transmittedAmbientColor, transmissionWeight, skylightingDiffuse);
#endif
	return diffuseColor;
}

#endif
