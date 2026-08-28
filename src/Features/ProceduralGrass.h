#pragma once

#include "ProceduralGrass/GrassCellCache.h"
#include "ProceduralGrass/PGrassCommon.h"
#include "ProceduralGrass/PGrassRenderer.h"

#include <limits>

struct ProceduralGrass : Feature
{
public:
	virtual inline std::string GetName() override { return "Procedural Grass"; }
	virtual std::string GetDisplayName() override { return T("feature.procedural_grass.name", "Procedural Grass"); }
	virtual inline std::string GetShortName() override { return "ProceduralGrass"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kGrass; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.procedural_grass.description", "Generates configurable, dynamically lit grass across loaded terrain and distant landscape."),
			{ T("feature.procedural_grass.key_feature_1", "Dynamic grass rendering with realistic lighting and shading"),
				T("feature.procedural_grass.key_feature_2", "Configurable grass density and distribution"),
				T("feature.procedural_grass.key_feature_3", "Real-time grass animation with wind effects") } };
	};

	struct Settings
	{
		bool Enabled = true;
		int32_t Quality = 2;  // QualityDensities index

		// Blade shape and material
		float grassHeight = 100.0f;
		float grassWidth = 0.7f;
		float stiffness = 0.24f;
		float tipWeight = 0.54f;
		float mid = 0.73f;
		float rotationalStiffness = 1.0f;
		float ao = 0.10f;  // Minimum blade AO
		float specular = 0.20f;
		float2 subsurfaceOpacity = float2(0.8f, 0.10f);  // Base to tip
		float3 grassSubsurfaceTint = float3(1.50f, 1.00f, 0.60f);  // Backlight tint
		float3 baseMinTipRoughness = float3(0.65f, 0.45f, 0.55f);
		float tipRoughnessStart = 0.75f;
		float clumpAOStrength = 0.5f;

		// Colour
		float3 baseColor = float3(0.193f, 0.141f, 0.069f);
		float3 tipColor = float3(0.221f, 0.241f, 0.147f);
		float grassColorHueVariation = 0.60f;                   // Per-blade hue variation
		float grassColorValueVariation = 0.40f;                 // Per-blade brightness variation
		float grassColorTipDryStrength = 0.35f;                 // Tip dry-tint strength
		float grassColorMottleStrength = 0.15f;                 // Along-blade mottle strength
		float3 grassColorCool = float3(0.65f, 1.15f, 0.50f);    // Cool blade tint
		float3 grassColorWarm = float3(1.35f, 1.00f, 0.45f);    // Warm blade tint
		float3 grassColorTipDry = float3(1.20f, 1.08f, 0.70f);  // Dry tip tint

		// Detail and lighting
		float grassBaseAO = 0.35f;
		float grassClumpColorStrength = 0.6f;
		float grassMicroDetail = 0.5f;
		float grassAmbientFlatten = 0.7f;
		float grassCanopySkyOcclusion = 0.0f;
		float grassDensityAO = 0.2f;
		float grassWrap = 1.0f;
		float grassAniso = 0.15f;
		float grassBounceStrength = 0.35f;
		float3 grassBounceColor = float3(0.55f, 0.42f, 0.24f);
		float grassSunSelfShadow = 0.0f;
		float grassSpecOcclusion = 0.0f;
		float grassAmbientDesat = 0.5f;

		// Surface texture
		float grassBlotchStrength = 0.28f;
		float grassBlotchScale = 1.0f;
		float grassSpeckleStrength = 0.10f;
		float grassSpeckleScale = 1.0f;

		// Per-type vein detail
		float3 grassVeinTint = float3(0.70f, 0.80f, 0.66f);  // albedo tint in the vein grooves
		float grassVeinAlbedoStrength = 0.75f;               // how strongly the tint applies
		float grassVeinNormalStrength = 0.60f;               // vein normal-tilt amount
		float grassVeinRippleDepth = 0.28f;                  // along-blade ripple modulation of the veins
		float grassVeinWiggleAmount = 0.09f;                 // fine micro-wiggle of the surface normal

		// Terrain blend and shadow
		float grassTerrainBlendStrength = 1.0f;
		float grassTerrainBlendHeight = 2.0f;
		float grassTerrainBlendNormal = 0.8f;
		float grassTerrainBlendRough = 0.7f;
		float grassAOStrength = 0.6f;   // Terrain darkening. 0 disables it.
		float grassAODensity = 12.0f;   // Blades per full-coverage density texel

		// Clump
		int voronoiGridSize = 256;
		float clumpDistanceFactor = 0.1f;
		float clumpFacingFactor = 0.25f;
		float clumpHeightFactor = 0.5f;

		// Slope
		float grassMinSlope = 0.0f;
		float grassMaxSlope = 60.0f;    // Degrees. 90 never culls grass.
		float grassSlopeFacing = 0.2f;  // Downhill lean strength

		// Wind / animation
		float windAngle = 0.0f;
		float windSpeed = 0.4f;
		float spatialFreq = 1.0f;
		float phaseOffset = 0.5f;
		float phaseLag = 0.5f;

		// Occlusion / placement
		float occlusionClearance = 100.0f;  // Underside clearance in world units
		float occlusionHalfExtent = 10240.0f;
		float occlusionPadding = 8.0f;
		float occlusionBias = 4.0f;  // Minimum occluder height above a blade
		float grassMapEdgeNoise = 48.0f;
		float grassViewThicken = 1.0f;  // Edge-on blade widening. 0 disables it.

		// Per-LOD densities and far tier
		int midGrassDensity = 256;
		int lowGrassDensity = 256;
		int farGrassDensity = 96;
		int grassCellRadius = 6;
		float farDensityFalloff = 0.15f;

		struct GrassTypeDef
		{
			float weight = 1.0f;
			bool noGrass = false;
			nlohmann::json overrides = nlohmann::json::object();
		};
		// Keyed by "plugin|0xLOCALID" (LandTextureKey).
		std::unordered_map<std::string, std::vector<GrassTypeDef>> textureTypes;

		// Debug
		bool debugIgnoreGrassMap = false;
		bool debugIgnoreObjectOcclusion = false;
		bool debugDisableAllCulls = false;
		bool debugIgnorePreProcessedFlag = true;
	};

	Settings settings;

	virtual void DrawSettings() override;

	/** @brief Draws the per-type overrides and landscape-texture mix editor. */
	void DrawGrassTypeEditor();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	void DeferredRendering() const;

	struct Main_RenderShadowmasks_UpdateCamera
	{
		static void thunk(RE::BSGraphics::State* state, RE::NiCamera* camera, bool flag);
		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	enum class Quality : uint8_t
	{
		Low = 0,
		Medium = 1,
		High = 2,
		Ultra = 3,
		Count = 4
	};

	const char* QualityNames[static_cast<uint8_t>(Quality::Count)] = { "160", "192", "256", "320" };
	uint32_t QualityDensities[static_cast<uint8_t>(Quality::Count)] = { 160, 192, 256, 320 };

	PGrassRenderer<PGrassCommon::HighTierQuadrantCap, 4>* grassRendererHighLOD = nullptr;
	PGrassRenderer<PGrassCommon::MidTierQuadrantCap, 2>* grassRendererMidLOD = nullptr;
	PGrassRenderer<PGrassCommon::LowTierQuadrantCap, 1>* grassRendererLowLOD = nullptr;
	PGrassRenderer<PGrassCommon::FarQuadrantCount, 1>* grassRendererFarLOD = nullptr;

	std::vector<PGrassCommon::Quadrant> quadrantsHighLOD;
	std::vector<PGrassCommon::Quadrant> quadrantsMidLOD;
	std::vector<PGrassCommon::Quadrant> quadrantsLowLOD;
	std::vector<PGrassCommon::Quadrant> quadrantsFarLOD;
	std::vector<PGrassCommon::Quadrant> quadrantsPresence;

	bool vanillaToggled = false;

	// Far reads LAND data on workers. Near tiers use loaded cell LAND data.
	GrassCellCache grassCellCache;

	ID3D11RasterizerState* noCullRS = nullptr;
	ID3D11DepthStencilState* depthOnDSS = nullptr;
	ID3D11DepthStencilState* depthWriteDS = nullptr;
	ID3D11DepthStencilState* depthEqualDS = nullptr;
	ID3D11BlendState* depthOnlyBlend = nullptr;
	ID3D11BlendState* defaultBlend = nullptr;
	ID3D11BlendState* terrainFadeBlend = nullptr;
	ID3D11BlendState* multiplyBlend = nullptr;
	ID3D11DepthStencilState* noDepthDSS = nullptr;

	// Top-down grass density and the terrain-darkening pass.
	Texture2D* grassDensityTexture = nullptr;
	ID3D11VertexShader* densityAOVS = nullptr;
	ID3D11PixelShader* densityAOPS = nullptr;
	/** @brief Restricts High/Mid depth writes to the fully opaque portion above the terrain fade. */
	ID3D11PixelShader* depthClipPS = nullptr;
	static constexpr uint32_t grassDensityDim = 256;

	// Gathered density reads this Low-tier world-space grass-id texture without atomics.
	static constexpr uint32_t grassPresenceDim = (2 * PGrassCommon::LowTierQuadrantRadius + 1) * (PGrassCommon::QuadrantGrassPitch - 1) + 1;  // 177
	Texture2D* grassPresenceTexture = nullptr;
	ID3D11ComputeShader* densityGatherCS = nullptr;
	std::vector<uint8_t> grassPresenceStaging;  // grassPresenceDim^2 ids. 0 is bare.
	float2 grassPresenceOrigin = float2(0.0f, 0.0f);  // World-space texture origin
	int32_t grassPresenceOriginQuadX = (std::numeric_limits<int32_t>::min)();
	int32_t grassPresenceOriginQuadY = (std::numeric_limits<int32_t>::min)();
	uint64_t grassPresenceContentGeneration = (std::numeric_limits<uint64_t>::max)();
	mutable bool grassPresenceUploadDirty = true;

	ID3D11SamplerState* linearClampSampler = nullptr;
	ID3D11SamplerState* shadowSampler = nullptr;

	ConstantBuffer* grassGlobalsCB = nullptr;
	ConstantBuffer* grassTypesArrayCB = nullptr;
	ConstantBuffer* grassGeneratorTypesCB = nullptr;
	PGrassCommon::GrassTypesArray resolvedGrassTypes{};
	PGrassCommon::GrassGeneratorTypesArray resolvedGeneratorTypes{};
	bool grassTypesDirty = true;
	Buffer* vertexIndicesHighBuffer = nullptr;
	Buffer* vertexIndicesMidBuffer = nullptr;  // 9-index, five-vertex Mid blade
	Buffer* vertexIndicesLowBuffer = nullptr;
	Buffer* vertexIndicesFarBuffer = nullptr;  // 3-index single-triangle far blade

	/** @brief Cached grass ids and heights for one LAND quadrant. */
	struct QuadrantGrass
	{
		RE::TESObjectLAND* land = nullptr;
		uint64_t lastSeenFrame = 0;
		std::array<uint8_t, PGrassCommon::QuadrantGrassSamples> ids{};
		/** @brief World Z per LAND vertex. All values are QuadrantNoHeight when unavailable. */
		std::array<float, PGrassCommon::QuadrantGrassSamples> heights{};
		float minHeight = PGrassCommon::QuadrantNoHeight;
		float maxHeight = PGrassCommon::QuadrantNoHeight;
	};

	std::unordered_map<uint64_t, QuadrantGrass> grassMapCache;
	uint64_t grassMapFrame = 0;
	// Increments when cached ids or heights change so renderers re-upload stable pointers.
	uint64_t grassContentGeneration = 0;

	float2 windDirection = float2(1.0f, 0.0f);
	float previousShaderTimer = 0.0f;
	float nearQuadrantFrustumPadding = 0.0f;
	float farQuadrantFrustumPadding = 0.0f;

	/** @brief Raw LAND height inputs for the last resolved quadrant; debug panel only. */
	struct LandHeightDebug
	{
		float rawFirst;
		float rawMin;
		float2 extents;
		float anchor;
		float meshWorldZ;
	} landHeightDebug{};

	/** @brief Per-stage counters for why quadrants are rejected; debug panel only. */
	struct QuadrantReject
	{
		uint32_t cells;
		uint32_t withExterior;
		uint32_t withLand;
		uint32_t withLoadedData;
		uint32_t withMesh;
		uint32_t preProcessed;
	} quadrantReject{};

	/** @brief Packs a quadrant identity into a cache key shared by both per-quadrant caches. */
	static uint64_t QuadrantKey(int32_t cellX, int32_t cellY, uint32_t quadIndex);

	/**
	 * @brief Returns cached LAND grass ids and heights, rebuilding stale entries.
	 * @return The cache entry, stable until eviction.
	 */
	const QuadrantGrass& GetQuadrantCache(RE::TESObjectLAND* land, uint32_t quadIndex, int32_t cellX, int32_t cellY);

	/** @brief Returns terrain Z from cached LAND data, or nullopt outside loaded cells. */
	std::optional<float> GetLandHeightAt(float worldX, float worldY) const;

	/** @brief Returns geometric-mean Far patch density so Low and Far meet at the seam. */
	uint32_t FarPatchDensity() const
	{
		return std::max(8u, static_cast<uint32_t>(std::lround(std::sqrt(static_cast<double>(settings.lowGrassDensity) * settings.farGrassDensity))));
	}

	uint32_t FarBladeQuadrantCapacity() const
	{
		const uint32_t radius = static_cast<uint32_t>(std::clamp(settings.grassCellRadius, 0, 15));
		const uint32_t cellWidth = radius * 2u + 1u;
		return std::min(PGrassCommon::FarQuadrantCount, cellWidth * cellWidth * 4u);
	}

	/** @brief Builds a GPU GrassType from base settings and sparse JSON overrides. */
	PGrassCommon::GrassType ResolveGrassType(const nlohmann::json& typeOverride) const;

	/** @brief Returns the stable settings key for a land texture. */
	static std::string LandTextureKey(const RE::TESLandTexture* tex);

	/** @brief Weighted type selection for one texture. */
	struct TextureSelection
	{
		std::vector<uint8_t> ids;         // Global type id per variant
		std::vector<float> cumulative;    // Running weight sum
		float total = 0.0f;
	};

	// Maps texture variants to global type ids and weighted selections. Rebuilt when variants or weights change.
	std::vector<std::pair<std::string, uint32_t>> typeAllocation;
	std::unordered_map<std::string, TextureSelection> textureSelection;
	std::unordered_map<const RE::TESLandTexture*, const TextureSelection*> textureSelectionByTexture;

	/** @brief Rebuilds type ids and weighted selections from settings.textureTypes. */
	void RebuildTypeAllocation();
	void LoadTextureTypes();
	void SaveTextureTypes() const;

	static bool ConsoleFunc_ToggleGrass();

	static std::vector<uint16_t> CreateVertexIndicesArray(uint16_t vertCount);

	static void CopyDepthBuffer(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer);
	static void SetViewport(ID3D11DeviceContext* ctx, float2 size);

	void PostDepthRendering();
	void GetVisibleQuadrants();
	void PostDepthRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer);
	void GenerateBlades(ID3D11DeviceContext* ctx) const;
	void RenderDepth(ID3D11DeviceContext* ctx) const;

	void DeferredRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer) const;
	void RenderGrass(ID3D11DeviceContext* ctx) const;

public:
	/** @brief Multiply-darkens terrain albedo from the grass density map. */
	void DarkenTerrainUnderGrass() const;
};
