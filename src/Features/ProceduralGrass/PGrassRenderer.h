#pragma once

#include "PGrassCommon.h"

namespace PGrassRendererQuads
{
	// Packed visible-work layout consumed by PGrassBladeGeneratorCS.
	inline constexpr uint32_t WorkQuadrantMask = 0xFFFu;  // Far's fixed cbuffer capacity is 4,000.
	inline constexpr uint32_t WorkLaneShift = 12;
	inline constexpr uint32_t WorkHasLand = 1u << 16;
	inline constexpr uint32_t WorkInsideFrustum = 1u << 17;
	inline constexpr uint32_t WorkAllowSlopeExtras = 1u << 18;
	inline constexpr uint32_t WorkNearCovered = 1u << 19;
	inline constexpr uint32_t WorkCompactFar = 1u << 20;
	inline constexpr uint32_t WorkOccupiedTile = 1u << 21;
	inline constexpr uint32_t WorkTileShift = 22;
	inline constexpr uint32_t WorkTileMask = 0xFFu;
	inline constexpr uint32_t WorkFullGrass = 1u << 30;
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

	void GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<PGrassCommon::Quadrant>& quadrants, uint64_t contentVersion, int32_t cellXOffset, int32_t cellYOffset,
		const float2& lodOrigin, const float4& lodFadeIn, const float4& lodFadeOut, float frustumPadding, bool disableGeneratorCulls,
		float fadeInPositionPadding = 0.0f, float compactStartDistance = -1.0f, float compactKeep = 1.0f);
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
	std::string patchBladeCountString = std::to_string(PatchBladeCount);
	std::string slopeExtraBladesString = "0";
	std::string bladeBatchSizeString;
	uint32_t patchesPerQuadrant;
	uint32_t bladeBufferCapacity = 0;
	uint32_t threadGroupSize;
	std::string threadGroupSizeString;
	std::string quadrantCountString = std::to_string(QuadrantCount);

	ID3D11ComputeShader* bladeGeneratorCS = nullptr;
	ID3D11ComputeShader* compactBladeGeneratorCS = nullptr;
	ID3D11ComputeShader* batchArgsCS = nullptr;
	bool bladeGeneratorCompileAttempted = false;
	bool compactBladeGeneratorCompileAttempted = false;
	// Indexed by (depth ? 2 : 0) + (outer ? 1 : 0).
	std::array<ID3D11VertexShader*, 4> vertexShaders{};
	// Feature variants for wetness and local-light availability.
	std::array<ID3D11PixelShader*, 8> pixelShaders{};

	StructuredBuffer* bladesSB = nullptr;

	StructuredBuffer* quadrantGrassCellsSB = nullptr;
	std::vector<uint32_t> quadrantGrassCellsStaging;  // one packed 2x2 LAND-id cell per 16x16 quadrant cell
	StructuredBuffer* quadrantOccupancySB = nullptr;
	std::vector<uint32_t> quadrantOccupancyStaging;
	StructuredBuffer* quadrantHeightSB = nullptr;
	std::vector<float> quadrantHeightStaging;
	StructuredBuffer* tileHeightBoundsSB = nullptr;
	std::vector<float2> tileHeightBoundsStaging;
	StructuredBuffer* visibleWorkSB = nullptr;
	StructuredBuffer* visibleCompactWorkSB = nullptr;
	std::vector<uint32_t> visibleWorkStaging;
	std::vector<uint32_t> visibleCompactWorkStaging;
	bool compactWorkAllowsSlopeExtras = false;
	struct OccupiedTile
	{
		uint16_t tile = 0;
		uint16_t patchCount = 0;
	};
	std::array<float4, PGrassRendererQuads::OccupancyTileCount> tileLocalBounds{};
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
		float compactStartDistance;
		float compactKeep;
		float edgeNoise;
		bool disableGeneratorCulls;

		bool operator==(const WorkListState&) const = default;
	};
	WorkListState lastWorkListState{};
	uint64_t cachedRequiredBladeCount = 0;
	uint32_t cachedWorkGX = 0;
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
	bool UsesBatchedDraws() const { return UsesBatchedLow() || std::string_view(vertCountDefine) == "MID_VERTEX"; }
	void AppendVertexShaderDefines(ShaderDefines& defines) const;

	ID3D11ComputeShader* GetBladeGeneratorCS(bool compact = false);
	ID3D11ComputeShader* GetBatchArgsCS();
	/** @brief Appends lit-shader feature defines; simple lighting keeps only the features its reduced model evaluates. */
	void AppendFeatureDefines(ShaderDefines& defines, bool simpleLighting) const;
	/** @brief Returns the depth or colour VS for the inner or outer geometry segment, compiling on first use. */
	ID3D11VertexShader* GetVertexShader(bool depth, bool outer);
	ID3D11PixelShader* GetPS(bool noWetness = false, bool noLocalLights = false, bool innerHigh = false);

	/** @brief Patches kept per compact Far quadrant for the given keep fraction. */
	uint32_t CompactPatchCount(float compactKeep) const;
	/** @brief Stages and uploads per-quadrant grass cells, heights, occupancy and tile bounds when their inputs change. */
	void UploadQuadrantInputs(const std::vector<PGrassCommon::Quadrant>& quadrants, uint64_t contentVersion, int32_t cellXOffset, int32_t cellYOffset,
		const float4& lodFadeIn, const float4& lodFadeOut, float tileReach);
	/** @brief Packs one quadrant's 2x2 LAND grass ids per cell, filling bare samples from a neighbour. */
	void StageQuadrantGrassCells(uint32_t index, const PGrassCommon::Quadrant& quadrant);
	/** @brief Stages conservative per-tile LAND height bounds that cover jittered and clumped roots. */
	void StageTileHeightBounds(uint32_t index, const PGrassCommon::Quadrant& quadrant, float tileReach);
	/** @brief Returns the cached occupied tiles of a quadrant, recomputing them when its grass map changes. */
	const OccupancyCacheEntry& GetOccupiedTiles(const PGrassCommon::Quadrant& quadrant, float edgeNoise);
	/** @brief Culls quadrants and tiles on the CPU and uploads the generator's work list. */
	void BuildVisibleWorkList(const std::vector<PGrassCommon::Quadrant>& quadrants, const WorkListState& state);
	/** @brief Binds generator inputs and dispatches full, compact and batch-argument passes. */
	void DispatchGeneration(ID3D11DeviceContext* ctx, ID3D11ComputeShader* bladeGenerator, ID3D11ComputeShader* batchArgsGenerator, float compactKeep);

	static std::string BuildDefineList(std::span<const std::pair<const char*, const char*>> defines);

	template <class ShaderT>
	static ShaderT* CompileShader(const wchar_t* path, std::vector<std::pair<const char*, const char*>>& defines, const char* programType);
};
