#ifndef __PGRASS_FAR_LIGHTING_HLSLI__
#define __PGRASS_FAR_LIGHTING_HLSLI__

struct FarGrassLighting
{
	float3 diffuse;
	float3 ambient;
	float3 specular;
	float3 diffuseAlbedo;
};

struct FarGrassSurface
{
	float3 reflectionAlbedo;
	float3 transmissionAlbedo;
	float3 bounceColor;
	float ambientDesaturation;
	float fuzzAlbedo;
	float3 fuzzColor;
};

/** @brief Approximates unresolved reflection layers with broad lobes, retaining thin-surface scattering. */
FarGrassLighting GetFarGrassLighting(FarGrassSurface surface, MaterialProperties material,
	float3 directNormal, float3 indirectNormal,
	float3 viewDirection, float3 lightDirection, float3 lightColor, float surfaceShadow, float detailShadow,
	float canopyAO, float canopyOverhead, float canopyHeight)
{
	FarGrassLighting lighting;
	float normalDotView = clamp(dot(directNormal, viewDirection), EPSILON_DOT_CLAMP, 1.0f);
	// Use the fully rough limit for the reflection energy of unresolved blades.
	float3 specularAlbedo = saturate(PBR::SpecularDirectionalAlbedo(material.F0, 1.0f, normalDotView));
	float fuzzAlbedo = surface.fuzzAlbedo;
	float3 surfaceThroughput = (1.0f - specularAlbedo) * (1.0f - fuzzAlbedo);
	float3 layerAlbedo = specularAlbedo * (1.0f - fuzzAlbedo) + fuzzAlbedo * surface.fuzzColor;

	float normalDotLight = dot(directNormal, lightDirection);
	float2 diffuseCosines = max(float2(normalDotLight, -normalDotLight), 0.0f);
	float3 sunIrradiance = lightColor * surfaceShadow * detailShadow * BRDF::Diffuse_Lambert();
	lighting.diffuse = (surface.reflectionAlbedo * diffuseCosines.x + surface.transmissionAlbedo * diffuseCosines.y) *
	                   sunIrradiance * surfaceThroughput;

	// Redistribute the surface layers' reflected energy over the visible hemisphere. The fuzz covers both faces, so light
	// from behind reaches the viewer-side fuzz through the blade, tinted by its transmission.
	float lightPath = canopyOverhead / max(lightDirection.z, CanopyReflectionMinElevation);
	lighting.specular = (layerAlbedo * diffuseCosines.x + fuzzAlbedo * surface.fuzzColor * surface.transmissionAlbedo * diffuseCosines.y) * sunIrradiance *
	                    exp2(-lightPath * CanopyReflectionExtinction);

	float3 singleScatterAlbedo = saturate(surface.reflectionAlbedo + surface.transmissionAlbedo);
	float3 canopyScatterAlbedo = CanopyScatterEscape * singleScatterAlbedo /
	                             (1.0f - (1.0f - CanopyScatterEscape) * singleScatterAlbedo);
	float blockedLight = saturate(1.0f - detailShadow) * surfaceShadow * saturate(lightDirection.z);
	lighting.diffuse += lightColor * BRDF::Diffuse_Lambert() * blockedLight * canopyScatterAlbedo *
	                    (surface.reflectionAlbedo + surface.transmissionAlbedo) * surfaceThroughput;

	float3 frontIrradiance = DistantAmbientLUT.SampleLevel(LinearSampler, GBuffer::EncodeNormal(indirectNormal), 0).rgb;
	float3 backIrradiance = DistantAmbientLUT.SampleLevel(LinearSampler, GBuffer::EncodeNormal(-indirectNormal), 0).rgb;
	float3 frontSkyLight = canopyAO * GetCanopySkyLight(indirectNormal, canopyOverhead, canopyScatterAlbedo);
	float3 backSkyLight = canopyAO * GetCanopySkyLight(-indirectNormal, canopyOverhead, canopyScatterAlbedo);
	float3 frontAmbient = lerp(frontIrradiance, dot(frontIrradiance, float3(0.2126f, 0.7152f, 0.0722f)),
							  surface.ambientDesaturation) *
	                      frontSkyLight;
	float3 backAmbient = lerp(backIrradiance, dot(backIrradiance, float3(0.2126f, 0.7152f, 0.0722f)),
							 surface.ambientDesaturation) *
	                     backSkyLight;
	float3 scatteringThroughput = MultiBounceAO(material.BaseColor, material.AO) * surfaceThroughput;
	lighting.diffuseAlbedo = surface.reflectionAlbedo * scatteringThroughput;
	lighting.ambient = frontAmbient * lighting.diffuseAlbedo + backAmbient * surface.transmissionAlbedo * scatteringThroughput;
	lighting.ambient += frontAmbient * surface.bounceColor * (1.0f - canopyHeight) * surface.reflectionAlbedo * surfaceThroughput;

	// Ambient fuzz reflects in its colour; the broad reflection reuses the same filtered sky hemisphere.
	lighting.ambient += frontIrradiance * frontSkyLight * material.AO * fuzzAlbedo * surface.fuzzColor;
	lighting.diffuse += lighting.ambient;
	lighting.specular += Color::IrradianceToLinear(frontIrradiance * frontSkyLight * Color::ReflectionNormalisationScale) *
	                     material.AO * specularAlbedo * (1.0f - fuzzAlbedo);

	lighting.diffuse *= grassFrameLight.w;
	lighting.ambient *= grassFrameLight.w;
	lighting.specular *= grassFrameLight.w;
	lighting.diffuseAlbedo *= grassPBRLightingScale;
	return lighting;
}

/** @brief Resolves Far lighting in linear space with the deferred pass's ambient and direct AO weights. */
float3 ResolveFarGrassLighting(FarGrassLighting lighting, float screenAO)
{
	float3 diffuse = lighting.diffuse;
	[branch] if (screenAO < 1.0f)
	{
		float3 albedo = Color::IrradianceToLinear(lighting.diffuseAlbedo / Color::PBRLightingScale);
		// Move the AO weights into irradiance space to avoid converting each light component twice.
		float3 ambientAO = Color::IrradianceToGamma(MultiBounceAO(albedo, screenAO));
		float3 ambient = min(lighting.ambient, lighting.diffuse);
		diffuse = (lighting.diffuse - ambient) * sqrt(ambientAO) + ambient * ambientAO;
	}
	// Canopy and contact occlusion are already included above; terrain darkness belongs to the ground.
	return Color::IrradianceToLinear(diffuse) + lighting.specular;
}

#endif
