cbuffer GrassGlobals : register(b8)
{
	float2 _padClumpGrid;  // Clump grid sizes are per grass type.
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

	float4 grassAOParams;     // x: density map dim, y: terrain darkness (0 disables), z: full-coverage encoding, w: canopy height (world units)
	float4 grassLightParams;  // x: density AO, y: canopy sky occlusion, z: unused, w: base canopy shading
	float4 grassFrameLight;   // xyz: resolved TRUE_PBR directional light, w: resolved grass brightness scale

	float4 farParams;          // x: thin start, y: inverse range, z: Far candidate spacing, w: Far performance keep
	float4 miscParams;         // x: grass map edge noise in world units, y: object surface map enabled, z: view thicken, w: timer delta
	float4 grassTerrainBlend;  // x: blend strength, y: blend height (world units), z: normal blend, w: roughness blend

	float2 heightMapScale;   // world space -> terrain heightmap UV, pairs with heightMapOffset
	float2 heightMapOffset;  // -pos0.xy * heightMapScale
	float2 heightMapZRange;  // {pos0.z, pos1.z}; texels are normalised and lerp between these

	float2 debugFlags;           // x: bypass generator culling, y: colour blades by tier
	float4 grassPresenceParams;  // xy: world min-corner of the grass-id texture, z: 1/sample spacing, w: texture dim (density gather)
	float4 grassHiZParams;       // xy: valid base extent, z: near-tier geometry radius, w: trustworthy mip count; zero disables
	float2 grassLodOrigin;       // camera XY with a small dead zone, preventing stationary camera sway from moving LOD bands
	float windRotationScale;
	uint occlusionMapDim;
	float4 frustumPlaneExtent;     // Left, right, bottom, top clip-plane extents for a unit world-space box.
	float4 grassHiZBounds;         // x: Far radius, y: near-tier clump reach, z: wind reach, w: High depth base cutoff; negative disables.
	float4 loadedLandBounds;       // xy: world min, zw: world max of the cells with attached LAND; terrain LOD is rendered outside.
	int4 terrainLiftOrigin;        // xy: world cell at the terrain lift map's window origin, zw: last frame's origin
	uint terrainLiftPhase;         // The quarter of the terrain lift map refreshed this frame
	float farHandoffDensityRatio;  // Low's blades per Far patch: (Low lattice density / Far lattice density)^2.
	float pad1;
	float pad2;
	int4 terrainCanopyWindow;    // World quadrant bounds, maximum exclusive.
	float4 terrainCanopyParams;  // x: transition start, y: inverse range, z: maximum blade height, w: Far density falloff.
}

// The terrain lift map stores the lift above LAND multiplied by its validity weight, alongside the weight.
// It wraps a camera-centred window of world-aligned cells, so texels keep their values as the camera moves.
static const float TerrainLiftMax = 160.0f;
static const float TerrainLiftCellSize = 256.0f;
static const int TerrainLiftDim = 512;

/** @brief Keeps roots on LOD terrain at the loaded ring's edge and reaches LAND before the Mid handoff. */
float GetTerrainLiftBlend(float squareDistance)
{
	return smoothstep(6144.0f, 8192.0f, squareDistance);
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
	float minAO;
	float specular;
	float clumpLeanFactor;

	float2 minMaxSubsurfaceOpacity;
	float clumpGridSize;
	float specularAnisotropy;         // OpenPBR specular_roughness_anisotropy, stretched across the blade
	float4 grassSurfParams;           // x: sheen (fuzz) strength, y: thin-subsurface anisotropy + 1, z: slope facing, w: sheen roughness
	float4 baseMinTipRoughnessStart;  // roughness at the base, at the smoothest point, and at the tip and t at which roughness bottoms out and starts climbing to the tip
	float4 midRoughnessPolynomial;    // x: cubic, y: quadratic, z: base; matches the authored curve at Mid's t={0,.5,1}
	float4 grassTypeLightParams;      // x: ground bounce, y: mean vertical height fraction, z: mean Far triangle area, w: ambient desaturation

	float4 baseColor;
	float4 tipColor;
	float4 grassColorTipDry;
	float4 grassColorVar;  // x: hue variation, y: brightness variation, z: tip-dry strength, w: mottle strength
	float4 grassColorCool;
	float4 grassColorWarm;
	float4 grassBounceColor;
	float4 grassTextureParams;           // x: blotch strength, y: blotch scale, z: speckle strength, w: speckle scale
	float4 grassVeinParams;              // rgb: vein albedo tint, w: vein albedo strength
	float4 grassVeinParams2;             // x: vein normal strength, y: ripple depth, z: micro-wiggle amount, w: curved normal strength
	float4 grassSubsurfaceColor;         // rgb: linear scattering tint divided by the reference blade colour, w: sheen (fuzz) tint toward the blade hue
	float4 fuzzDirectionalAlbedoParams;  // inverse width, centre, amplitude and offset of the MaterialX fuzz fit
};

#define GRASS_TYPE_COUNT 128

cbuffer GrassTypes : register(b9)
{
	GrassType grassType[GRASS_TYPE_COUNT];
}

/** @brief Darkens blade bases toward a clump's centre, where blades crowd together, and leaves the tips lit. */
float GetClumpAO(GrassType type, float clumpDensity, float bladeT)
{
	float baseWeight = 1.0f - saturate(bladeT);
	return lerp(1.0f, type.minAO, clumpDensity * type.clumpAOStrength * baseWeight * baseWeight);
}

float ApproximateGrassDistance(float2 offset)
{
	float2 distanceXY = abs(offset);
	return max(distanceXY.x, distanceXY.y) + min(distanceXY.x, distanceXY.y) * 0.375f;
}

// Mid draws two of High's four blades in each patch at High's width. Low and Far draw blades at this multiple of High's
// width. Keep in sync with PGrassCommon.h.
static const float DistantWidthScale = 1.5f;

/** @brief Share of a patch's extra slope slot to keep: the extra ground area a slope adds per unit of map area. */
float GetSlopeFillKeep(float terrainNormalZ)
{
	return saturate(1.0f / max(terrainNormalZ, 0.05f) - 1.0f);
}

// Mid hands off to Low from MidLowHandoffStart over MidLowHandoffBand. Keep in sync with PGrassCommon.h.
static const float MidLowHandoffStart = 6144.0f;
static const float MidLowHandoffBand = 1024.0f;

float GetMidLowBlend(float rootDistance)
{
	return smoothstep(4096.0f, 6144.0f, rootDistance);
}

#if defined(FAR_LOD)
// Far lays each blade's root edge across the view, so a blade covers its full width rather than the 2/pi a random
// facing averages. Far keeps this share of its candidates to cover the same ground with fewer blades.
static const float FarViewFacingKeep = 0.63661977f;

/** @brief Returns the horizontal axis across the view at a camera-relative position; Far blades take their width along it. */
float2 GetFarSideAxis(float2 cameraRelativeXY)
{
	return float2(-cameraRelativeXY.y, cameraRelativeXY.x) * rsqrt(max(dot(cameraRelativeXY, cameraRelativeXY), 1.0f));
}

/** @brief Packs the terrain normal's horizontal components into a Far record's bits 16-19 and 24-27. */
uint PackFarTerrainNormal(float2 normalXY)
{
	uint2 packed = (uint2)round(saturate(normalXY * 0.5f + 0.5f) * 15.0f);
	return packed.x << 16 | packed.y << 24;
}

/** @brief Unpacks the terrain normal stored by PackFarTerrainNormal. */
float3 UnpackFarTerrainNormal(uint seedAndType)
{
	float2 normalXY = float2((seedAndType >> 16) & 0xFu, (seedAndType >> 24) & 0xFu) * (2.0f / 15.0f) - 1.0f;
	return float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
}

// Far type IDs use seven bits (GRASS_TYPE_COUNT = 128); bit 7 marks an object root in this internal blade record.
static const uint FAR_OBJECT_SURFACE = 1u << 7u;

#	if defined(FAR_DOUBLE_GEOMETRY) || defined(FAR_DOUBLE_VERTEX)
/** @brief Places a Far double blade's second root within one lattice cell, from record bits that stay fixed for the blade. */
float2 GetFarDoubleRootOffset(uint seedAndType, uint posZWidthHeight)
{
	uint2 hash = Random::pcg2d(uint2(seedAndType, posZWidthHeight & 0xFFFFu));
	return (float2(hash) * (2.0f / 4294967296.0f) - 1.0f) * farParams.z;
}
#	endif

/** @brief Returns Far's blade width as a multiple of the authored width: Low's width at the seam, widening with distance. */
float GetFarWidthScale(float2 lodOffset)
{
	float2 distanceXY = abs(lodOffset);
	float squareDistance = max(distanceXY.x, distanceXY.y);
	return lerp(DistantWidthScale, 32.0f, saturate((squareDistance - farParams.x) * farParams.y));
}

// Keep in sync with PGrassCommon.h.
static const float FarMaxHeightScale = 2.0f;

/**
 * @brief Returns Far's blade height as a multiple of the authored height. Seen at a grazing angle, a blade field covers
 * the ground with its rows, so height rather than width makes up for the blades Far thins out with distance.
 */
float GetFarHeightScale(float2 lodOffset, float projectionScale)
{
	float2 distanceXY = abs(lodOffset);
	float squareDistance = max(distanceXY.x, distanceXY.y);
	float widthT = saturate((squareDistance - farParams.x) * farParams.y);
	float thinning = lerp(1.0f, terrainCanopyParams.w, widthT) * GetFarPerformanceKeep(ApproximateGrassDistance(lodOffset), projectionScale);
	float compensationBlend = smoothstep(0.0f, 1.0f, (squareDistance - farParams.x) * (1.0f / 4096.0f));
	return lerp(1.0f, min(rcp(max(thinning, 1.0e-3f)), FarMaxHeightScale), compensationBlend);
}

/** @brief Mean retention after Low's handoff, including diagonal seam coverage and radial unloading. */
float GetFarCanopyKeep(float2 offset, float projectionScale, float densityFalloff)
{
	float distance = length(offset);
	float squareDistance = max(abs(offset.x), abs(offset.y));
	float widthT = saturate((distance - farParams.x) * farParams.y);
	float unload = 1.0f - saturate((distance - farParams.x - rcp(max(farParams.y, 1.0e-6f))) * (1.0f / 2048.0f));
	float keep = lerp(1.0f, densityFalloff, widthT) * GetFarPerformanceKeep(distance, projectionScale);
	float seamKeep = 1.0f - saturate((squareDistance - farParams.x) * (1.0f / 4096.0f));
	return max(keep, seamKeep) * unload;
}

/** @brief Returns widening and thinning compensation after Far has cleared Low's square fade band. */
float2 GetFarCoverage(float2 lodOffset, float projectionScale)
{
	float2 distanceXY = abs(lodOffset);
	float squareDistance = max(distanceXY.x, distanceXY.y);
	float widthT = saturate((squareDistance - farParams.x) * farParams.y);
	float keep = GetFarPerformanceKeep(ApproximateGrassDistance(lodOffset), projectionScale);
	float compensation = min(rcp(max(keep, 0.5f)), 2.0f);
	float compensationBlend = smoothstep(0.0f, 1.0f, (squareDistance - farParams.x) * (1.0f / 4096.0f));
	return float2(widthT, lerp(1.0f, compensation, compensationBlend));
}
#endif

#if defined(CSHADER) || defined(DEPTH) || defined(MID_VERTEX)
struct GrassGeneratorType
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
	float clumpLeanFactor;
	float clumpGridSize;
	float inverseClumpGridSize;
	float slopeFacing;
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
	uint posXY;            // camera-relative x/y as two f16 values
	uint posZWidthHeight;  // camera-relative z as f16; low 16 are tier-specific geometry data
	uint facingTilt;       // 4x UNORM8 mapped to [-1,1]: facing.xy, tilt sin/cos
	uint seedAndType;      // high 4: clump density; 24-27 and 16-19: terrain normal; 20-23: bend; 8-15: clump seed; 7: object surface; low 7: type
};

#else
struct Blade
{
	uint posXY;            // camera-relative x/y as two f16 values
	uint posZWidthHeight;  // camera-relative z as f16; low 16 are tier-specific geometry data
	uint facingAndWind;    // low 16: current facing as 2x SNORM8; high 16 is tier-specific
	uint previousWind;     // tier-specific geometry and motion data
	uint hashClumpAndGrassType;
	uint tipDir;

#	if defined(MID_LOD)
	float3 skylightingRoot;  // Preserve the generator's full-precision probe position.
#	elif !defined(LOW_LOD)
	uint skylightingSH0;  // x/y as f16
	uint skylightingSH1;  // z/w as f16
#	endif

#	if defined(PGRASS_CACHED_COLLISION)
#		if defined(MID_LOD)
	uint collisionData;  // current x and y as f16, with current z in previousWind
#		else
	uint3 collisionData;  // current.xyz and previous.xyz packed as six f16 values
#		endif
#	endif
};
#endif

// Object surfaces shared by generation, lighting and terrain darkening.
// Unpadded topmost object height, geometric normal XY, and packed grass type/density.
Texture2D<float4> GrassObjectSurfaces : register(t66);

float GetGrassObjectTexelWidth()
{
	return 2.0f * occlusionHalfExtent / float(occlusionMapDim);
}

float2 GetGrassObjectTexelCentre(float2 texel)
{
	return (texel + 0.5f) * GetGrassObjectTexelWidth() + occlusionParams.xy - occlusionHalfExtent;
}

float GetGrassObjectHeightTolerance()
{
	return max(2.0f, GetGrassObjectTexelWidth() * 0.25f);
}

bool GetGrassObjectTexel(float2 worldPosition, out int2 texel)
{
	float2 uv = (worldPosition - occlusionParams.xy) * occlusionInvExtent + 0.5f;
	texel = int2(floor(uv * occlusionMapDim));
	return miscParams.y > 0.0f && all(texel > 0) && all(texel + 1 < int(occlusionMapDim));
}

/** @brief Reads one surface plane; zero-type records are blockers, not grass surfaces. */
bool LoadGrassObjectSurface(float2 worldPosition, out float height, out float2 slope, out uint type, out float density)
{
	height = 0.0f;
	slope = 0.0f;
	type = 0u;
	density = 0.0f;
	int2 texel;
	if (!GetGrassObjectTexel(worldPosition, texel))
		return false;

	float4 surface = GrassObjectSurfaces.Load(int3(texel, 0));
	uint packed = (uint)surface.w;
	type = packed & 0xFFu;
	if (type == 0u)
		return false;

	float normalZ = sqrt(saturate(1.0f - dot(surface.yz, surface.yz)));
	if (normalZ < 0.01f)
		return false;

	slope = -surface.yz / normalZ;
	float2 centre = GetGrassObjectTexelCentre(float2(texel));
	height = surface.x + dot(slope, worldPosition - centre);
	density = float(packed >> 8) * (1.0f / 255.0f);
	float2 toEdge = occlusionHalfExtent - abs(worldPosition - occlusionParams.xy);
	density *= smoothstep(0.0f, 256.0f, min(toEdge.x, toEdge.y));
	return true;
}

/** @brief Insets roots from ledges and discontinuities without dilating the object footprint. */
bool IsGrassObjectInterior(float2 worldPosition, float height, float2 slope, uint type)
{
	int2 texel;
	if (!GetGrassObjectTexel(worldPosition, texel))
		return false;

	float texelWidth = GetGrassObjectTexelWidth();
	float2 centre = GetGrassObjectTexelCentre(float2(texel));
	float centreHeight = height + dot(slope, centre - worldPosition);
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		int2 offset = int2(i == 0u ? -1 : (i == 1u ? 1 : 0), i == 2u ? -1 : (i == 3u ? 1 : 0));
		float4 neighbour = GrassObjectSurfaces.Load(int3(texel + offset, 0));
		if (((uint)neighbour.w & 0xFFu) != type || abs(neighbour.x - centreHeight - dot(slope, float2(offset) * texelWidth)) > GetGrassObjectHeightTolerance())
			return false;
	}
	return true;
}
