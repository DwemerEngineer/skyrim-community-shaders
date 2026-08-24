cbuffer GrassGlobals : register(b8)
{
    float voronoiGridSize;
    float inverseVoronoiGridSize;
    float cameraViewRow0Sum;
    float cameraViewRow1Sum;
    float2 dynamicResolutionInverted;

    float windSpeed;
    float windTimer;
    float2 windDir;
    float windAngle;

    float occlusionHalfExtent;
    float occlusionInvExtent;
    float3 _occlusionPadding;
    float4 occlusionParams; // xy: window centre in world space, z: underside clearance, w: top-height bias (world units)

    float4 grassAOParams; // x: density map dim, y: darken strength, z: blades-per-texel for full dark, w: canopy height (world units)
    float4 grassLightParams; // x: density AO, y: canopy sky occlusion, z: sun self-shadow, w: base canopy shading

    float4 farParams; // x: thin start dist (world), y: 1/(end-start), z: min keep fraction at far edge
    float4 miscParams; //  x: grass map edge noise in world units, y: slope facing, z: view thicken, w: timer delta
    float4 grassTerrainBlend; // x: blend strength, y: blend height (world units), z: normal blend, w: roughness blend

    float2 heightMapScale; // world space -> terrain heightmap UV, pairs with heightMapOffset
    float2 heightMapOffset; // -pos0.xy * heightMapScale
    float2 heightMapZRange; // {pos0.z, pos1.z}; texels are normalised and lerp between these

    float2 debugFlags; // x: bypass every cull in the generator
    float4 grassPresenceParams; // xy: world min-corner of the grass-id texture, z: 1/sample spacing, w: texture dim (density gather)
}

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
    float4 grassSurfParams; // x: micro-detail, y: ambient normal flatten, z: wrap amount, w: anisotropic specular
    float4 baseMinTipRoughnessStart; // roughness at the base, at the smoothest point, and at the tip and t at which roughness bottoms out and starts climbing to the tip
    float4 midRoughnessPolynomial; // x: cubic, y: quadratic, z: base; matches the authored curve at Mid's t={0,.5,1}
    float4 grassTypeLightParams; // x: ground bounce, y: sky translucency, z: specular occlusion, w: ambient desaturation

    float4 baseColor;
    float4 tipColor;
    float4 grassColorTipDry;
    float4 grassColorVar; // x: hue variation, y: brightness variation, z: tip-dry strength, w: mottle strength
    float4 grassColorCool;
    float4 grassColorWarm;
    float4 grassBounceColor;
    float4 grassTextureParams; // x: blotch strength, y: blotch scale, z: speckle strength, w: speckle scale
    float4 grassVeinParams;    // rgb: vein albedo tint, w: vein albedo strength
    float4 grassVeinParams2;   // x: vein normal strength, y: ripple depth, z: micro-wiggle amount
    float4 grassSubsurfaceColor; // rgb: subsurface/translucency tint
};

#define GRASS_TYPE_COUNT 128

cbuffer GrassTypes : register(b9)
{
    GrassType grassType[GRASS_TYPE_COUNT];
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
    uint posXY;
    uint posZWidthHeight;
    uint facingTilt;     // 4x UNORM8 mapped to [-1,1]: facing.xy, tilt sin/cos
    uint seedAndType;    // high 8: far width factor; next 16: direct colour seed; low 8: grass type
};
#else
struct Blade
{
	uint posXY;
	uint posZWidthHeight;
	uint facingAndWind;        // low 16: static facing as 2x SNORM8; high 16: current wind displacement as f16
	uint previousWind;         // low 16: previous wind displacement as f16
	uint hashClumpAndGrassType;
	uint clumpDensity;   // low 16: clump density (f16); high 16: randBend (f16), the blade's precomputed bend amount
	uint tipDir;         // (sin, cos) of the blade tilt as 2x f16; precomputed in the generator so the VS skips the pcg hash + sincos
#if defined(HIGH_LOD)
	uint skylightingSH0;  // x/y as f16
	uint skylightingSH1;  // z/w as f16
#endif
};
#endif
