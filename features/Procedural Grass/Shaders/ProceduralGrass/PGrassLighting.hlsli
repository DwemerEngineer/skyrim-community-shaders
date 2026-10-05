#ifndef __PGRASS_LIGHTING_HLSLI__
#define __PGRASS_LIGHTING_HLSLI__

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
 * Low and Far shade before writing depth; use the root's surface unless nearer geometry covers it.
 * @param coverWeight Coverage by geometry in front of the root
 * @param distantForegroundWeight Excludes shadows from distant foreground surfaces
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
 * @brief Estimates sun visibility from the terrain slope beyond the shadow mask's range.
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

#if defined(SCREEN_SPACE_SHADOWS) && (defined(MID_LOD) || defined(LOW_LOD) || (defined(HIGH_LOD) && !defined(HIGH_INNER)))
/**
 * @brief Statistical stand-in for blades shadowing each other in screen space.
 * Stable blade seeds select lit, partial and full shadows; lower sections and low sun receive more occlusion.
 * @param viewDirection Direction from the blade toward the camera
 * @param lightDirection Direction toward the sun
 * @param seed Stable per-blade random seed
 * @param along Position along the blade [0,1]
 * @param expectedBlend Blend toward the pattern's expected visibility, for geometry standing in for several blades
 * @return Sun visibility from neighbouring blades
 */
float GetBladeShadowPattern(float3 viewDirection, float3 lightDirection, uint seed, float along, float expectedBlend)
{
	static const float BladeSunBlocking = 0.2f;
	float sunHeight = max(abs(lightDirection.z), 0.05f);
	// Match the visibility of shadowed blade sections as the view turns toward the sun.
	float sunFacingView = 1.0f - smoothstep(0.0f, 0.9f, dot(viewDirection, lightDirection));
	// Blades block more of a lower sun, up to the reach of the screen-space trace this stands in for.
	static const float BladeSunBlockingLimit = 3.0f;
	float bladeSunVisibility = exp(-BladeSunBlocking * sunFacingView * min(sqrt(saturate(1.0f - sunHeight * sunHeight)) / sunHeight, BladeSunBlockingLimit));
	float shadowLit = bladeSunVisibility * bladeSunVisibility;
	float shadowFull = 0.85f * pow(saturate(1.0f - bladeSunVisibility), 1.5f);
	float shadowPartial = max(1.0f - shadowLit - shadowFull, 1.0e-3f);
	static const float ShadowMaxLine = 0.8f;
	float shadowRandom = GrassNoiseHash(float2(seed, 0.0f));
	float shadowLine = (shadowRandom - shadowLit) * (ShadowMaxLine / shadowPartial);
	shadowLine = shadowRandom >= 1.0f - shadowFull ? 2.0f : shadowLine;
	float bladeShadow = smoothstep(shadowLine - 0.05f, shadowLine + 0.05f, along);
	float expectedShadow = shadowLit + shadowPartial * saturate(along * (1.0f / ShadowMaxLine));
	return lerp(bladeShadow, expectedShadow, expectedBlend);
}
#endif

// Extinction along a vertical canopy path and its minimum elevation.
static const float CanopyReflectionExtinction = 4.0f;
static const float CanopyReflectionMinElevation = 0.05f;
static const float CanopyScatterEscape = 0.5f;  // Probability that scattered light escapes the canopy.

/** @brief Sky visibility along the surface hemisphere's mean elevation. */
float GetCanopySkyVisibility(float3 normal, float canopyOverhead)
{
	static const float MinSkyElevation = 0.1f;
	// Unit surface normals give the halfway elevation directly, including the closed-sky limit when pointing down.
	float skyElevation = max(sqrt(saturate(0.5f * (1.0f + normal.z))), MinSkyElevation);
	return exp2(-canopyOverhead * grassLightParams.y * (1.0f / skyElevation - 1.0f));
}

/** @brief Blends hidden sky irradiance toward canopy scatter. */
float3 GetCanopySkyLight(float3 normal, float canopyOverhead, float3 canopyFill)
{
	return lerp(canopyFill, 1.0f, GetCanopySkyVisibility(normal, canopyOverhead));
}

void GetDiffuseLightInputProcGrass(out DirectLightingOutput lightingOutput, DirectContext context, float normalDotLight,
	float3 reflectionAlbedo, float3 transmissionAlbedo, float3 surfaceThroughput)
{
	lightingOutput = (DirectLightingOutput)0;
	// Use Blender's zero-diffuse-roughness limit: Lambert reflection and transmission on opposite hemispheres.
	float2 diffuseCosines = max(float2(normalDotLight, -normalDotLight), 0.0f);
	float3 irradiance = context.lightColor * context.detailedShadow * BRDF::Diffuse_Lambert() * surfaceThroughput;
	// Combine the scattered lobes so the wetness layer attenuates both together.
	lightingOutput.diffuse = (reflectionAlbedo * diffuseCosines.x + transmissionAlbedo * diffuseCosines.y) * irradiance;
}

void GetDirectLightInputProcGrass(out DirectLightingOutput lightingOutput, DirectContext context, MaterialProperties material, float diffuseNdotL,
	float3 reflectionAlbedo, float3 transmissionAlbedo,
	float3 specularNormal, float3 specularTangent, float specularRoughness, float specularAnisotropy, float specularSlopeVariance, float3 specularAlbedo, float3 fuzzNormal, float fuzzNdotV, float fuzzAlbedo, float fuzzRoughness)
{
	const float3 detailedLightColor = context.lightColor * context.detailedShadow;
	const float3 V = context.viewDir;
	const float3 L = context.lightDir;

	// Evaluate the blade GGX lobe; only light on the visible hemisphere reflects.
	float specularNdotL = saturate(dot(specularNormal, L));
	float3 specularF;
#if defined(FAR_LOD)
	// The canopy has no anisotropy; evaluate the isotropic limit without the tangent frame.
	float3 halfVector = V + L;
	float halfLengthSquared = dot(halfVector, halfVector);
	float specularNdotV = dot(specularNormal, V);
	float3 directSpecular = 0.0f;
	[branch] if (specularNdotL > 0.0f && specularNdotV > 0.0f && halfLengthSquared > 1e-8f)
	{
		float3 H = halfVector * rsqrt(halfLengthSquared);
		float alpha = max(specularRoughness * specularRoughness, 1e-3f);
		float roughness = sqrt(sqrt(alpha * alpha + specularSlopeVariance));
		float distribution = BRDF::D_GGX(roughness, saturate(dot(specularNormal, H)));
		float visibility = BRDF::Vis_SmithJoint(roughness, saturate(specularNdotV) + EPSILON_DOT_CLAMP, specularNdotL);
		specularF = BRDF::F_Schlick(material.F0, saturate(dot(V, H)));
		directSpecular = distribution * visibility * specularF * specularNdotL;
	}
#else
	float3 directSpecular = PBR::SpecularMicrofacetAnisotropic(specularRoughness, specularAnisotropy, material.F0, specularNormal, specularTangent, V, L, specularF, specularSlopeVariance) * specularNdotL;
#endif

	// OpenPBR fuzz sits over the whole blade: it reflects fuzzAlbedo of the light and passes the rest untinted.
	float fuzzThroughput = 1.0f - fuzzAlbedo;
	float fuzzReflection = fuzzAlbedo * PBR::FuzzLobe(L, V, fuzzNormal, fuzzNdotV, fuzzRoughness);

	GetDiffuseLightInputProcGrass(lightingOutput, context, diffuseNdotL, reflectionAlbedo, transmissionAlbedo,
		(1.0f - specularAlbedo) * fuzzThroughput);
	lightingOutput.specular = (directSpecular * fuzzThroughput + fuzzReflection) * detailedLightColor;
}

#endif
