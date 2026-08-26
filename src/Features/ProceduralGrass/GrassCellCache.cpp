#include "GrassCellCache.h"

#include "Utils/Game.h"

#include <algorithm>
#include <bit>
#include <thread>

namespace
{
	// LAND subrecord four-character codes.
	constexpr uint32_t kVHGT = Util::FCC("VHGT");
	constexpr uint32_t kBTXT = Util::FCC("BTXT");
	constexpr uint32_t kATXT = Util::FCC("ATXT");
	constexpr uint32_t kVTXT = Util::FCC("VTXT");

	constexpr uint32_t kQuadrantPitch = PGrassCommon::QuadrantGrassPitch;  // 17 vertices per side
	constexpr uint32_t kQuadrantSamples = PGrassCommon::QuadrantGrassSamples;  // 17 x 17 = 289 samples
	constexpr uint32_t kCellVertexPitch = 33;

	// Swaps only fire on the rare big-endian file, matching the water cache.
	uint32_t Swap32(uint32_t v) { return _byteswap_ulong(v); }
	uint16_t Swap16(uint16_t v) { return _byteswap_ushort(v); }
	float SwapF(float v) { return std::bit_cast<float>(_byteswap_ulong(std::bit_cast<uint32_t>(v))); }

	// BTXT/ATXT share this 8-byte header: texture FormID, target quadrant, and layer index.
	struct LandscapeTextureHeader
	{
		RE::FormID landTexture;
		uint8_t quadrant;
		uint8_t unused;
		int16_t layer;
	};
	static_assert(sizeof(LandscapeTextureHeader) == 8);

	// VTXT alpha entry of a quadrant-local vertex index and its blend opacity.
	struct VertexTextureAlpha
	{
		uint16_t position;
		uint8_t unused[2];
		float opacity;
	};
	static_assert(sizeof(VertexTextureAlpha) == 8);

	RE::TESLandTexture* ResolveLandTexture(RE::TESFile* file, RE::FormID rawFormID)
	{
		if (!rawFormID)
			return PGrassCommon::GetDefaultLandTexture();
		return RE::TESForm::LookupByID<RE::TESLandTexture>(file->GetRuntimeFormID(rawFormID));
	}
}

uint64_t GrassCellCache::Key(int32_t cellX, int32_t cellY)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(cellX)) << 32) | static_cast<uint32_t>(cellY);
}

void GrassCellCache::BeginFrame(RE::TESWorldSpace* landWorldSpace)
{
	frame++;

	if (!pool) {
		// A small pool suffices since reads are light and cache once, without competing with the game's own workers.
		const unsigned hw = std::thread::hardware_concurrency();
		pool = std::make_unique<BS::thread_pool<>>(std::clamp(hw / 4u, 1u, 4u));
	}

	if (landWorldSpace == worldSpace)
		return;

	// Worldspace changed, so bump the generation to drop in-flight worker results on drain, then clear.
	generation.fetch_add(1, std::memory_order_relaxed);
	worldSpace = landWorldSpace;
	files = landWorldSpace ? landWorldSpace->sourceFiles.array : nullptr;
	ready.clear();
	lastTouched.clear();
	pending.clear();
	{
		std::scoped_lock lock(completedMutex);
		completed.clear();
	}
}

void GrassCellCache::DrainCompleted()
{
	std::vector<std::tuple<uint64_t, uint64_t, std::unique_ptr<CellGrass>>> drained;
	{
		std::scoped_lock lock(completedMutex);
		drained.swap(completed);
	}

	const uint64_t gen = generation.load(std::memory_order_relaxed);
	for (auto& [key, taskGen, data] : drained) {
		pending.erase(key);
		if (taskGen != gen)
			continue;  // read belongs to a previous worldspace
		lastTouched[key] = frame;
		ready[key] = std::move(data);
	}
}

const CellGrass* GrassCellCache::GetOrRequest(int32_t cellX, int32_t cellY)
{
	const uint64_t key = Key(cellX, cellY);

	if (const auto it = ready.find(key); it != ready.end()) {
		lastTouched[key] = frame;
		return it->second.get();
	}

	if (!worldSpace || !files || !pool || pending.contains(key))
		return nullptr;

	pending.insert(key);
	const uint64_t gen = generation.load(std::memory_order_relaxed);
	RE::TESWorldSpace* ws = worldSpace;
	RE::TESFileArray* fileArray = files;

	pool->detach_task([this, key, cellX, cellY, ws, fileArray, gen] {
		auto data = ReadCell(ws, fileArray, cellX, cellY);
		std::scoped_lock lock(completedMutex);
		completed.emplace_back(key, gen, std::move(data));
	});

	return nullptr;
}

void GrassCellCache::EvictUntouched()
{
	constexpr size_t kMaxCachedCells = 2048;
	if (ready.size() <= kMaxCachedCells)
		return;

	std::vector<std::pair<uint64_t, uint64_t>> byAge;  // (last-touched frame, key)
	byAge.reserve(ready.size());
	for (const auto& kv : ready) {
		const auto it = lastTouched.find(kv.first);
		byAge.emplace_back(it != lastTouched.end() ? it->second : 0, kv.first);
	}
	std::ranges::sort(byAge);  // oldest first

	const size_t toRemove = ready.size() - kMaxCachedCells;
	for (size_t i = 0; i < toRemove; ++i) {
		if (byAge[i].first == frame)
			break;  // never evict a cell requested this frame - its pointers are live in quadrantsFarLOD

		ready.erase(byAge[i].second);
		lastTouched.erase(byAge[i].second);
	}
}

void GrassCellCache::Shutdown()
{
	if (pool) {
		pool->wait();
		pool.reset();
	}

	ready.clear();
	lastTouched.clear();
	pending.clear();
	std::scoped_lock lock(completedMutex);
	completed.clear();
}

std::unique_ptr<CellGrass> GrassCellCache::ReadCell(RE::TESWorldSpace* worldSpace, RE::TESFileArray* files, int32_t cellX, int32_t cellY)
{
	auto cell = std::make_unique<CellGrass>();
	for (auto& quad : cell->heights)
		quad.fill(PGrassCommon::QuadrantNoHeight);
	cell->minHeights.fill(PGrassCommon::QuadrantNoHeight);
	cell->maxHeights.fill(PGrassCommon::QuadrantNoHeight);

	if (!files)
		return cell;

	const int32_t fileCount = static_cast<int32_t>(files->size());
	auto* const fileData = files->data();

	// Highest load order wins, so search files back to front for the one that overrides this cell's LAND.
	for (int32_t i = fileCount - 1; i >= 0; --i) {
		RE::TESFile* file = fileData[i]->Duplicate();
		if (file && file->SeekCell(worldSpace, cellX, cellY) && file->SeekLandscapeForCurrentCell()) {
			ParseLandscape(file, *cell);
			break;
		}
	}

	return cell;
}

void GrassCellCache::ParseLandscape(RE::TESFile* file, CellGrass& out)
{
	const bool bigEndian = file->isBigEndian;

	using TextureGrid = std::array<RE::TESLandTexture*, kQuadrantSamples>;
	using OpacityGrid = std::array<float, kQuadrantSamples>;
	std::array<RE::TESLandTexture*, 4> baseTexture{};
	std::array<TextureGrid, 4> layerWinner{};
	std::array<OpacityGrid, 4> bestOpacity{};
	std::array<OpacityGrid, 4> totalOpacity{};
	const auto defaultLandTexture = PGrassCommon::GetDefaultLandTexture();

	int32_t activeQuadrant = -1;
	RE::TESLandTexture* activeLayerTexture = nullptr;

	while (file->SeekNextSubrecord()) {
		const uint32_t recordType = file->GetCurrentSubRecordType();
		const uint32_t recordSize = file->GetCurrentSubRecordSize();

		if (recordType == kVHGT) {
			if (recordSize < 4 + kCellVertexPitch * kCellVertexPitch + 3)
				continue;

			struct VHGT
			{
				float offset;
				int8_t deltas[kCellVertexPitch * kCellVertexPitch];
				uint8_t pad[3];
			} data{};

			file->ReadData(&data, sizeof(VHGT));

			// VHGT stores a row-start delta followed by deltas across that row. Decode it to world units first.
			float accumulatedRowHeight = bigEndian ? SwapF(data.offset) : data.offset;
			float cellHeights[kCellVertexPitch * kCellVertexPitch];

			for (uint32_t y = 0; y < kCellVertexPitch; ++y) {
				accumulatedRowHeight += static_cast<float>(data.deltas[y * kCellVertexPitch]);
				float accumulatedHeight = accumulatedRowHeight;

				for (uint32_t x = 0; x < kCellVertexPitch; ++x) {
					if (x != 0)
						accumulatedHeight += static_cast<float>(data.deltas[y * kCellVertexPitch + x]);

					cellHeights[y * kCellVertexPitch + x] = accumulatedHeight * 8.0f;
				}
			}

			// Split the 33x33 cell grid into four 17x17 quadrants that share the middle row and column.
			for (uint32_t qy = 0; qy < 2; ++qy) {
				for (uint32_t qx = 0; qx < 2; ++qx) {
					const uint32_t quad = qy * 2 + qx;
					for (uint32_t localY = 0; localY < kQuadrantPitch; ++localY) {
						for (uint32_t localX = 0; localX < kQuadrantPitch; ++localX) {
							const uint32_t cellX = qx * (kQuadrantPitch - 1) + localX;
							const uint32_t cellY = qy * (kQuadrantPitch - 1) + localY;
							out.heights[quad][localY * kQuadrantPitch + localX] = cellHeights[cellY * kCellVertexPitch + cellX];
						}
					}
				}
			}

		} else if (recordType == kBTXT) {
			if (recordSize < sizeof(LandscapeTextureHeader))
				continue;

			LandscapeTextureHeader header{};
			file->ReadData(&header, sizeof(header));

			if (header.quadrant < 4)
				baseTexture[header.quadrant] = ResolveLandTexture(file, bigEndian ? Swap32(header.landTexture) : header.landTexture);

		} else if (recordType == kATXT) {
			if (recordSize < sizeof(LandscapeTextureHeader))
				continue;

			LandscapeTextureHeader header{};
			file->ReadData(&header, sizeof(header));
			const int16_t layer = bigEndian ? static_cast<int16_t>(Swap16(static_cast<uint16_t>(header.layer))) : header.layer;
			const bool validLayer = header.quadrant < 4 && layer >= 0 && layer < 5;
			activeQuadrant = validLayer ? header.quadrant : -1;
			activeLayerTexture = validLayer ? ResolveLandTexture(file, bigEndian ? Swap32(header.landTexture) : header.landTexture) : nullptr;

		} else if (recordType == kVTXT) {
			if (activeQuadrant < 0 || !activeLayerTexture || recordSize < sizeof(VertexTextureAlpha))
				continue;

			const uint32_t pointCount = recordSize / sizeof(VertexTextureAlpha);
			std::vector<VertexTextureAlpha> points(pointCount);
			file->ReadData(points.data(), pointCount * sizeof(VertexTextureAlpha));

			for (const VertexTextureAlpha& point : points) {
				const uint16_t position = bigEndian ? Swap16(point.position) : point.position;
				const float opacity = bigEndian ? SwapF(point.opacity) : point.opacity;

				if (position >= kQuadrantSamples)
					continue;

				totalOpacity[activeQuadrant][position] += std::clamp(opacity, 0.0f, 1.0f);
				if (opacity > bestOpacity[activeQuadrant][position]) {
					bestOpacity[activeQuadrant][position] = opacity;
					layerWinner[activeQuadrant][position] = activeLayerTexture;
				}
			}
		}
	}

	// A land texture grows grass when its grass list is non-empty, and the winning texture per vertex decides.
	for (uint32_t quad = 0; quad < 4; ++quad) {
		for (uint32_t v = 0; v < kQuadrantSamples; ++v) {
			const float baseOpacity = std::max(1.0f - totalOpacity[quad][v], 0.0f);
			RE::TESLandTexture* winner = bestOpacity[quad][v] > baseOpacity ? layerWinner[quad][v] : baseTexture[quad];
			if (!winner)
				winner = defaultLandTexture;
			out.ids[quad][v] = (winner && !winner->textureGrassList.empty()) ? 1u : 0u;
		}

		const auto [minIt, maxIt] = std::minmax_element(out.heights[quad].begin(), out.heights[quad].end());
		if (*maxIt > PGrassCommon::QuadrantNoHeight) {
			out.minHeights[quad] = *minIt;
			out.maxHeights[quad] = *maxIt;
		}
	}
}
