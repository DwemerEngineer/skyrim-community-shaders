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

RE::TESLandTexture* PGrassCommon::GetDefaultLandTexture()
{
	static const auto defaultLandTextureAddress = REL::Relocation<RE::TESLandTexture**>(RELOCATION_ID(514783, 400936));
	return *defaultLandTextureAddress;
}

const ProceduralGrass::LoadedCellGrass& ProceduralGrass::GetCellCache(RE::TESObjectLAND* land, int32_t cellX, int32_t cellY, uint32_t debugQuadIndex)
{
	auto& cell = grassMapCache[PGrassCommon::GrassCellKey(cellX, cellY)];
	cell.lastSeenFrame = grassMapFrame;

	if (cell.land == land)
		return cell;

	cell.land = land;

	const auto landData = land->loadedData;

	// Convert possible record-relative heights with one four-quadrant anchor to avoid seams.
	float rawMin = std::numeric_limits<float>::max();
	for (uint32_t q = 0; q < 4; q++)
		rawMin = std::min(rawMin, *std::min_element(landData->heights[q], landData->heights[q] + PGrassCommon::QuadrantGrassSamples));

	const float anchor = landData->heightExtents.x - rawMin;

	debugQuadIndex = std::min(debugQuadIndex, 3u);
	landHeightDebug = {
		.rawFirst = landData->heights[debugQuadIndex][0],
		.rawMin = rawMin,
		.extents = float2(landData->heightExtents.x, landData->heightExtents.y),
		.anchor = anchor,
		.meshWorldZ = landData->mesh[debugQuadIndex] ? landData->mesh[debugQuadIndex]->world.translate.z : 0.0f,
	};

	const RE::TESLandTexture* defaultLandTexture = PGrassCommon::GetDefaultLandTexture();

	for (uint32_t quadIndex = 0; quadIndex < 4; ++quadIndex) {
		auto& quadrant = cell.quadrants[quadIndex];
		quadrant.cacheVersion = nextGrassCacheVersion++;
		quadrant.minHeight = (std::numeric_limits<float>::max)();
		quadrant.maxHeight = (std::numeric_limits<float>::lowest)();

		for (uint32_t v = 0; v < PGrassCommon::QuadrantGrassSamples; ++v) {
			const float height = landData->heights[quadIndex][v] + anchor;
			quadrant.heights[v] = height;
			quadrant.minHeight = std::min(quadrant.minHeight, height);
			quadrant.maxHeight = std::max(quadrant.maxHeight, height);
		}

		if (settings.debugIgnoreGrassMap) {
			quadrant.ids.fill(1u);
			continue;
		}

		// Reuse the last texture selection within the quadrant.
		const RE::TESLandTexture* cachedWinner = nullptr;
		const TextureSelection* cachedSelection = nullptr;

		for (uint32_t v = 0; v < PGrassCommon::QuadrantGrassSamples; ++v) {
			int32_t overlayTotal = 0;
			for (uint32_t layer = 0; layer < 5; ++layer) {
				const auto texture = landData->quadTextures[quadIndex][layer];
				if (texture && (texture->formID != 0 || defaultLandTexture))
					overlayTotal += static_cast<uint8_t>(landData->percents[quadIndex][v][layer]);
			}

			const auto baseTexture = landData->defQuadTextures[quadIndex];
			const RE::TESLandTexture* winner = baseTexture && baseTexture->formID != 0 ? baseTexture : defaultLandTexture;
			int32_t bestPercent = std::max(255 - overlayTotal, 0);

			for (uint32_t layer = 0; layer < 5; ++layer) {
				// Stored as int8_t but represents unsigned opacity.
				const int32_t percent = static_cast<uint8_t>(landData->percents[quadIndex][v][layer]);
				const auto texture = landData->quadTextures[quadIndex][layer];
				const auto effectiveTexture = texture && texture->formID == 0 ? defaultLandTexture : texture;
				if (effectiveTexture && percent > bestPercent) {
					bestPercent = percent;
					winner = effectiveTexture;
				}
			}

			if (!winner) {
				quadrant.ids[v] = 0u;
				continue;
			}

			if (winner != cachedWinner) {
				cachedWinner = winner;
				if (const auto cached = textureSelectionByTexture.find(winner); cached != textureSelectionByTexture.end()) {
					cachedSelection = cached->second;
				} else {
					const auto selection = textureSelection.find(LandTextureKey(winner));
					cachedSelection = selection != textureSelection.end() ? &selection->second : nullptr;
					textureSelectionByTexture.emplace(winner, cachedSelection);
				}
			}

			// Select configured variants deterministically; otherwise preserve vanilla behavior.
			uint32_t type = winner->textureGrassList.empty() ? 0u : 1u;
			if (cachedSelection && cachedSelection->total > 0.0f) {
				const float r = (QuadrantSampleHash(cellX, cellY, quadIndex, v) * (1.0f / 4294967296.0f)) * cachedSelection->total;
				type = cachedSelection->ids.back();
				for (size_t i = 0; i < cachedSelection->ids.size(); ++i) {
					if (r < cachedSelection->cumulative[i]) {
						type = cachedSelection->ids[i];
						break;
					}
				}
			}

			quadrant.ids[v] = static_cast<uint8_t>(type);
		}
	}

	return cell;
}

void ProceduralGrass::EvictGrassMapCache()
{
	if (grassMapCache.size() <= grassMapCacheCapacity)
		return;

	std::vector<std::pair<uint64_t, uint64_t>> byAge;
	byAge.reserve(grassMapCache.size());
	for (const auto& [key, cell] : grassMapCache)
		byAge.emplace_back(cell.lastSeenFrame, key);
	std::ranges::sort(byAge);

	const size_t removeCount = grassMapCache.size() - grassMapCacheCapacity;
	for (size_t i = 0; i < removeCount; ++i) {
		if (byAge[i].first == grassMapFrame)
			break;
		grassMapCache.erase(byAge[i].second);
	}
}

std::optional<float> ProceduralGrass::GetLandHeightAt(const float worldX, const float worldY) const
{
	const int32_t quadX = static_cast<int32_t>(std::floor(worldX / 2048.0f));
	const int32_t quadY = static_cast<int32_t>(std::floor(worldY / 2048.0f));
	const int32_t cellX = FloorDiv2(quadX);
	const int32_t cellY = FloorDiv2(quadY);
	const uint32_t quadIndex = static_cast<uint32_t>(quadY - cellY * 2) * 2 + static_cast<uint32_t>(quadX - cellX * 2);

	const auto entry = grassMapCache.find(PGrassCommon::GrassCellKey(cellX, cellY));
	if (entry == grassMapCache.end())
		return std::nullopt;
	const auto& quadrant = entry->second.quadrants[quadIndex];
	if (quadrant.heights[0] <= PGrassCommon::QuadrantNoHeight)
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

	const auto& h = quadrant.heights;
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
	std::array<bool, PGrassCommon::LowTierQuadrantCap> nearCoveredQuadrants{};
	constexpr int32_t nearCoverageDiameter = PGrassCommon::LowTierQuadrantRadius * 2 + 1;

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
				const LoadedCellGrass* cellCache = nullptr;
				for (uint32_t j = 0; j < 4; j++) {
					if (const auto mesh = land->loadedData->mesh[j]) {
						quadrantReject.withMesh++;
						if (settings.debugIgnorePreProcessedFlag || mesh->GetFlags().all(RE::NiAVObject::Flag::kPreProcessedNode)) {

							quadrantReject.preProcessed++;
							quadrant.x = j % 2;
							quadrant.y = j / 2;
							quadrant.nearCovered = true;

							if (!cellCache)
								cellCache = &GetCellCache(land, quadrant.cellX, quadrant.cellY, j);
							const auto& quadrantCache = cellCache->quadrants[j];
							quadrant.cacheVersion = quadrantCache.cacheVersion;
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

							if (md <= PGrassCommon::LowTierQuadrantRadius) {
								// Record only near quadrants with accepted LAND geometry.
								const uint32_t coverageX = static_cast<uint32_t>(worldQuadrantX - playerQuadrantX + PGrassCommon::LowTierQuadrantRadius);
								const uint32_t coverageY = static_cast<uint32_t>(worldQuadrantY - playerQuadrantY + PGrassCommon::LowTierQuadrantRadius);
								nearCoveredQuadrants[coverageY * nearCoverageDiameter + coverageX] = true;
								quadrantsPresence.push_back(quadrant);
							}
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

	// Retain recently unloaded LAND cells across grid boundaries.
	EvictGrassMapCache();

	uint64_t presenceContentHash = PGrassCommon::GrassHashOffsetBasis;
	const size_t presenceCount = quadrantsPresence.size();
	PGrassCommon::GrassHashValue(presenceContentHash, presenceCount);
	for (const auto& presenceQuadrant : quadrantsPresence) {
		const uint64_t cellKey = PGrassCommon::GrassCellKey(presenceQuadrant.cellX, presenceQuadrant.cellY);
		const uint64_t quadrantKey = PGrassCommon::GrassQuadrantKey(presenceQuadrant.x, presenceQuadrant.y);
		PGrassCommon::GrassHashValue(presenceContentHash, cellKey);
		PGrassCommon::GrassHashValue(presenceContentHash, quadrantKey);
		PGrassCommon::GrassHashValue(presenceContentHash, presenceQuadrant.cacheVersion);
	}

	// Rebuild the presence texture only when its window or cached LAND data changes.
	if (grassPresenceOriginQuadX != presenceOriginQuadX || grassPresenceOriginQuadY != presenceOriginQuadY || grassPresenceContentHash != presenceContentHash) {
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

		// Match the generator's single-sample, non-propagating dilation.
		const auto sourcePresence = grassPresenceStaging;
		const int32_t worldSampleBaseX = presenceOriginQuadX * static_cast<int32_t>(PGrassCommon::QuadrantGrassPitch - 1);
		const int32_t worldSampleBaseY = presenceOriginQuadY * static_cast<int32_t>(PGrassCommon::QuadrantGrassPitch - 1);
		for (uint32_t y = 0; y < grassPresenceDim; ++y) {
			for (uint32_t x = 0; x < grassPresenceDim; ++x) {
				const size_t sample = static_cast<size_t>(y) * grassPresenceDim + x;
				if (sourcePresence[sample] == 0) {
					grassPresenceStaging[sample] = PGrassCommon::FindAdjacentGrassId(sourcePresence.data(), grassPresenceDim, grassPresenceDim, x, y,
						worldSampleBaseX + static_cast<int32_t>(x), worldSampleBaseY + static_cast<int32_t>(y));
				}
			}
		}

		grassPresenceOriginQuadX = presenceOriginQuadX;
		grassPresenceOriginQuadY = presenceOriginQuadY;
		grassPresenceContentHash = presenceContentHash;
		grassPresenceUploadDirty = true;

	}

	// Stream Far LAND data and yield to accepted near geometry.
	if (landWorldSpace) {
		const int32_t radius = std::clamp(settings.grassCellRadius, 0, 15);
		const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
		// Keep sparse Far fallback outside High's fully dense range.
		const float farFallbackStart = PGrassCommon::HighTierQuadrantRadius * 2048.0f;
		const float farFallbackStartSq = farFallbackStart * farFallbackStart;

		for (int32_t cy = playerCellY - radius; cy <= playerCellY + radius; ++cy) {
			for (int32_t cx = playerCellX - radius; cx <= playerCellX + radius; ++cx) {

				const CellGrass* cellGrass = grassCellCache.GetOrRequest(cx, cy);
				if (!cellGrass)
					continue; 

				for (uint32_t j = 0; j < 4 && quadrantsFarLOD.size() < PGrassCommon::FarQuadrantCount; ++j) {

					const int32_t worldQuadrantX = cx * 2 + static_cast<int32_t>(j % 2);
					const int32_t worldQuadrantY = cy * 2 + static_cast<int32_t>(j / 2);
					const int32_t md = std::max(std::abs(playerQuadrantX - worldQuadrantX), std::abs(playerQuadrantY - worldQuadrantY));
					bool nearCovered = false;
					if (md <= PGrassCommon::LowTierQuadrantRadius) {
						const uint32_t coverageX = static_cast<uint32_t>(worldQuadrantX - playerQuadrantX + PGrassCommon::LowTierQuadrantRadius);
						const uint32_t coverageY = static_cast<uint32_t>(worldQuadrantY - playerQuadrantY + PGrassCommon::LowTierQuadrantRadius);
						nearCovered = nearCoveredQuadrants[coverageY * nearCoverageDiameter + coverageX];
					}
					const float worldX = worldQuadrantX * 2048.0f;
					const float worldY = worldQuadrantY * 2048.0f;
					const float farthestX = std::max(std::abs(worldX - cameraPosAdjust.x), std::abs(worldX + 2048.0f - cameraPosAdjust.x));
					const float farthestY = std::max(std::abs(worldY - cameraPosAdjust.y), std::abs(worldY + 2048.0f - cameraPosAdjust.y));

					// Skip only actual near coverage whose entire area is inside the fallback boundary.
					if (nearCovered && farthestX * farthestX + farthestY * farthestY < farFallbackStartSq)
						continue;

					quadrant.cellX = cx;
					quadrant.cellY = cy;
					quadrant.x = j % 2;
					quadrant.y = j / 2;
					quadrant.cacheVersion = cellGrass->quadrantCacheVersions[j];
					quadrant.nearCovered = nearCovered;
					quadrant.grassIds = cellGrass->ids[j].data();
					quadrant.heights = cellGrass->heights[j].data();
					quadrant.worldPos = float2{ worldX, worldY };
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
