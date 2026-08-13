#pragma once

#include "ProceduralGrass/GrassCellCache.h"
#include "ProceduralGrass/PGrassCommon.h"
#include "ProceduralGrass/PGrassRenderer.h"

using namespace PGrassCommon;

struct ProceduralGrass : Feature
{
public:
	virtual inline std::string GetName() override { return "Procedural Grass"; }
	virtual inline std::string GetShortName() override { return "ProceduralGrass"; }

	struct Settings
	{
		bool Enabled = true;
		int32_t Quality = 1;
		int32_t ThreadGroupSize = 6;
	};

	Settings settings;

	virtual void DrawSettings() override;

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

	// Tier reach and caps live in PGrassCommon. QuadrantCount = the tier's (2r+1)^2 square
	// PatchBladeCount (4/2/1) is the blades-per-patch LOD, dropping with distance.
	PGrassRenderer<PGrassCommon::HighTierQuadrantCap, 4>* grassRendererHighLOD = nullptr;
	PGrassRenderer<PGrassCommon::MidTierQuadrantCap, 2>* grassRendererMidLOD = nullptr;
	PGrassRenderer<PGrassCommon::LowTierQuadrantCap, 1>* grassRendererLowLOD = nullptr;
	PGrassRenderer<PGrassCommon::FarQuadrantCount, 1>* grassRendererFarLOD = nullptr;

	std::vector<Quadrant> quadrantsHighLOD;
	std::vector<Quadrant> quadrantsMidLOD;
	std::vector<Quadrant> quadrantsLowLOD;
	std::vector<Quadrant> quadrantsFarLOD;

	// Reads LAND grass data straight from plugins, on worker threads, for quadrants beyond Low's reach. The near tiers' quadrants are resolved from the loaded cells' LAND data.
	GrassCellCache grassCellCache;

	ID3D11RasterizerState* noCullRS = nullptr;
	ID3D11DepthStencilState* depthOnDSS = nullptr;
	ID3D11DepthStencilState* depthWriteDS = nullptr;
	ID3D11DepthStencilState* depthEqualDS = nullptr;
	ID3D11BlendState* depthOnlyBlend = nullptr;
	ID3D11BlendState* defaultBlend = nullptr;
	ID3D11BlendState* multiplyBlend = nullptr;
	ID3D11DepthStencilState* noDepthDSS = nullptr;

	// Top-down grass density (one count per texel) and the pass that darkens terrain albedo from it.
	Texture2D* grassDensityTexture = nullptr;
	ID3D11VertexShader* densityAOVS = nullptr;
	ID3D11PixelShader* densityAOPS = nullptr;
	/** @brief Dither-discards the blade base in the depth prepass, matching the colour pass's terrain dissolve. */
	ID3D11PixelShader* depthClipPS = nullptr;
	static constexpr uint32_t grassDensityDim = 256;

	ID3D11SamplerState* linearClampSampler = nullptr;
	ID3D11SamplerState* shadowSampler = nullptr;

	ConstantBuffer* grassGlobalsCB = nullptr;
	ConstantBuffer* grassTypesArrayCB = nullptr;
	Buffer* vertexIndicesHighBuffer = nullptr;
	Buffer* vertexIndicesLowBuffer = nullptr;

	/** @brief Cached per-quadrant grass ids derived from the cell's LAND texture layers. */
	struct QuadrantGrass
	{
		RE::TESObjectLAND* land = nullptr;
		uint64_t lastSeenFrame = 0;
		std::array<uint8_t, PGrassCommon::QuadrantGrassSamples> ids{};
		/** @brief World Z per LAND vertex; all QuadrantNoHeight when heights are unavailable. */
		std::array<float, PGrassCommon::QuadrantGrassSamples> heights{};
	};

	std::unordered_map<uint64_t, QuadrantGrass> grassMapCache;
	uint64_t grassMapFrame = 0;


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
	 * @brief Resolves the grass ids and vertex heights for one LAND quadrant, rebuilding if stale.
	 * @return Cache entry, stable until it is evicted.
	 */
	const QuadrantGrass& GetQuadrantCache(RE::TESObjectLAND* land, uint32_t quadIndex, int32_t cellX, int32_t cellY);

	/** @brief Terrain Z at a world XY from cached LAND data, or nullopt outside the loaded cells. */
	std::optional<float> GetLandHeightAt(float worldX, float worldY) const;

	static bool ConsoleFunc_ToggleGrass();

	static std::vector<uint16_t> CreateVertexIndicesArray(uint16_t vertCount);

	static void CopyDepthBuffer(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer);
	static void SetViewport(ID3D11DeviceContext* ctx, float2 size);
	static void ClearRenderTargets(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtvs[7]);

	void PostDepthRendering();
	void GetVisibleQuadrants();
	void PostDepthRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer);
	void GenerateBlades(ID3D11DeviceContext* ctx) const;
	void RenderDepth(ID3D11DeviceContext* ctx) const;

	void DeferredRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer) const;
	void RenderGrass(ID3D11DeviceContext* ctx) const;

public:
	/** @brief Multiply-darkens the terrain albedo under grass from the density map; call before the composite. */
	void DarkenTerrainUnderGrass() const;
};
