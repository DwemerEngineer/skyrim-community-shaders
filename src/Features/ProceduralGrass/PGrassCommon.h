#pragma once

namespace PGrassCommon
{
	/** @brief Returns the game's default landscape texture used when a LAND quadrant has no base texture. */
	RE::TESLandTexture* GetDefaultLandTexture();

	inline constexpr uint64_t GrassHashOffsetBasis = 14695981039346656037ull;
	inline constexpr uint64_t GrassHashPrime = 1099511628211ull;

	inline void GrassHashBytes(uint64_t& hash, const void* data, size_t byteCount)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < byteCount; ++i) {
			hash ^= bytes[i];
			hash *= GrassHashPrime;
		}
	}

	template <class T>
	inline void GrassHashValue(uint64_t& hash, const T& value)
	{
		GrassHashBytes(hash, &value, sizeof(value));
	}

	constexpr uint64_t GrassCellKey(int32_t cellX, int32_t cellY)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(cellX)) << 32) |
		       static_cast<uint32_t>(cellY);
	}

	constexpr uint64_t GrassQuadrantKey(uint32_t quadrantX, uint32_t quadrantY)
	{
		return (static_cast<uint64_t>(quadrantX) << 32) | quadrantY;
	}

	// A LAND quadrant carries a 17x17 grid of texture-blend samples, so 128 world units apart.
	static constexpr uint32_t QuadrantGrassPitch = 17;
	static constexpr uint32_t QuadrantGrassSamples = QuadrantGrassPitch * QuadrantGrassPitch;
	static constexpr uint32_t QuadrantCellPitch = QuadrantGrassPitch - 1;
	using QuadrantOccupancy = std::array<uint16_t, QuadrantCellPitch>;

	/** @brief Builds a 16x16 occupied-cell mask after the generator's one-sample neighbour fill. */
	inline QuadrantOccupancy BuildQuadrantOccupancy(const uint8_t* ids)
	{
		QuadrantOccupancy rows{};
		if (!ids)
			return rows;

		for (uint32_t cellY = 0; cellY < QuadrantCellPitch; ++cellY) {
			uint16_t row = 0;
			for (uint32_t cellX = 0; cellX < QuadrantCellPitch; ++cellX) {
				const uint32_t minX = cellX > 0 ? cellX - 1 : 0;
				const uint32_t minY = cellY > 0 ? cellY - 1 : 0;
				const uint32_t maxX = std::min(cellX + 2, QuadrantGrassPitch - 1);
				const uint32_t maxY = std::min(cellY + 2, QuadrantGrassPitch - 1);

				bool occupied = false;
				for (uint32_t y = minY; y <= maxY && !occupied; ++y)
					for (uint32_t x = minX; x <= maxX; ++x)
						if (ids[y * QuadrantGrassPitch + x] != 0u) {
							occupied = true;
							break;
						}

				if (occupied)
					row |= static_cast<uint16_t>(1u << cellX);
			}
			rows[cellY] = row;
		}

		return rows;
	}

	/** @brief Returns one neighbouring grass id for a bare sample; reads use the original map so the fill cannot spread farther. */
	inline uint8_t FindAdjacentGrassId(const uint8_t* ids, uint32_t width, uint32_t height, uint32_t x, uint32_t y, int32_t worldSampleX, int32_t worldSampleY)
	{
		static constexpr int8_t offsets[8][2] = {
			{ -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 },
			{ 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }
		};
		const uint32_t first = (static_cast<uint32_t>(worldSampleX) * 73856093u ^ static_cast<uint32_t>(worldSampleY) * 19349663u) & 7u;

		for (uint32_t i = 0; i < 8; ++i) {
			const auto& offset = offsets[(first + i) & 7u];
			const int32_t sampleX = static_cast<int32_t>(x) + offset[0];
			const int32_t sampleY = static_cast<int32_t>(y) + offset[1];
			if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<int32_t>(width) || sampleY >= static_cast<int32_t>(height))
				continue;

			const uint8_t id = ids[static_cast<size_t>(sampleY) * width + sampleX];
			if (id != 0)
				return id;
		}

		return 0;
	}

	static constexpr int32_t HighTierQuadrantRadius = 2;
	static constexpr int32_t MidTierQuadrantRadius = 4;  // Extend Mid this far to avoid popping when the player moves between Mid and Low tiers.
	static constexpr int32_t LowTierQuadrantRadius = 5;  // Low overlaps Far across their radial transition band.

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
		uint64_t cacheVersion;
		bool nearCovered;  // a loaded near tier also renders this quadrant
		const uint8_t* grassIds;
		const uint16_t* occupancyRows;
		const float* heights;  // null when the LAND is unloaded
		float2 worldPos;       // cached lower-left world XY
		float minHeight;       // QuadrantNoHeight when unavailable
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
		float4 lodFadeIn;   // x: fade-in start dist (world), y: 1/range, z: Far seam-extra keep
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
		float previousWindSpeed;
		float2 windDir;
		float windAngle;

		float occlusionHalfExtent;
		float occlusionInvExtent;  // 1 / (2 * half extent), used by the generator's top-down-map UV transform
		float2 previousWindDir;
		float grassPBRLightingScale;  // TRUE_PBR's lighting scale, resolved for the active Linear Lighting mode.
		float4 occlusionParams;       // xy: window centre in world space, z: underside clearance, w: top-height bias (world units)

		float4 grassAOParams;     // x: density map dim, y: darken strength, z: blades-per-texel for full dark, w: canopy height (world units)
		float4 grassLightParams;  // x: density AO, y: canopy sky occlusion, z: resolved sun-shadow exponent, w: base canopy shading
		float4 grassFrameLight;   // xyz: resolved TRUE_PBR directional light, w: resolved grass brightness scale

		float4 farParams;          // x: thin start, y: inverse range, z: Far candidate spacing, w: Far performance keep
		float4 miscParams;         //  x: grass map edge noise in world units, y: slope facing, z: view thicken, w: timer delta
		float4 grassTerrainBlend;  // x: blend strength, y: blend height (world units), z: normal blend, w: roughness blend

		float2 heightMapScale;   // world space -> terrain heightmap UV, pairs with heightMapOffset
		float2 heightMapOffset;  // -pos0.xy * heightMapScale
		float2 heightMapZRange;  // {pos0.z, pos1.z}; texels are normalised and lerp between these

		float2 debugFlags;           // x: bypass every cull in the generator
		float4 grassPresenceParams;  // xy: world min-corner of the grass-id texture, z: 1/sample spacing, w: texture dim (density gather)
		float4 grassHiZParams;       // xy: valid base extent, z: nominal pixels/texel, w: trustworthy mip count; zero disables
		float2 grassLodOrigin;       // camera XY with a small dead zone, preventing stationary camera sway from moving LOD bands
		float2 _grassLodPadding;
	};
	STATIC_ASSERT_ALIGNAS_16(GrassGlobals);
	static_assert(offsetof(GrassGlobals, grassPBRLightingScale) == 60);
	static_assert(offsetof(GrassGlobals, grassFrameLight) == 112);
	static_assert(sizeof(GrassGlobals) == 256);

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
		float4 grassSurfParams;           // y: ambient normal flatten, z: wrap amount
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
		float4 grassVeinParams2;      // x: vein normal strength, y: ripple depth, z: micro-wiggle amount
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

	// Compact type data used by blade generation.
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
		uint posXY;           // camera-relative x/y as two f16 values
		uint posZWidthHeight;  // camera-relative z as f16, then width and height as UNORM8
		uint facingAndWind;  // low 16: current facing as 2x SNORM8; high 16: current wind displacement as f16
		uint previousWind;   // low 16: previous wind displacement or Mid collision Z; high 16: blade colour and bend
		uint hashClumpAndGrassType;
		uint tipDir;  // tier-specific packed tilt and lighting or distance data
	};
	static_assert(sizeof(Blade) == 24);

	// Struct for high blades to store a compact, per-blade skylighting SH value (four f16 values) along with the blade's packed data.
	struct BladeSkylit
	{
		Blade blade;
		uint skylightingSH0;
		uint skylightingSH1;
	};
	static_assert(sizeof(BladeSkylit) == 32);

	// Mid stores only the current bend because it has no previous-position output.
	struct BladeCollision
	{
		Blade blade;
		uint collisionData;
	};
	static_assert(sizeof(BladeCollision) == 28);

	struct BladeSkylitCollision
	{
		BladeSkylit blade;
		uint collisionData[3];
	};
	static_assert(sizeof(BladeSkylitCollision) == 44);

	struct BladeFar
	{
		uint posXY;           // camera-relative x/y as two f16 values
		uint posZWidthHeight;  // camera-relative z as f16, then width and height as UNORM8
		uint facingTilt;
		uint seedAndType;  // high 8: clump density; next 16: Voronoi-cell appearance seed; low 8: grass type
	};
	static_assert(sizeof(BladeFar) == 16);
}
