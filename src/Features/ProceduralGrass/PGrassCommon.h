#pragma once

namespace PGrassCommon
{
	// A LAND quadrant carries a 17x17 grid of texture-blend samples, so 128 world units apart.
	static constexpr uint32_t QuadrantGrassPitch = 17;
	static constexpr uint32_t QuadrantGrassSamples = QuadrantGrassPitch * QuadrantGrassPitch;

	static constexpr int32_t HighTierQuadrantRadius = 2;
	static constexpr int32_t MidTierQuadrantRadius = 4;  // Extend Mid this far to avoid popping when the player moves between Mid and Low tiers.
	static constexpr int32_t LowTierQuadrantRadius = 5;  // Cover the last quadrants befor far, keeping far seperate since it covers LOD cells

	// Quadrants in an md<=r square (r in each of x and y), one per tier's renderer buffer.
	constexpr uint32_t QuadrantSquare(int32_t r) { return static_cast<uint32_t>((2 * r + 1) * (2 * r + 1)); }

	// Each tier's renderer holds a full (2r+1)^2 quadrant square.
	// Used by QuadrantCount to sizes its cbuffer + blade buffers and the cap used for each tier (25/81/121 for radii 2/4/5).
	static constexpr uint32_t HighTierQuadrantCap = QuadrantSquare(HighTierQuadrantRadius);
	static constexpr uint32_t MidTierQuadrantCap = QuadrantSquare(MidTierQuadrantRadius);
	static constexpr uint32_t LowTierQuadrantCap = QuadrantSquare(LowTierQuadrantRadius);

	// Set near the 4,096 dx11 cbuffer size cap to be able to fit as many far-tier quadrants as possible in a single cbuffer, to avoid multiple dispatches for far cells.
	static constexpr uint32_t FarQuadrantCount = 4000;

	struct Quadrant
	{
		int cellX;
		int cellY;
		uint x;
		uint y;
		const uint8_t* grassIds;
		const float* heights;            // null when the LAND is unloaded
		float2 worldPos;                 // cached lower-left world XY
		float minHeight;                 // QuadrantNoHeight when unavailable
		float maxHeight;
	};

	static constexpr float QuadrantNoHeight = -3.0e38f;
	
	struct alignas(16) QuadrantData
	{
		float2 quadWorldPos;
		uint quadrantHash;  // CPU-precomputed iqint3(quadX, quadY) for randomisation
		uint flags;
	};
	STATIC_ASSERT_ALIGNAS_16(QuadrantData);

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
		float occlusionInvExtent;  // 1 / (2 * half extent), used by the generator's top-down-map UV transform
		float3 _occlusionPadding;
		float4 occlusionParams;  // xy: window centre in world space, z: underside clearance, w: top-height bias (world units)

		float4 grassAOParams;     // x: density map dim, y: darken strength, z: blades-per-texel for full dark, w: canopy height (world units)
		float4 grassLightParams;  // x: density AO, y: canopy sky occlusion, z: sun self-shadow, w: base canopy shading

		float4 farParams;  // x: thin start dist (world), y: 1/(end-start), z: min keep fraction at far edge
		float4 miscParams;  //  x: grass map edge noise in world units, y: slope facing, z: view thicken, w: timer delta
		float4 grassTerrainBlend;  // x: blend strength, y: blend height (world units), z: normal blend, w: roughness blend

		float2 heightMapScale;      // world space -> terrain heightmap UV, pairs with heightMapOffset
		float2 heightMapOffset;     // -pos0.xy * heightMapScale
		float2 heightMapZRange;     // {pos0.z, pos1.z}; texels are normalised and lerp between these

		float2 debugFlags;          // x: bypass every cull in the generator
		float4 grassPresenceParams;  // xy: world min-corner of the grass-id texture, z: 1/sample spacing, w: texture dim (density gather)
	};
	STATIC_ASSERT_ALIGNAS_16(GrassGlobals);

	struct alignas(16) GrassType
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
		float4 grassSurfParams;  // x: micro-detail, y: ambient normal flatten, z: wrap amount, w: anisotropic specular
		float4 baseMinTipRoughnessStart;  // roughness at the base, at the smoothest point, and at the tip and t at which roughness bottoms out and starts climbing to the tip
		float4 midRoughnessPolynomial;  // x: cubic, y: quadratic, z: base; matches the authored curve at Mid's t={0,.5,1}
		float4 grassTypeLightParams;      // x: ground bounce, y: sky translucency, z: specular occlusion, w: ambient desaturation

		float4 baseColor;
		float4 tipColor;
		float4 grassColorTipDry;
		float4 grassColorVar;     // x: hue variation, y: brightness variation, z: tip-dry strength, w: mottle strength
		float4 grassColorCool;  
		float4 grassColorWarm;  
		float4 grassBounceColor;
		float4 grassTextureParams;  // x: blotch strength, y: blotch scale, z: speckle strength, w: speckle scale
		float4 grassVeinParams;     // rgb: vein albedo tint, w: vein albedo strength
		float4 grassVeinParams2;    // x: vein normal strength, y: ripple depth, z: micro-wiggle amount
		float4 grassSubsurfaceColor;  // rgb: subsurface/translucency tint
	};
	STATIC_ASSERT_ALIGNAS_16(GrassType);
	static_assert(sizeof(GrassType) == 320);

	// Slot 0 = bare, slot 1 = the base/default type, leaving 126 total slots for loaded per-texture variants.
	static constexpr uint32_t MaxGrassTypes = 128;

	struct GrassTypesArray
	{
		GrassType grassType[MaxGrassTypes];
	};

	// Simplfied type definition to save memory in the generator 
	struct alignas(16) GrassGeneratorType
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
	STATIC_ASSERT_ALIGNAS_16(GrassGeneratorType);

	struct GrassGeneratorTypesArray
	{
		GrassGeneratorType grassType[MaxGrassTypes];
	};

	struct Blade
	{
		uint posXY;
		uint posZWidthHeight;
		uint facingAndWind;  // low 16: static facing as 2x SNORM8; high 16: current wind displacement as f16
		uint previousWind;   // low 16: previous wind displacement as f16
		uint hashClumpAndGrassType;
		uint clumpDensity;
		uint tipDir;
	};
	static_assert(sizeof(Blade) == 28);

	// Struct for high blades to store a compact, per-blade skylighting SH value (four f16 values) along with the blade's packed data.
	struct BladeSkylit
	{
		Blade blade;
		uint skylightingSH0;
		uint skylightingSH1;
	};
	static_assert(sizeof(BladeSkylit) == 36);

	struct BladeFar
	{
		uint posXY;
		uint posZWidthHeight;
		uint facingTilt;
		uint seedAndType;
	};
	static_assert(sizeof(BladeFar) == 16);
}
