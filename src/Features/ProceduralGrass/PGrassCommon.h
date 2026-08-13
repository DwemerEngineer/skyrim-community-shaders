#pragma once

namespace PGrassCommon
{
	// A LAND quadrant carries a 17x17 grid of texture-blend samples, so 128 world units apart.
	static constexpr uint32_t QuadrantGrassPitch = 17;
	static constexpr uint32_t QuadrantGrassSamples = QuadrantGrassPitch * QuadrantGrassPitch;

	// Per-tier reach as md, the larger of the x/y quadrant offsets from the player (2 quadrants per cell), with a spacing of 1 cell
	// Adjacent tiers overlap by one step so they cross-fade, and every quadrant past LowTierQuadrantRadius is Far, regardless of ugrids.
	static constexpr int32_t HighTierQuadrantRadius = 4;
	static constexpr int32_t MidTierQuadrantRadius = 6;
	static constexpr int32_t LowTierQuadrantRadius = 8;

	// Quadrants in an md<=r square (r in each of x and y), one per tier's renderer buffer.
	constexpr uint32_t QuadrantSquare(int32_t r) { return static_cast<uint32_t>((2 * r + 1) * (2 * r + 1)); }

	// Each tier's renderer holds a full (2r+1)^2 quadrant square.
	// Used by QuadrantCount to sizes its cbuffer + blade buffers and the cap used for each tier (81/169/289 for radii 4/6/8).
	static constexpr uint32_t HighTierQuadrantCap = QuadrantSquare(HighTierQuadrantRadius);
	static constexpr uint32_t MidTierQuadrantCap = QuadrantSquare(MidTierQuadrantRadius);
	static constexpr uint32_t LowTierQuadrantCap = QuadrantSquare(LowTierQuadrantRadius);

	// Far-tier capacity for quadrants beyond Low's fixed reach (LowTierQuadrantRadius).
	// Set near the 4,096 dx11 cbuffer size cap to be able to fit as many far-tier quadrants as possible in a single cbuffer, to avoid multiple dispatches for far cells.
	static constexpr uint32_t FarQuadrantCount = 4000;

	struct Quadrant
	{
		int cellX;
		int cellY;
		uint quadrantX;
		uint quadrantY;
		const uint8_t* grassIds;         // QuadrantGrassSamples entries, owned by ProceduralGrass's cache
		const float* heights;            // QuadrantGrassSamples world Z values, null when the LAND is unloaded
	};

	static constexpr float QuadrantNoHeight = -3.0e38f;
	
	struct alignas(16) QuadrantData
	{
		float2 quadWorldPos;
		uint quadX;
		uint quadY;
	};

	template <std::size_t N>
	struct alignas(16) QuadrantDataArray
	{
		// Per-tier LOD cross-fade bands, so a quadrant dithers in/out at tier boundaries instead of popping.
		float4 lodFadeIn;   // x: fade-in start dist (world), y: 1/range; keep ramps 0 -> 1
		float4 lodFadeOut;  // x: fade-out start dist (world), y: 1/range, z: min keep at/after the far edge
		QuadrantData data[N];
	};

	struct alignas(16) GrassGlobals
	{
		float4 color;
		float2 dynamicResolutionInverted;
		float voronoiGridSize;
		float inverseVoronoiGridSize;
		float cameraViewRow0Sum;
		float cameraViewRow1Sum;
		float windSpeed;
		float windTimer;
		float2 windDir;
		float2 heightMapScale;   // world space -> terrain heightmap UV, pairs with heightMapOffset
		float2 heightMapOffset;  // -pos0.xy * heightMapScale
		float2 heightMapZRange;  // {pos0.z, pos1.z}; texels are normalised and lerp between these
		float4 debugFlags;       // x: bypass every cull in the generator
		float4 occlusionParams;  // x: NDC depth bias for the top-down occlusion mask test
		float4 grassParams;      // x: occluder padding in world units, y: grass map edge noise in world units
		float4 occlusionWindow;  // xy: window centre in world space, z: half extent, w: unused
		float4 grassAOParams;     // x: density map dim, y: darken strength, z: blades-per-texel for full dark
		float4 farParams;         // x: thin start dist (world), y: 1/(end-start), z: min keep fraction at far edge
		float4 grassColorVar;     // x: hue variation, y: brightness variation, z: tip-dry strength, w: mottle strength
		float4 grassColorCool;    // rgb: cooler per-blade tint
		float4 grassColorWarm;    // rgb: warmer per-blade tint
		float4 grassColorTipDry;  // rgb: dried-tip tint
		float4 grassDetailParams; // x: sun-glow strength, y: base AO depth, z: clump colour strength, w: micro-detail
		float4 grassLightParams;  // x: ambient normal flatten, y: canopy sky occlusion, z: density AO, w: wrap amount
		float4 grassLightParams2; // x: anisotropic specular, y: ground bounce strength, z: canopy height scale
		float4 grassBounceColor;  // rgb: ground bounce tint
		float4 grassTextureParams;  // x: blotch strength, y: blotch scale, z: speckle strength, w: speckle scale
		float4 grassTerrainBlend;   // x: blend strength, y: blend height (world units), z: normal blend, w: roughness blend
		float4 grassLightParams3;   // x: sun self-shadow, y: sky translucency, z: sky sheen, w: specular occlusion
	};

	struct alignas(16) GrassType
	{
		float height;
		float width;
		float stiffness;
		float tipWeight;
		float mid;
		float minAO;
		float rotationalStiffness;
		float specular;
		float2 minMaxSubsurfaceOpacity;
		float clumpDistanceFactor;
		float clumpHeightFactor;
		float clumpFacingFactor;
		float spatialFreq;
		float phaseOffset;
		float phaseLag;
		float3 baseMinTipRoughness;  // roughness at the base, at the smoothest point, and at the tip
		float tipRoughnessStart;     // t at which roughness bottoms out and starts climbing to the tip
		float3 baseColor;
		float baseColorPadding;
		float3 tipColor;
		float clumpAOStrength;
	};

	struct GrassTypesArray
	{
		GrassType grassType[2];
	};

	struct Blade
	{
		uint positionXY;
		uint positionZFacing;
		uint widthHeight;
		uint hashClumpAndGrassType;
		uint clumpDensity;
		uint previousFacing;
	};
}
