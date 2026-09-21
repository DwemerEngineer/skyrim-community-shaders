cbuffer GrassGlobals : register(b8)
{
	float voronoiGridSize;
	float inverseVoronoiGridSize;
	float cameraViewRow0Sum;
	float cameraViewRow1Sum;
	float2 dynamicResolutionInverted;

	float windSpeed;
	float previousWindSpeed;
	float2 windDir;
	float windAngle;

	float occlusionHalfExtent;
	float occlusionInvExtent;
	float2 previousWindDir;
	float grassPBRLightingScale;
	float4 occlusionParams;  // xy: window centre in world space, z: underside clearance, w: top-height bias (world units)

	float4 grassAOParams;     // x: density map dim, y: darken strength, z: blades-per-texel for full dark, w: canopy height (world units)
	float4 grassLightParams;  // x: density AO, y: canopy sky occlusion, z: resolved sun-shadow exponent, w: base canopy shading
	float4 grassFrameLight;   // xyz: resolved TRUE_PBR directional light, w: resolved grass brightness scale

	float4 farParams;          // x: thin start, y: inverse range, z: Far candidate spacing, w: Far performance keep
	float4 miscParams;         //  x: grass map edge noise in world units, y: slope facing, z: view thicken, w: timer delta
	float4 grassTerrainBlend;  // x: blend strength, y: blend height (world units), z: normal blend, w: roughness blend

	float2 heightMapScale;   // world space -> terrain heightmap UV, pairs with heightMapOffset
	float2 heightMapOffset;  // -pos0.xy * heightMapScale
	float2 heightMapZRange;  // {pos0.z, pos1.z}; texels are normalised and lerp between these

	float2 debugFlags;           // x: bypass every cull in the generator, y: High body depth-clip height (-1 disables clipping)
	float4 grassPresenceParams;  // xy: world min-corner of the grass-id texture, z: 1/sample spacing, w: texture dim (density gather)
	float4 grassHiZParams;       // xy: valid base extent, z: nominal pixels/texel, w: trustworthy mip count; zero disables
	float2 grassLodOrigin;       // camera XY with a small dead zone, preventing stationary camera sway from moving LOD bands
	float2 _grassLodPadding;
}

#if defined(FAR_LOD)
float GetFarPerformanceKeep(float lodDistance, float projectionScale)
{
	float farWidthT = saturate((lodDistance - farParams.x) * farParams.y);
	float distanceKeep = lerp(1.0f, farParams.w, farWidthT);

	// Keep about one Far candidate per projected pixel once the original lattice becomes sub-pixel.
	float renderWidth = rcp(max(dynamicResolutionInverted.x, 1.0e-6f));
	float projectedSpacing = farParams.z * abs(projectionScale) * (0.5f * renderWidth) / max(lodDistance, 1.0f);
	float screenKeep = max(saturate(projectedSpacing * projectedSpacing), 0.4f);
	return min(distanceKeep, lerp(1.0f, screenKeep, farWidthT));
}
#endif

struct GrassType
{
	float height;
	float width;
	float minSlope;
	float maxSlope;
	float stiffness;
	float rotationalStiffness;
	float tipWeight;

	float mid;

	float clumpDistanceFactor;
	float clumpHeightFactor;
	float clumpFacingFactor;
	float clumpAOStrength;
	float clumpColorStrength;

	float spatialFreq;
	float phaseOffset;
	float phaseLag;

	float minAO;
	float specular;

	float2 minMaxSubsurfaceOpacity;
	float4 grassSurfParams;           // x: wax sheen strength, y: ambient normal flatten, z: wrap amount, w: wax roughness multiplier
	float4 baseMinTipRoughnessStart;  // roughness at the base, at the smoothest point, and at the tip and t at which roughness bottoms out and starts climbing to the tip
	float4 midRoughnessPolynomial;    // x: cubic, y: quadratic, z: base; matches the authored curve at Mid's t={0,.5,1}
	float4 grassTypeLightParams;      // x: ground bounce, y: sky translucency, z: specular occlusion, w: ambient desaturation

	float4 baseColor;
	float4 tipColor;
	float4 grassColorTipDry;
	float4 grassColorVar;  // x: hue variation, y: brightness variation, z: tip-dry strength, w: mottle strength
	float4 grassColorCool;
	float4 grassColorWarm;
	float4 grassBounceColor;
	float4 grassTextureParams;    // x: blotch strength, y: blotch scale, z: speckle strength, w: speckle scale
	float4 grassVeinParams;       // rgb: vein albedo tint, w: vein albedo strength
	float4 grassVeinParams2;      // x: vein normal strength, y: ripple depth, z: micro-wiggle amount, w: curved normal strength
	float4 grassSubsurfaceColor;  // rgb: subsurface/translucency tint
};

#define GRASS_TYPE_COUNT 128

cbuffer GrassTypes : register(b9)
{
	GrassType grassType[GRASS_TYPE_COUNT];
}

float ApproximateGrassDistance(float2 offset)
{
	float2 distanceXY = abs(offset);
	return max(distanceXY.x, distanceXY.y) + min(distanceXY.x, distanceXY.y) * 0.375f;
}

#if defined(CSHADER)
struct GrassGeneratorType
{
	float height;
	float width;
	float minSlope;
	float maxSlope;
	float stiffness;
	float rotationalStiffness;
	float tipWeight;
	float _pad0;
	float clumpDistanceFactor;
	float clumpHeightFactor;
	float clumpFacingFactor;
	float _pad1;
};

cbuffer GrassGeneratorTypes : register(b10)
{
	GrassGeneratorType generatorGrassType[GRASS_TYPE_COUNT];
}
#endif

#if defined(FAR_LOD)
struct Blade
{
	uint posXY;          // camera-relative x/y as two f16 values
	uint posZWidthHeight;  // camera-relative z as f16, then UNORM8 width/height
	uint facingTilt;   // 4x UNORM8 mapped to [-1,1]: facing.xy, tilt sin/cos
	uint seedAndType;  // high 8: clump density; next 16: Voronoi-cell appearance seed; low 8: grass type
};
#else
struct Blade
{
	uint posXY;          // camera-relative x/y as two f16 values
	uint posZWidthHeight;  // camera-relative z as f16, then UNORM8 width/height
	uint facingAndWind;  // low 16: current facing as 2x SNORM8; high 16: current wind displacement as f16
	uint previousWind;   // low 16: previous wind displacement or Mid collision Z; high 16: blade colour and bend
	uint hashClumpAndGrassType;
	uint tipDir;
#	if defined(HIGH_LOD)
	uint skylightingSH0;  // x/y as f16
	uint skylightingSH1;  // z/w as f16
#	endif
#	if defined(PGRASS_CACHED_COLLISION)
#	if defined(MID_LOD)
	uint collisionData;  // current x and y as f16, with current z in previousWind
#	else
	uint3 collisionData;  // current.xyz and previous.xyz packed as six f16 values
#	endif
#	endif
};
#endif
