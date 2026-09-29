#pragma once

#include "PGrassCommon.h"

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <BS_thread_pool.hpp>

/**
 * @brief One exterior cell's grass inputs for the far tier, read from plugin LAND records for cells beyond the loaded grid.
 * Four quadrants, each a 17x17 grid of world Z and grass-or-bare ids, laid out as the runtime path produces.
 */
struct CellGrass
{
	std::array<uint64_t, 4> quadrantCacheVersions{};
	std::array<std::array<uint8_t, PGrassCommon::QuadrantGrassSamples>, 4> ids{};
	std::array<PGrassCommon::QuadrantOccupancy, 4> occupancy{};
	std::array<std::array<float, PGrassCommon::QuadrantGrassSamples>, 4> heights{};
	std::array<float, 4> minHeights{};
	std::array<float, 4> maxHeights{};
};

struct GrassTexturePolicy
{
	std::vector<uint8_t> ids;
	std::vector<float> cumulative;
	float total = 0.0f;

	bool operator==(const GrassTexturePolicy&) const = default;
};

/**
 * @brief Background reader + cache of per-cell grass data, mirroring the water cache's file-seeking.
 *
 * Reads run on a worker pool so first sight never hitches the render thread. Workers hand finished cells
 * to the main thread through a locked queue.
 */
class GrassCellCache
{
public:
	~GrassCellCache() { Shutdown(); }

	/** @brief Per-frame setup on the main thread. Rebuilds from scratch when the worldspace changes. */
	void BeginFrame(RE::TESWorldSpace* landWorldSpace);

	/** @brief Updates LTEX grass overrides used by streamed LAND reads and invalidates stale cells. */
	void SetGrassMapPolicy(std::unordered_map<const RE::TESLandTexture*, GrassTexturePolicy> textureOverrides, bool ignoreGrassMap);

	/** @brief Folds up to maxCount finished background reads into the readable map. Main thread only. */
	void DrainCompleted(size_t maxCount);

	/** @brief Returns a cached cell and marks it used this frame. */
	const CellGrass* Get(int32_t cellX, int32_t cellY);

	/** @brief Enqueues one missing cell. Returns true only when a new request was queued. */
	bool Request(int32_t cellX, int32_t cellY);

	uint64_t GetReadyVersion() const { return readyVersion; }

	/** @brief Drops cells not requested this frame. Call after gathering. Main thread only. */
	void EvictUntouched();

	void Shutdown();

private:
	static std::unique_ptr<CellGrass> ReadCell(RE::TESWorldSpace* worldSpace, std::shared_ptr<const std::vector<RE::TESFile*>> files, int32_t cellX, int32_t cellY,
		std::shared_ptr<const std::unordered_map<const RE::TESLandTexture*, GrassTexturePolicy>> textureOverrides, bool ignoreGrassMap);
	static void ParseLandscape(RE::TESFile* file, CellGrass& out, int32_t cellX, int32_t cellY,
		const std::unordered_map<const RE::TESLandTexture*, GrassTexturePolicy>& textureOverrides, bool ignoreGrassMap);

	std::unordered_map<uint64_t, std::unique_ptr<CellGrass>> ready;  // main-thread only
	std::unordered_map<uint64_t, uint64_t> lastTouched;              // key -> frame, main-thread only
	std::unordered_set<uint64_t> pending;                           // enqueued keys, main-thread only

	std::mutex completedMutex;
	std::deque<std::tuple<uint64_t, uint64_t, std::unique_ptr<CellGrass>>> completed;  // key, generation, data

	std::unique_ptr<BS::thread_pool<>> pool;
	RE::TESWorldSpace* worldSpace = nullptr;
	std::shared_ptr<const std::vector<RE::TESFile*>> files = std::make_shared<const std::vector<RE::TESFile*>>();
	std::shared_ptr<const std::unordered_map<const RE::TESLandTexture*, GrassTexturePolicy>> textureOverrides =
		std::make_shared<const std::unordered_map<const RE::TESLandTexture*, GrassTexturePolicy>>();
	bool ignoreGrassMap = false;
	std::atomic<uint64_t> generation{ 0 };  // bumped on worldspace change, stale results dropped
	uint64_t frame = 0;
	uint64_t nextCacheVersion = 1;
	uint64_t readyVersion = 1;
};
