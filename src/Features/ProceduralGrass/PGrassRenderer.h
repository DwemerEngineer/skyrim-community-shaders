#pragma once

#include "PGrassCommon.h"

namespace PGrassRendererQuads
{
	// Packed visible-work layout consumed by PGrassBladeGeneratorCS.
	inline constexpr uint32_t WorkQuadrantMask = 0xFFFu;  // Far's fixed cbuffer capacity is 4,000.
	inline constexpr uint32_t WorkLaneShift = 12;
	inline constexpr uint32_t WorkHasLand = 1u << 16;
	inline constexpr uint32_t WorkInsideFrustum = 1u << 17;
	inline constexpr uint32_t WorkObjectSurface = 1u << 18;  // Internal work-list bit, unrelated to NIF and material flags.
	inline constexpr uint32_t WorkNearCovered = 1u << 19;
	inline constexpr uint32_t WorkCompactFar = 1u << 20;  // Generates only its dispatch's share of the item's patches.
	inline constexpr uint32_t WorkShareSteps = 8;         // Compact Far work generates a multiple of 1/8 of its patches.
	inline constexpr uint32_t WorkOccupiedTile = 1u << 21;
	inline constexpr uint32_t WorkTileShift = 22;
	inline constexpr uint32_t WorkTileMask = 0xFFu;
	inline constexpr uint32_t WorkFullGrass = 1u << 30;
	inline constexpr uint32_t WorkFarHandoff = 1u << 31;  // Far work near Low, generated with the handoff fill.
	inline constexpr float FarHandoffFillFade = 4096.0f;  // Keep in sync with PGrassGeneration.hlsli.
	inline constexpr uint32_t OccupancyTilesPerAxis = PGrassCommon::QuadrantGrassPitch - 1;
	inline constexpr uint32_t OccupancyTileCount = OccupancyTilesPerAxis * OccupancyTilesPerAxis;

	/** Stable quadrant identity hash, generated once on the CPU for all patches in that quadrant. */
	uint32_t QuadrantHash(uint32_t x, uint32_t y);

	enum class QuadrantFrustumState : uint8_t
	{
		Outside,
		Intersecting,
		Inside,
	};

	struct SideFrustum
	{
		std::array<float4, 4> planes{};
	};

	SideFrustum BuildSideFrustum(const float4x4& viewProj);

	/** Conservative XY clip test for a padded quadrant LAND AABB. Near/far clipping intentionally matches the generator and stays disabled. */
	QuadrantFrustumState ClassifyQuadrantFrustum(const PGrassCommon::Quadrant& quadrant, const SideFrustum& frustum, const float4& cameraPosAdjust, float xyPadding, bool& hasLand);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
class PGrassRenderer
{
public:
	/**
	 * @brief Creates a tier renderer with a lazily sized blade buffer.
	 *
	 * @param slopeExtraBlades Extra candidate blade slots per patch for filling sloped ground. Keep this small because it enlarges the
	 *						   blade buffer and the base thread's candidate loop.
	 */
	PGrassRenderer(uint32_t grassDensity, uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef = nullptr,
		uint32_t slopeExtraBlades = 0, uint32_t bladeStrideBytes = sizeof(PGrassCommon::Blade), Buffer* outerVertexIndicesBuf = nullptr);

	void SetDensity(uint32_t grassDensity);
	/** Discards the retained blade-buffer capacity. */
	void ResetBladeCapacity();
	void SetThreadGroupSize(uint32_t tgSize);

	void ClearShaderCache();

	/**
	 * @brief Builds the visible work list when its inputs change and dispatches blade generation.
	 * @param farKeepParams Far only. x: start of Far's distance thinning, y: its inverse length, z: the keep at its end,
	 * w: the view-facing keep. Bounds each quadrant's keep so its work generates only the patches that keep can retain.
	 */
	void GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<PGrassCommon::Quadrant>& quadrants, uint64_t contentVersion, int32_t cellXOffset, int32_t cellYOffset,
		const float2& lodOrigin, const float4& lodFadeIn, const float4& lodFadeOut, float frustumPadding, bool disableGeneratorCulls,
		float fadeInPositionPadding = 0.0f, const float4& farKeepParams = float4());
	void RenderDepth(ID3D11DeviceContext* ctx, ID3D11PixelShader* depthClipPS = nullptr);
	void RenderGrass(ID3D11DeviceContext* ctx);

	/** @brief Reads back the instance count generated last frame. Debug only; stalls. */
	uint32_t ReadBladeCount() const;

private:
	using ShaderDefines = std::vector<std::pair<const char*, const char*>>;

	const char* lodDefine;
	const char* vertCountDefine;
	const char* extraDefine;
	uint32_t density;
	uint32_t bladeStrideBytes = sizeof(PGrassCommon::Blade);  // High stores SH, Mid stores the probe root; either may include collision. Far is 16 bytes.
	std::string densityString;
	uint32_t slopeExtraBlades = 0;
	uint32_t handoffExtraBlades = 0;  // Far's slope slot plus the fill slots that match Low's density in the handoff band.
	std::string patchBladeCountString = std::to_string(PatchBladeCount);
	std::string bladeBatchSizeString;
	uint32_t patchesPerQuadrant;
	uint32_t bladeBufferCapacity = 0;
	uint32_t threadGroupSize;
	std::string threadGroupSizeString;
	std::string quadrantCountString = std::to_string(QuadrantCount);

	ID3D11ComputeShader* generatorCS = nullptr;
	bool generatorCompileAttempted = false;
	ID3D11ComputeShader* handoffGeneratorCS = nullptr;
	bool handoffGeneratorCompileAttempted = false;
	ID3D11ComputeShader* batchArgsCS = nullptr;

	// Indexed by (depth ? 2 : 0) + (outer ? 1 : 0).
	std::array<ID3D11VertexShader*, 4> vertexShaders{};

	// Feature variants for wetness and local-light availability.
	std::array<ID3D11PixelShader*, 8> pixelShaders{};

	StructuredBuffer* bladesSB = nullptr;

	StructuredBuffer* quadrantGrassCellsSB = nullptr;
	std::vector<std::array<uint8_t, PGrassCommon::QuadrantGrassSamples>> quadrantGrassIdsStaging;
	std::vector<uint32_t> quadrantGrassCellsStaging;  // one packed 2x2 LAND-id cell per 16x16 quadrant cell
	StructuredBuffer* quadrantOccupancySB = nullptr;
	std::vector<uint32_t> quadrantOccupancyStaging;
	std::vector<uint64_t> quadrantOccupancyVersions;

	StructuredBuffer* quadrantHeightSB = nullptr;
	std::vector<float> quadrantHeightStaging;

	StructuredBuffer* tileHeightBoundsSB = nullptr;
	std::vector<float2> tileHeightBoundsStaging;
	std::vector<PGrassCommon::QuadrantOccupancy> objectOccupancyRows;
	std::vector<float2> objectHeightBounds;

	StructuredBuffer* visibleWorkSB = nullptr;
	std::vector<uint32_t> visibleWorkStaging;
	uint32_t visibleTerrainWorkCount = 0;
	uint32_t visibleObjectHandoffCount = 0;
	uint32_t cachedObjectGX = 0;
	uint32_t visibleHandoffWorkCount = 0;  // Far work items at the front of the list that take the handoff fill.
	// Far's base work items, after the handoff items, ordered by the eighths of their patches they generate.
	std::array<uint32_t, PGrassRendererQuads::WorkShareSteps> baseWorkShareCounts{};
	std::unique_ptr<ConstantBuffer> workRangeCB;

	struct OccupiedTile
	{
		uint16_t tile = 0;
		uint16_t patchCount = 0;
	};

	std::array<float4, PGrassRendererQuads::OccupancyTileCount> tileLocalBounds{};
	std::array<uint32_t, PGrassRendererQuads::OccupancyTileCount> tilePatchCounts{};
	std::vector<OccupiedTile> visibleTilesStaging;

	struct OccupancyCacheEntry
	{
		uint64_t cacheVersion = 0;
		uint32_t density = 0;
		float edgeNoise = -1.0f;
		uint16_t occupiedTileCount = 0;
		std::array<OccupiedTile, PGrassRendererQuads::OccupancyTileCount> occupiedTiles{};
	};

	std::unordered_map<uint64_t, OccupancyCacheEntry> occupancyCache;

	struct VisibleWorkCandidate
	{
		uint32_t quadrantIndex = 0;
		uint32_t flags = 0;
		uint32_t tileOffset = 0;
		uint32_t tileCount = 0;
		const OccupiedTile* cachedTiles = nullptr;
		uint32_t shareStep = PGrassRendererQuads::WorkShareSteps;
	};

	std::vector<VisibleWorkCandidate> visibleWorkCandidates;

	struct WorkListState
	{
		uint64_t contentVersion;
		uint32_t density;
		uint32_t threadGroupSize;
		uint32_t quadrantCount;

		float4x4 viewProj;
		float4 cameraPosAdjust;
		float2 lodOrigin;

		float4 lodFadeIn;
		float4 lodFadeOut;

		float frustumPadding;
		float fadeInPositionPadding;
		float4 farKeepParams;
		float edgeNoise;

		bool disableGeneratorCulls;

		bool operator==(const WorkListState&) const = default;
	};

	WorkListState lastWorkListState{};
	uint64_t cachedRequiredBladeCount = 0;
	uint32_t cachedWorkGX = 0;
	uint32_t cachedWorkPatchCount = 0;  // Patches a work item can span: one tile's, or the whole quadrant's.
	bool hasCachedWorkList = false;

	ConstantBuffer* quadrantsCB = nullptr;
	PGrassCommon::QuadrantDataArray<QuadrantCount> quadrantDataStaging{};

	// Skip staging rebuilds and uploads while the tier content and fade constants are unchanged.
	uint64_t lastUploadVersion = 0;
	float4 lastUploadLodFadeIn{};
	float4 lastUploadLodFadeOut{};
	float lastTileReach = -1.0f;
	bool hasUploadedQuadrants = false;

	Buffer* argsBuffer = nullptr;
	Buffer* batchArgsBuffer = nullptr;
	winrt::com_ptr<ID3D11Buffer> argsStaging;
	Buffer* vertexIndicesBuffer = nullptr;
	Buffer* outerVertexIndicesBuffer = nullptr;

	void CreateArgsBuffer();
	/** Grow the blade buffer for this frame's visible candidate work. */
	void EnsureBladeCapacity(uint64_t requiredBladeCount);
	bool UsesGrassCollision(bool grassCollisionLoaded) const
	{
		const auto lod = std::string_view(lodDefine);
		return grassCollisionLoaded && (lod == "HIGH_LOD" || lod == "MID_LOD");
	}
	bool UsesSimpleLighting() const;
	bool UsesBatchedLow() const { return std::string_view(vertCountDefine) == "LOW_VERTEX"; }
	bool UsesBatchedMid() const { return std::string_view(vertCountDefine) == "MID_VERTEX"; }

	bool UsesBatchedDraws() const { return UsesBatchedLow() || UsesBatchedMid() || extraDefine; }
	void AppendVertexShaderDefines(ShaderDefines& defines) const;

	ID3D11ComputeShader* GetBladeGeneratorCS(bool farHandoff = false);
	void ClearGeneratorCache();
	ID3D11ComputeShader* GetBatchArgsCS();
	/** @brief Appends lit-shader feature defines; simple lighting keeps only the features its reduced model evaluates. */
	void AppendFeatureDefines(ShaderDefines& defines, bool simpleLighting) const;

	/** @brief Returns the depth or colour VS for the inner or outer geometry segment, compiling on first use. */
	ID3D11VertexShader* GetVertexShader(bool depth, bool outer);
	ID3D11PixelShader* GetPS(bool noWetness = false, bool noLocalLights = false, bool innerHigh = false);

	/** @brief Patches a work item spanning patchCount generates at a share of shareStep eighths. */
	static uint32_t SharedPatchCount(uint32_t patchCount, uint32_t shareStep);
	/** @brief Stages and uploads per-quadrant grass cells, heights, occupancy and tile bounds when their inputs change. */
	void UploadQuadrantInputs(const std::vector<PGrassCommon::Quadrant>& quadrants, uint64_t contentVersion, int32_t cellXOffset, int32_t cellYOffset,
		const float4& lodFadeIn, const float4& lodFadeOut, float tileReach);

	/** @brief Fills one quadrant's bare grass samples from a neighbour. */
	void StageQuadrantGrassIds(uint32_t index, const PGrassCommon::Quadrant& quadrant);
	/** @brief Reconciles shared border samples before packing grass cells and their occupancy masks. */
	void PackQuadrantGrassCells(const std::vector<PGrassCommon::Quadrant>& quadrants);
	/** @brief Stages conservative per-tile LAND height bounds that cover jittered and clumped roots. */
	void StageTileHeightBounds(uint32_t index, const PGrassCommon::Quadrant& quadrant, float tileReach);

	/** @brief Returns the cached occupied tiles of a quadrant, recomputing them when its grass map changes. */
	const OccupancyCacheEntry& GetOccupiedTiles(const PGrassCommon::Quadrant& quadrant, uint32_t index, float edgeNoise);
	/** @brief Culls quadrants and tiles on the CPU and uploads the generator's work list. */
	void BuildVisibleWorkList(const std::vector<PGrassCommon::Quadrant>& quadrants, const WorkListState& state);

	/** @brief Binds generator inputs, dispatches handoff and base work, and prepares batched draw arguments. */
	void DispatchGeneration(ID3D11DeviceContext* ctx, ID3D11ComputeShader* bladeGenerator, ID3D11ComputeShader* batchArgsGenerator);

	static std::string BuildDefineList(std::span<const std::pair<const char*, const char*>> defines);

	template <class ShaderT>
	static ShaderT* CompileShader(const wchar_t* path, std::vector<std::pair<const char*, const char*>>& defines, const char* programType);
};
