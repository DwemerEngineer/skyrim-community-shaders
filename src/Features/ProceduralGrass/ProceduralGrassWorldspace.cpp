#include "Features/ProceduralGrass.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace
{

	/** @brief Floor division by 2, so negative cell coordinates map to the correct quadrant. */
	constexpr int32_t FloorDiv2(int32_t a)
	{
		return a >= 0 ? a / 2 : -((-a + 1) / 2);
	}

	/** @brief Stable 32-bit hash of a grass-map sample's identity, for deterministic per-sample type selection. */
	uint32_t QuadrantSampleHash(int32_t cellX, int32_t cellY, uint32_t quadIndex, uint32_t sample)
	{
		constexpr uint32_t Fnv1aOffsetBasis = 2166136261u;
		const auto mix = [](uint32_t hash, uint32_t value) {
			constexpr uint32_t Fnv1aPrime = 16777619u;
			for (uint32_t shift = 0; shift < 32; shift += 8) {
				hash ^= (value >> shift) & 0xFFu;
				hash *= Fnv1aPrime;
			}
			return hash;
		};

		uint32_t h = Fnv1aOffsetBasis;
		h = mix(h, static_cast<uint32_t>(cellX));
		h = mix(h, static_cast<uint32_t>(cellY));
		h = mix(h, quadIndex);
		h = mix(h, sample);
		return h;
	}

}

uint64_t ProceduralGrass::QuadrantKey(int32_t cellX, int32_t cellY, uint32_t quadIndex)
{
	return (static_cast<uint64_t>(static_cast<uint16_t>(cellX)) << 18) |
	       (static_cast<uint64_t>(static_cast<uint16_t>(cellY)) << 2) |
	       quadIndex;
}

const ProceduralGrass::QuadrantGrass& ProceduralGrass::GetQuadrantCache(RE::TESObjectLAND* land, uint32_t quadIndex, int32_t cellX, int32_t cellY)
{
	const uint64_t key = QuadrantKey(cellX, cellY, quadIndex);

	auto& entry = grassMapCache[key];
	entry.lastSeenFrame = grassMapFrame;

	if (entry.land == land)
		return entry;

	entry.land = land;
	grassContentGeneration++;  // Rebuilt in place, so force a renderer re-upload.

	const auto landData = land->loadedData;

	// Convert possible record-relative heights with one four-quadrant anchor to avoid seams.
	float rawMin = std::numeric_limits<float>::max();
	for (uint32_t q = 0; q < 4; q++)
		rawMin = std::min(rawMin, *std::min_element(landData->heights[q], landData->heights[q] + PGrassCommon::QuadrantGrassSamples));

	const float anchor = landData->heightExtents.x - rawMin;
	entry.minHeight = (std::numeric_limits<float>::max)();
	entry.maxHeight = (std::numeric_limits<float>::lowest)();

	for (uint32_t v = 0; v < PGrassCommon::QuadrantGrassSamples; v++) {
		const float height = landData->heights[quadIndex][v] + anchor;
		entry.heights[v] = height;
		entry.minHeight = std::min(entry.minHeight, height);
		entry.maxHeight = std::max(entry.maxHeight, height);
	}

	landHeightDebug = {
		.rawFirst = landData->heights[quadIndex][0],
		.rawMin = rawMin,
		.extents = float2(landData->heightExtents.x, landData->heightExtents.y),
		.anchor = anchor,
		.meshWorldZ = landData->mesh[quadIndex] ? landData->mesh[quadIndex]->world.translate.z : 0.0f,
	};

	if (settings.debugIgnoreGrassMap) {
		entry.ids.fill(1u);
		return entry;
	}

	// Cache the resolved selection for repeated winning textures. Null selects the base type.
	const RE::TESLandTexture* cachedWinner = nullptr;
	const TextureSelection* cachedSel = nullptr;

	for (uint32_t v = 0; v < PGrassCommon::QuadrantGrassSamples; v++) {
		const RE::TESLandTexture* winner = landData->defQuadTextures[quadIndex];
		int32_t bestPercent = 0;

		for (uint32_t layer = 0; layer < 6; layer++) {
			// Stored as int8_t but represents unsigned opacity.
			const int32_t percent = static_cast<uint8_t>(landData->percents[quadIndex][v][layer]);
			if (percent > bestPercent) {
				bestPercent = percent;
				winner = landData->quadTextures[quadIndex][layer];
			}
		}

		if (!winner || winner->textureGrassList.empty()) {
			entry.ids[v] = 0u;  // The dominant texture has no grass.
			continue;
		}

		if (winner != cachedWinner) {
			cachedWinner = winner;
			if (const auto cached = textureSelectionByTexture.find(winner); cached != textureSelectionByTexture.end()) {
				cachedSel = cached->second;
			} else {
				const auto it = textureSelection.find(LandTextureKey(winner));
				cachedSel = it != textureSelection.end() ? &it->second : nullptr;
				textureSelectionByTexture.emplace(winner, cachedSel);
			}
		}

		// Use stable weighted selection so cache rebuilds preserve the grass mix.
		uint32_t type = 1;  // base/default type for a texture with no variants
		if (cachedSel && cachedSel->total > 0.0f) {
			const float r = (QuadrantSampleHash(cellX, cellY, quadIndex, v) * (1.0f / 4294967296.0f)) * cachedSel->total;
			type = cachedSel->ids.back();
			for (size_t i = 0; i < cachedSel->ids.size(); i++) {
				if (r < cachedSel->cumulative[i]) {
					type = cachedSel->ids[i];
					break;
				}
			}
		}

		entry.ids[v] = static_cast<uint8_t>(type);
	}

	return entry;
}

std::optional<float> ProceduralGrass::GetLandHeightAt(const float worldX, const float worldY) const
{
	const int32_t quadX = static_cast<int32_t>(std::floor(worldX / 2048.0f));
	const int32_t quadY = static_cast<int32_t>(std::floor(worldY / 2048.0f));
	const int32_t cellX = FloorDiv2(quadX);
	const int32_t cellY = FloorDiv2(quadY);
	const uint32_t quadIndex = static_cast<uint32_t>(quadY - cellY * 2) * 2 + static_cast<uint32_t>(quadX - cellX * 2);

	const auto entry = grassMapCache.find(QuadrantKey(cellX, cellY, quadIndex));
	if (entry == grassMapCache.end() || entry->second.heights[0] <= PGrassCommon::QuadrantNoHeight)
		return std::nullopt;

	const float localX = worldX - quadX * 2048.0f;
	const float localY = worldY - quadY * 2048.0f;
	const float spacing = 2048.0f / (PGrassCommon::QuadrantGrassPitch - 1);
	const float maxSample = PGrassCommon::QuadrantGrassPitch - 1.001f;
	const float sampleX = std::clamp(localX / spacing, 0.0f, maxSample);
	const float sampleY = std::clamp(localY / spacing, 0.0f, maxSample);

	const auto baseX = static_cast<uint32_t>(sampleX);
	const auto baseY = static_cast<uint32_t>(sampleY);
	const float fracX = sampleX - baseX;
	const float fracY = sampleY - baseY;

	const auto& h = entry->second.heights;
	const uint32_t i = baseY * PGrassCommon::QuadrantGrassPitch + baseX;

	return std::lerp(
		std::lerp(h[i], h[i + 1], fracX),
		std::lerp(h[i + PGrassCommon::QuadrantGrassPitch], h[i + PGrassCommon::QuadrantGrassPitch + 1], fracX),
		fracY);
}

void ProceduralGrass::GetVisibleQuadrants()
{
	globals::profiler->BeginPass("ProceduralGrass::Visible Quadrants");

	grassMapFrame++;

	quadrantsHighLOD.clear();
	quadrantsMidLOD.clear();
	quadrantsLowLOD.clear();
	quadrantsFarLOD.clear();
	quadrantsPresence.clear();

	const auto tes = globals::game::tes;

	RE::TESWorldSpace* landWorldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	while (landWorldSpace && landWorldSpace->parentWorld && landWorldSpace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
		landWorldSpace = landWorldSpace->parentWorld;

	grassCellCache.BeginFrame(landWorldSpace);
	grassCellCache.DrainCompleted();

	const auto& playerNiPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	const auto& playerPos = reinterpret_cast<float3 const&>(playerNiPos);
	const int playerQuadrantX = static_cast<int>(std::floor(playerPos.x / 2048.0f));
	const int playerQuadrantY = static_cast<int>(std::floor(playerPos.y / 2048.0f));
	const int32_t playerCellX = static_cast<int32_t>(std::floor(playerPos.x / 4096.0f));
	const int32_t playerCellY = static_cast<int32_t>(std::floor(playerPos.y / 4096.0f));

	// Centre the terrain-darkening grass-id window on the player.
	const int32_t presenceOriginQuadX = playerQuadrantX - PGrassCommon::LowTierQuadrantRadius;
	const int32_t presenceOriginQuadY = playerQuadrantY - PGrassCommon::LowTierQuadrantRadius;
	grassPresenceOrigin = float2{ presenceOriginQuadX * 2048.0f, presenceOriginQuadY * 2048.0f };

	quadrantReject = {};

	const auto cells = tes ? tes->gridCells : nullptr;
	auto quadrant = PGrassCommon::Quadrant{};
	const auto cellCount = cells ? cells->length * cells->length : 0u;

	for (uint32_t i = 0; i < cellCount; i++) {
		if (const auto cell = cells->cells[i]) {
			quadrantReject.cells++;

			const auto& runtimeData = cell->GetRuntimeData();
			if (!runtimeData.cellData.exterior)
				continue;

			quadrantReject.withExterior++;
			quadrant.cellX = runtimeData.cellData.exterior->cellX;
			quadrant.cellY = runtimeData.cellData.exterior->cellY;

			const auto land = runtimeData.cellLand;
			if (land)
				quadrantReject.withLand++;

			if (land && land->loadedData) {
				quadrantReject.withLoadedData++;
				for (uint32_t j = 0; j < 4; j++) {
					if (const auto mesh = land->loadedData->mesh[j]) {
						quadrantReject.withMesh++;
						if (settings.debugIgnorePreProcessedFlag || mesh->GetFlags().all(RE::NiAVObject::Flag::kPreProcessedNode)) {

							quadrantReject.preProcessed++;
							quadrant.x = j % 2;
							quadrant.y = j / 2;

							const auto& quadrantCache = GetQuadrantCache(land, j, quadrant.cellX, quadrant.cellY);
							quadrant.grassIds = quadrantCache.ids.data();
							quadrant.heights = quadrantCache.heights.data();
							quadrant.worldPos = float2{ (quadrant.cellX + quadrant.x * 0.5f) * 4096.0f, (quadrant.cellY + quadrant.y * 0.5f) * 4096.0f };
							quadrant.minHeight = quadrantCache.minHeight;
							quadrant.maxHeight = quadrantCache.maxHeight;

							const int32_t worldQuadrantX = quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x);
							const int32_t worldQuadrantY = quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y);
							const int32_t xDiff = abs(playerQuadrantX - worldQuadrantX);
							const int32_t yDiff = abs(playerQuadrantY - worldQuadrantY);

							// Overlap max-distance bands so adjacent tiers cross-fade.
							const int32_t md = std::max(xDiff, yDiff);

							if (md <= PGrassCommon::LowTierQuadrantRadius)
								quadrantsPresence.push_back(quadrant);
							if (md <= PGrassCommon::HighTierQuadrantRadius && quadrantsHighLOD.size() < PGrassCommon::HighTierQuadrantCap)
								quadrantsHighLOD.push_back(quadrant);
							if (md >= PGrassCommon::HighTierQuadrantRadius - 1 && md <= PGrassCommon::MidTierQuadrantRadius && quadrantsMidLOD.size() < PGrassCommon::MidTierQuadrantCap)
								quadrantsMidLOD.push_back(quadrant);
							if (md >= PGrassCommon::MidTierQuadrantRadius - 1 && md <= PGrassCommon::LowTierQuadrantRadius && quadrantsLowLOD.size() < PGrassCommon::LowTierQuadrantCap)
								quadrantsLowLOD.push_back(quadrant);
						}
					}
				}
			}
		}
	}

	// Evict quadrants outside the loaded grid. Node-based storage keeps active pointers valid.
	if (std::erase_if(grassMapCache, [this](const auto& kv) { return kv.second.lastSeenFrame != grassMapFrame; }) != 0)
		grassContentGeneration++;

	// Rebuild the presence texture only when its window or cached LAND data changes.
	if (grassPresenceOriginQuadX != presenceOriginQuadX || grassPresenceOriginQuadY != presenceOriginQuadY || grassPresenceContentGeneration != grassContentGeneration) {
		std::fill(grassPresenceStaging.begin(), grassPresenceStaging.end(), uint8_t{ 0 });

		for (const auto& presenceQuadrant : quadrantsPresence) {

			if (!presenceQuadrant.grassIds)
				continue;

			const int32_t worldQuadrantX = presenceQuadrant.cellX * 2 + static_cast<int32_t>(presenceQuadrant.x);
			const int32_t worldQuadrantY = presenceQuadrant.cellY * 2 + static_cast<int32_t>(presenceQuadrant.y);
			const int32_t sx0 = (worldQuadrantX - presenceOriginQuadX) * (PGrassCommon::QuadrantGrassPitch - 1);
			const int32_t sy0 = (worldQuadrantY - presenceOriginQuadY) * (PGrassCommon::QuadrantGrassPitch - 1);

			if (sx0 < 0 || sy0 < 0 || sx0 + static_cast<int32_t>(PGrassCommon::QuadrantGrassPitch) > static_cast<int32_t>(grassPresenceDim) || sy0 + static_cast<int32_t>(PGrassCommon::QuadrantGrassPitch) > static_cast<int32_t>(grassPresenceDim))
				continue;

			for (uint32_t row = 0; row < PGrassCommon::QuadrantGrassPitch; ++row) {
				uint8_t* dstRow = grassPresenceStaging.data() + static_cast<size_t>(sy0 + row) * grassPresenceDim + sx0;
				std::memcpy(dstRow, presenceQuadrant.grassIds + row * PGrassCommon::QuadrantGrassPitch, PGrassCommon::QuadrantGrassPitch);
			}
		}

		grassPresenceOriginQuadX = presenceOriginQuadX;
		grassPresenceOriginQuadY = presenceOriginQuadY;
		grassPresenceContentGeneration = grassContentGeneration;
		grassPresenceUploadDirty = true;

	}

	// Far streams LAND data on workers and skips quadrants already drawn by the near tiers.
	if (landWorldSpace) {

		const int32_t gridLen = cells ? static_cast<int32_t>(cells->length) : 0;
		const int32_t gridOffsetX = tes->currentGridX - gridLen / 2;
		const int32_t gridOffsetY = tes->currentGridY - gridLen / 2;

		const int32_t radius = std::clamp(settings.grassCellRadius, 0, 15);

		for (int32_t cy = playerCellY - radius; cy <= playerCellY + radius; ++cy) {
			for (int32_t cx = playerCellX - radius; cx <= playerCellX + radius; ++cx) {

				const int32_t gx = cx - gridOffsetX;
				const int32_t gy = cy - gridOffsetY;
				const bool inLoadedGrid = gx >= 0 && gy >= 0 && gx < gridLen && gy < gridLen;

				const CellGrass* cellGrass = grassCellCache.GetOrRequest(cx, cy);
				if (!cellGrass)
					continue; 

				for (uint32_t j = 0; j < 4 && quadrantsFarLOD.size() < PGrassCommon::FarQuadrantCount; ++j) {

					const int32_t worldQuadrantX = cx * 2 + static_cast<int32_t>(j % 2);
					const int32_t worldQuadrantY = cy * 2 + static_cast<int32_t>(j / 2);
					const int32_t md = std::max(std::abs(playerQuadrantX - worldQuadrantX), std::abs(playerQuadrantY - worldQuadrantY));

					// Loaded quadrants inside Low's range belong to the near tiers. Far draws the rest.
					if (inLoadedGrid && md <= PGrassCommon::LowTierQuadrantRadius)
						continue;

					quadrant.cellX = cx;
					quadrant.cellY = cy;
					quadrant.x = j % 2;
					quadrant.y = j / 2;
					quadrant.grassIds = cellGrass->ids[j].data();
					quadrant.heights = cellGrass->heights[j].data();
					quadrant.worldPos = float2{ worldQuadrantX * 2048.0f, worldQuadrantY * 2048.0f };
					quadrant.minHeight = cellGrass->minHeights[j];
					quadrant.maxHeight = cellGrass->maxHeights[j];

					quadrantsFarLOD.push_back(quadrant);
				}
			}
		}
	}

	grassCellCache.EvictUntouched();

	globals::profiler->EndPass();
}
