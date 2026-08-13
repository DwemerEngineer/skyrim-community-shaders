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

	constexpr uint32_t kPitch = PGrassCommon::QuadrantGrassPitch;   // 17
	constexpr uint32_t kSamples = PGrassCommon::QuadrantGrassSamples;  // 289
	constexpr uint32_t kCellVerts = 33;

	// Swaps only fire on the rare big-endian file, matching the water cache.
	uint32_t Swap32(uint32_t v) { return _byteswap_ulong(v); }
	uint16_t Swap16(uint16_t v) { return _byteswap_ushort(v); }
	float SwapF(float v) { return std::bit_cast<float>(_byteswap_ulong(std::bit_cast<uint32_t>(v))); }

	// BTXT/ATXT share this 8-byte header of land texture, quadrant, and layer.
	struct TextureHeader
	{
		RE::FormID landTexture;
		uint8_t quadrant;
		uint8_t unused;
		int16_t layer;
	};
	static_assert(sizeof(TextureHeader) == 8);

	// VTXT alpha entry of a quadrant-local vertex index and its blend opacity.
	struct AlphaPoint
	{
		uint16_t position;
		uint8_t unused[2];
		float opacity;
	};
	static_assert(sizeof(AlphaPoint) == 8);

	RE::TESLandTexture* ResolveLandTexture(RE::TESFile* file, RE::FormID rawFormID)
	{
		if (!rawFormID)
			return nullptr;
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

	// Per-quadrant base texture, and per vertex the highest-opacity layer that beat it. 
	// A vertex with no layer alpha keeps the base, as the loaded path's winner search does.
	RE::TESLandTexture* baseTexture[4] = { nullptr, nullptr, nullptr, nullptr };
	std::array<std::array<RE::TESLandTexture*, kSamples>, 4> layerWinner{};
	std::array<std::array<float, kSamples>, 4> bestOpacity{};

	int32_t currentQuad = -1;
	RE::TESLandTexture* currentLayerTexture = nullptr;

	while (file->SeekNextSubrecord()) {
		const uint32_t type = file->GetCurrentSubRecordType();
		const uint32_t size = file->GetCurrentSubRecordSize();

		if (type == kVHGT) {
			if (size < 4 + kCellVerts * kCellVerts + 3)
				continue;
			struct VHGT
			{
				float offset;
				int8_t deltas[kCellVerts * kCellVerts];
				uint8_t pad[3];
			} data{};
			file->ReadData(&data, sizeof(VHGT));

			float rowBase = bigEndian ? SwapF(data.offset) : data.offset;
			float decoded[kCellVerts * kCellVerts];
			for (uint32_t y = 0; y < kCellVerts; ++y) {
				rowBase += static_cast<float>(data.deltas[y * kCellVerts]);
				float height = rowBase;
				for (uint32_t x = 0; x < kCellVerts; ++x) {
					if (x != 0)
						height += static_cast<float>(data.deltas[y * kCellVerts + x]);
					decoded[y * kCellVerts + x] = height * 8.0f;
				}
			}

			// Split the 33x33 cell grid into four 17x17 quadrants that share the middle row and column.
			for (uint32_t qy = 0; qy < 2; ++qy) {
				for (uint32_t qx = 0; qx < 2; ++qx) {
					const uint32_t quad = qy * 2 + qx;
					for (uint32_t ly = 0; ly < kPitch; ++ly) {
						for (uint32_t lx = 0; lx < kPitch; ++lx) {
							const uint32_t gx = qx * (kPitch - 1) + lx;
							const uint32_t gy = qy * (kPitch - 1) + ly;
							out.heights[quad][ly * kPitch + lx] = decoded[gy * kCellVerts + gx];
						}
					}
				}
			}
		} else if (type == kBTXT) {
			if (size < sizeof(TextureHeader))
				continue;
			TextureHeader header{};
			file->ReadData(&header, sizeof(header));
			if (header.quadrant < 4)
				baseTexture[header.quadrant] = ResolveLandTexture(file, bigEndian ? Swap32(header.landTexture) : header.landTexture);
		} else if (type == kATXT) {
			if (size < sizeof(TextureHeader))
				continue;
			TextureHeader header{};
			file->ReadData(&header, sizeof(header));
			currentQuad = header.quadrant < 4 ? header.quadrant : -1;
			currentLayerTexture = ResolveLandTexture(file, bigEndian ? Swap32(header.landTexture) : header.landTexture);
		} else if (type == kVTXT) {
			// Alpha data always follows its ATXT header, so apply it to that layer.
			if (currentQuad < 0 || !currentLayerTexture || size < sizeof(AlphaPoint))
				continue;
			const uint32_t count = size / sizeof(AlphaPoint);
			std::vector<AlphaPoint> points(count);
			file->ReadData(points.data(), count * sizeof(AlphaPoint));
			for (const auto& point : points) {
				const uint16_t position = bigEndian ? Swap16(point.position) : point.position;
				const float opacity = bigEndian ? SwapF(point.opacity) : point.opacity;
				if (position < kSamples && opacity > bestOpacity[currentQuad][position]) {
					bestOpacity[currentQuad][position] = opacity;
					layerWinner[currentQuad][position] = currentLayerTexture;
				}
			}
		}
	}

	// A land texture grows grass when its grass list is non-empty, and the winning texture per vertex decides.
	for (uint32_t quad = 0; quad < 4; ++quad) {
		for (uint32_t v = 0; v < kSamples; ++v) {
			RE::TESLandTexture* winner = bestOpacity[quad][v] > 0.0f ? layerWinner[quad][v] : baseTexture[quad];
			out.ids[quad][v] = (winner && !winner->textureGrassList.empty()) ? 1u : 0u;
		}
	}
}
