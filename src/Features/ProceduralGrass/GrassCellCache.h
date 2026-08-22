#pragma once

#include "PGrassCommon.h"

#include <array>
#include <atomic>
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
	std::array<std::array<uint8_t, PGrassCommon::QuadrantGrassSamples>, 4> ids{};
	std::array<std::array<float, PGrassCommon::QuadrantGrassSamples>, 4> heights{};
	std::array<float, 4> minHeights{};
	std::array<float, 4> maxHeights{};
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

	/** @brief Folds finished background reads into the readable map. Main thread only. */
	void DrainCompleted();

	/** @brief The cell's grass if already read; otherwise nullptr, enqueuing a background read once. */
	const CellGrass* GetOrRequest(int32_t cellX, int32_t cellY);

	/** @brief Drops cells not requested this frame. Call after gathering. Main thread only. */
	void EvictUntouched();

	void Shutdown();

private:
	static uint64_t Key(int32_t cellX, int32_t cellY);
	static std::unique_ptr<CellGrass> ReadCell(RE::TESWorldSpace* worldSpace, RE::TESFileArray* files, int32_t cellX, int32_t cellY);
	static void ParseLandscape(RE::TESFile* file, CellGrass& out);

	std::unordered_map<uint64_t, std::unique_ptr<CellGrass>> ready;  // main-thread only
	std::unordered_map<uint64_t, uint64_t> lastTouched;              // key -> frame, main-thread only
	std::unordered_set<uint64_t> pending;                           // enqueued keys, main-thread only

	std::mutex completedMutex;
	std::vector<std::tuple<uint64_t, uint64_t, std::unique_ptr<CellGrass>>> completed;  // key, generation, data

	std::unique_ptr<BS::thread_pool<>> pool;
	RE::TESWorldSpace* worldSpace = nullptr;
	RE::TESFileArray* files = nullptr;
	std::atomic<uint64_t> generation{ 0 };  // bumped on worldspace change, stale results dropped
	uint64_t frame = 0;
};
