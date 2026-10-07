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

	/** @brief Builds a quadrant from streamed LAND data for one of a cell's four quadrants. */
	PGrassCommon::Quadrant MakeStreamedQuadrant(const CellGrass& cellGrass, int32_t cellX, int32_t cellY, uint32_t index, bool nearCovered)
	{
		PGrassCommon::Quadrant quadrant{};
		quadrant.cellX = cellX;
		quadrant.cellY = cellY;
		quadrant.x = index % 2;
		quadrant.y = index / 2;
		quadrant.cacheVersion = cellGrass.quadrantCacheVersions[index];
		quadrant.nearCovered = nearCovered;
		quadrant.grassIds = cellGrass.ids[index].data();
		quadrant.occupancyRows = cellGrass.occupancy[index].data();
		quadrant.heights = cellGrass.heights[index].data();
		quadrant.worldPos = float2{ (cellX * 2 + static_cast<int32_t>(quadrant.x)) * 2048.0f, (cellY * 2 + static_cast<int32_t>(quadrant.y)) * 2048.0f };
		quadrant.minHeight = cellGrass.minHeights[index];
		quadrant.maxHeight = cellGrass.maxHeights[index];
		return quadrant;
	}
}

int32_t ProceduralGrass::NearCoverageIndex(int32_t worldQuadrantX, int32_t worldQuadrantY, const VisibilityOrigin& origin)
{
	const int32_t x = worldQuadrantX - origin.quadrantX + NearCoverageRadius;
	const int32_t y = worldQuadrantY - origin.quadrantY + NearCoverageRadius;
	if (x < 0 || y < 0 || x >= NearCoverageDiameter || y >= NearCoverageDiameter)
		return -1;
	return y * NearCoverageDiameter + x;
}

RE::TESLandTexture* PGrassCommon::GetDefaultLandTexture()
{
	static const auto defaultLandTextureAddress = REL::Relocation<RE::TESLandTexture**>(RELOCATION_ID(514783, 400936));
	return *defaultLandTextureAddress;
}

float PGrassCommon::GetGrassTexturePctThreshold()
{
	if (const auto setting = RE::GetINISetting("fTexturePctThreshold:Grass"))
		return std::max(setting->GetFloat(), 0.0f);
	return 0.005f;
}

const ProceduralGrass::LoadedCellGrass& ProceduralGrass::GetCellCache(RE::TESObjectLAND* land, int32_t cellX, int32_t cellY, uint32_t debugQuadIndex)
{
	auto& cell = grassMapCache[PGrassCommon::GrassCellKey(cellX, cellY)];
	cell.lastSeenFrame = grassMapFrame;
	const auto landData = land->loadedData;

	if (cell.land == land && cell.loadedData == landData)
		return cell;

	cell.land = land;
	cell.loadedData = landData;

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
	const float texturePctThreshold = PGrassCommon::GetGrassTexturePctThreshold() * 255.0f;
	const auto getSelection = [this](const RE::TESLandTexture* texture) -> const TextureSelection* {
		if (!texture)
			return static_cast<const TextureSelection*>(nullptr);
		if (const auto cached = textureSelectionByTexture.find(texture); cached != textureSelectionByTexture.end())
			return cached->second;
		const auto selection = textureSelection.find(LandTextureKey(texture));
		const auto result = selection != textureSelection.end() ? &selection->second : nullptr;
		textureSelectionByTexture.emplace(texture, result);
		return result;
	};
	const auto growsGrass = [](const RE::TESLandTexture* texture, const TextureSelection* selection) {
		if (!texture)
			return false;
		if (selection && selection->total > 0.0f)
			return PGrassCommon::HasWeightedGrass(selection->ids, selection->cumulative);
		return !texture->textureGrassList.empty();
	};

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
			quadrant.occupancy = PGrassCommon::BuildQuadrantOccupancy(quadrant.ids.data());
			continue;
		}

		for (uint32_t v = 0; v < PGrassCommon::QuadrantGrassSamples; ++v) {
			int32_t overlayTotal = 0;
			for (uint32_t layer = 0; layer < PGrassCommon::LandscapeOverlayCount; ++layer) {
				const auto texture = landData->quadTextures[quadIndex][layer];
				if (texture && (texture->formID != 0 || defaultLandTexture))
					overlayTotal += static_cast<uint8_t>(landData->percents[quadIndex][v][layer]);
			}

			const auto baseTexture = landData->defQuadTextures[quadIndex];
			const RE::TESLandTexture* grassTexture = nullptr;
			const TextureSelection* grassSelection = nullptr;
			int32_t bestGrassPercent = -1;
			const auto considerTexture = [&](const RE::TESLandTexture* texture, const int32_t percent) {
				if (!texture || percent <= 0 || static_cast<float>(percent) < texturePctThreshold)
					return;
				const auto selection = getSelection(texture);
				if (percent > bestGrassPercent && growsGrass(texture, selection)) {
					bestGrassPercent = percent;
					grassTexture = texture;
					grassSelection = selection;
				}
			};

			considerTexture(baseTexture && baseTexture->formID != 0 ? baseTexture : defaultLandTexture, std::max(255 - overlayTotal, 0));

			for (uint32_t layer = 0; layer < PGrassCommon::LandscapeOverlayCount; ++layer) {
				const int32_t percent = static_cast<uint8_t>(landData->percents[quadIndex][v][layer]);
				const auto texture = landData->quadTextures[quadIndex][layer];
				const auto effectiveTexture = texture && texture->formID == 0 ? defaultLandTexture : texture;
				considerTexture(effectiveTexture, percent);
			}

			if (!grassTexture) {
				quadrant.ids[v] = 0u;
				continue;
			}

			uint32_t type = 1u;
			if (grassSelection && grassSelection->total > 0.0f) {
				type = PGrassCommon::SelectWeightedGrass(grassSelection->ids, grassSelection->cumulative,
					grassSelection->total, PGrassCommon::QuadrantSampleHash(cellX, cellY, quadIndex, v));
			}

			quadrant.ids[v] = static_cast<uint8_t>(type);
		}
		quadrant.occupancy = PGrassCommon::BuildQuadrantOccupancy(quadrant.ids.data());
	}

	return cell;
}

void ProceduralGrass::SyncGrassCellCachePolicy()
{
	const auto dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler)
		return;

	std::unordered_map<const RE::TESLandTexture*, GrassTexturePolicy> textureOverrides;
	textureOverrides.reserve(textureSelection.size());
	for (const auto texture : dataHandler->GetFormArray<RE::TESLandTexture>()) {
		if (!texture)
			continue;

		const auto selection = textureSelection.find(LandTextureKey(texture));
		if (selection == textureSelection.end() || selection->second.total <= 0.0f)
			continue;

		textureOverrides.emplace(texture, GrassTexturePolicy{ selection->second.ids, selection->second.cumulative, selection->second.total });
	}

	grassCellCache.SetGrassMapPolicy(std::move(textureOverrides), settings.debugIgnoreGrassMap);
	grassCellCachePolicyDirty = false;
}

void ProceduralGrass::ClearGrassMapCache()
{
	grassCellCachePolicyDirty = true;
	SyncGrassCellCachePolicy();
	grassMapCache.clear();
	++grassMapCacheVersion;
	nearVisibleStamp = (std::numeric_limits<uint64_t>::max)();
	farVisibleStamp = (std::numeric_limits<uint64_t>::max)();
	quadrantsHighLOD.clear();
	quadrantsMidLOD.clear();
	quadrantsLowLOD.clear();
	quadrantsFarLOD.clear();
	quadrantsPresence.clear();
	nearCoveredQuadrants.fill(false);
	++quadrantsHighVersion;
	++quadrantsMidVersion;
	++quadrantsLowVersion;
	++quadrantsFarVersion;
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
	if (grassCellCachePolicyDirty)
		SyncGrassCellCachePolicy();

	grassMapFrame++;
	constexpr size_t farCompletionBudget = 12;

	const auto tes = globals::game::tes;
	RE::TESWorldSpace* landWorldSpace = Util::GetLandDataWorldspace(tes ? tes->GetRuntimeData2().worldSpace : nullptr);

	grassCellCache.BeginFrame(landWorldSpace);
	grassCellCache.DrainCompleted(farCompletionBudget);

	const auto& playerPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	const VisibilityOrigin origin{
		static_cast<int32_t>(std::floor(playerPos.x / 2048.0f)),
		static_cast<int32_t>(std::floor(playerPos.y / 2048.0f)),
		static_cast<int32_t>(std::floor(playerPos.x / 4096.0f)),
		static_cast<int32_t>(std::floor(playerPos.y / 4096.0f))
	};

	// Centre the terrain-darkening grass-id window on the player.
	const int32_t presenceOriginQuadX = origin.quadrantX - PGrassCommon::LowTierQuadrantRadius;
	const int32_t presenceOriginQuadY = origin.quadrantY - PGrassCommon::LowTierQuadrantRadius;
	grassPresenceOrigin = float2{ presenceOriginQuadX * 2048.0f, presenceOriginQuadY * 2048.0f };

	const auto cells = tes ? tes->gridCells : nullptr;
	const uint64_t currentNearStamp = ComputeNearVisibilityStamp(landWorldSpace, cells, origin);
	if (currentNearStamp != nearVisibleStamp) {
		RebuildNearQuadrants(cells, origin);
		RebuildGrassPresence(presenceOriginQuadX, presenceOriginQuadY);
		nearVisibleStamp = currentNearStamp;
	}

	UpdateFarQuadrants(landWorldSpace, cells, origin);
	grassCellCache.EvictUntouched();

	globals::profiler->EndPass();
}

uint64_t ProceduralGrass::ComputeNearVisibilityStamp(RE::TESWorldSpace* landWorldSpace, const RE::GridCellArray* cells, const VisibilityOrigin& origin)
{
	const uint32_t cellCount = cells ? cells->length * cells->length : 0u;
	uint64_t stamp = PGrassCommon::GrassHashOffsetBasis;
	PGrassCommon::GrassHashValue(stamp, reinterpret_cast<uintptr_t>(landWorldSpace));
	PGrassCommon::GrassHashValue(stamp, reinterpret_cast<uintptr_t>(cells));
	PGrassCommon::GrassHashValue(stamp, origin.quadrantX);
	PGrassCommon::GrassHashValue(stamp, origin.quadrantY);
	PGrassCommon::GrassHashValue(stamp, grassMapCacheVersion);
	PGrassCommon::GrassHashValue(stamp, settings.debugIgnorePreProcessedFlag);
	PGrassCommon::GrassHashValue(stamp, cellCount);

	for (uint32_t i = 0; i < cellCount; ++i) {
		const auto cell = cells->cells[i];
		PGrassCommon::GrassHashValue(stamp, reinterpret_cast<uintptr_t>(cell));
		if (!cell)
			continue;

		const auto& runtimeData = cell->GetRuntimeData();
		const auto exterior = runtimeData.cellData.exterior;
		const auto land = runtimeData.cellLand;
		PGrassCommon::GrassHashValue(stamp, reinterpret_cast<uintptr_t>(exterior));
		PGrassCommon::GrassHashValue(stamp, reinterpret_cast<uintptr_t>(land));
		if (exterior) {
			PGrassCommon::GrassHashValue(stamp, exterior->cellX);
			PGrassCommon::GrassHashValue(stamp, exterior->cellY);
		}
		if (!land || !land->loadedData)
			continue;

		PGrassCommon::GrassHashValue(stamp, reinterpret_cast<uintptr_t>(land->loadedData));
		for (uint32_t j = 0; j < 4; ++j) {
			const auto mesh = land->loadedData->mesh[j];
			PGrassCommon::GrassHashValue(stamp, reinterpret_cast<uintptr_t>(mesh));
			if (mesh) {
				const bool preProcessed = mesh->GetFlags().all(RE::NiAVObject::Flag::kPreProcessedNode);
				PGrassCommon::GrassHashValue(stamp, preProcessed);
			}
		}
	}

	// Cached LAND fills Low's outer ring when Skyrim unloads a nearby cell before it leaves Low range.
	for (int32_t cy = origin.cellY - LowCellRadius; cy <= origin.cellY + LowCellRadius; ++cy) {
		for (int32_t cx = origin.cellX - LowCellRadius; cx <= origin.cellX + LowCellRadius; ++cx) {
			const CellGrass* cellGrass = grassCellCache.Get(cx, cy);
			PGrassCommon::GrassHashValue(stamp, PGrassCommon::GrassCellKey(cx, cy));
			if (!cellGrass) {
				PGrassCommon::GrassHashValue(stamp, uint64_t{ 0 });
				continue;
			}

			for (const auto cacheVersion : cellGrass->quadrantCacheVersions)
				PGrassCommon::GrassHashValue(stamp, cacheVersion);
		}
	}
	return stamp;
}

void ProceduralGrass::RebuildNearQuadrants(const RE::GridCellArray* cells, const VisibilityOrigin& origin)
{
	quadrantsHighLOD.clear();
	quadrantsMidLOD.clear();
	quadrantsLowLOD.clear();
	quadrantsPresence.clear();
	nearCoveredQuadrants.fill(false);
	quadrantReject = {};

	const uint32_t cellCount = cells ? cells->length * cells->length : 0u;
	for (uint32_t i = 0; i < cellCount; i++) {
		const auto cell = cells->cells[i];
		if (!cell)
			continue;
		quadrantReject.cells++;

		const auto& runtimeData = cell->GetRuntimeData();
		const auto exterior = runtimeData.cellData.exterior;
		if (!exterior)
			continue;
		quadrantReject.withExterior++;

		const auto land = runtimeData.cellLand;
		if (!land)
			continue;
		quadrantReject.withLand++;
		if (!land->loadedData)
			continue;
		quadrantReject.withLoadedData++;

		const LoadedCellGrass* cellCache = nullptr;
		for (uint32_t j = 0; j < 4; j++) {
			const auto mesh = land->loadedData->mesh[j];
			if (!mesh)
				continue;
			quadrantReject.withMesh++;
			if (!settings.debugIgnorePreProcessedFlag && !mesh->GetFlags().all(RE::NiAVObject::Flag::kPreProcessedNode))
				continue;
			quadrantReject.preProcessed++;

			if (!cellCache)
				cellCache = &GetCellCache(land, exterior->cellX, exterior->cellY, j);
			const auto& quadrantCache = cellCache->quadrants[j];

			PGrassCommon::Quadrant quadrant{};
			quadrant.cellX = exterior->cellX;
			quadrant.cellY = exterior->cellY;
			quadrant.x = j % 2;
			quadrant.y = j / 2;
			quadrant.nearCovered = true;
			quadrant.cacheVersion = quadrantCache.cacheVersion;
			quadrant.grassIds = quadrantCache.ids.data();
			quadrant.occupancyRows = quadrantCache.occupancy.data();
			quadrant.heights = quadrantCache.heights.data();
			quadrant.worldPos = float2{ (quadrant.cellX + quadrant.x * 0.5f) * 4096.0f, (quadrant.cellY + quadrant.y * 0.5f) * 4096.0f };
			quadrant.minHeight = quadrantCache.minHeight;
			quadrant.maxHeight = quadrantCache.maxHeight;

			const int32_t worldQuadrantX = quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x);
			const int32_t worldQuadrantY = quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y);
			// Overlap max-distance bands so adjacent tiers cross-fade.
			const int32_t md = std::max(std::abs(origin.quadrantX - worldQuadrantX), std::abs(origin.quadrantY - worldQuadrantY));

			if (const int32_t coverageIndex = NearCoverageIndex(worldQuadrantX, worldQuadrantY, origin); coverageIndex >= 0) {
				// Record only near quadrants with accepted LAND geometry.
				nearCoveredQuadrants[coverageIndex] = true;
				if (md <= PGrassCommon::LowTierQuadrantRadius)
					quadrantsPresence.push_back(quadrant);
			}
			if (md <= PGrassCommon::HighTierQuadrantRadius && quadrantsHighLOD.size() < PGrassCommon::HighTierQuadrantCap)
				quadrantsHighLOD.push_back(quadrant);
			if (md >= PGrassCommon::HighTierQuadrantRadius - 1 && md <= PGrassCommon::MidTierQuadrantRadius && quadrantsMidLOD.size() < PGrassCommon::MidTierQuadrantCap)
				quadrantsMidLOD.push_back(quadrant);
			if (md >= PGrassCommon::MidTierQuadrantRadius - 2 && md <= NearCoverageRadius && quadrantsLowLOD.size() < PGrassCommon::LowTierQuadrantCap)
				quadrantsLowLOD.push_back(quadrant);
		}
	}

	// Fill Low's ring from streamed LAND where the loaded grid no longer covers it.
	for (int32_t cy = origin.cellY - LowCellRadius; cy <= origin.cellY + LowCellRadius; ++cy) {
		for (int32_t cx = origin.cellX - LowCellRadius; cx <= origin.cellX + LowCellRadius; ++cx) {
			const CellGrass* cellGrass = grassCellCache.Get(cx, cy);
			if (!cellGrass)
				continue;

			for (uint32_t j = 0; j < 4; ++j) {
				const int32_t worldQuadrantX = cx * 2 + static_cast<int32_t>(j % 2);
				const int32_t worldQuadrantY = cy * 2 + static_cast<int32_t>(j / 2);
				const int32_t md = std::max(std::abs(origin.quadrantX - worldQuadrantX), std::abs(origin.quadrantY - worldQuadrantY));
				if (md < PGrassCommon::MidTierQuadrantRadius - 2 || md > NearCoverageRadius)
					continue;

				const int32_t coverageIndex = NearCoverageIndex(worldQuadrantX, worldQuadrantY, origin);
				if (nearCoveredQuadrants[coverageIndex])
					continue;

				const auto quadrant = MakeStreamedQuadrant(*cellGrass, cx, cy, j, true);
				nearCoveredQuadrants[coverageIndex] = true;
				quadrantsLowLOD.push_back(quadrant);
				if (md <= PGrassCommon::LowTierQuadrantRadius)
					quadrantsPresence.push_back(quadrant);
			}
		}
	}

	++quadrantsHighVersion;
	++quadrantsMidVersion;
	++quadrantsLowVersion;

	// Retain recently unloaded LAND cells across grid boundaries.
	EvictGrassMapCache();
}

uint8_t ProceduralGrass::LimitGrassIdToSlope(const uint8_t id, const PGrassCommon::Quadrant& quadrant, const uint32_t column, const uint32_t row) const
{
	if (!id || !quadrant.heights || quadrant.minHeight <= PGrassCommon::QuadrantNoHeight)
		return id;

	// Forward differences, taken backward on the quadrant's last row and column.
	constexpr uint32_t pitch = PGrassCommon::QuadrantGrassPitch;
	const uint32_t sample = std::min(row, pitch - 2u) * pitch + std::min(column, pitch - 2u);
	const float slopeX = (quadrant.heights[sample + 1] - quadrant.heights[sample]) / 128.0f;
	const float slopeY = (quadrant.heights[sample + pitch] - quadrant.heights[sample]) / 128.0f;
	const float normalZ = 1.0f / std::sqrt(slopeX * slopeX + slopeY * slopeY + 1.0f);
	const auto& type = resolvedGeneratorTypes.grassType[id];
	return normalZ < type.maxSlope || normalZ > type.minSlope ? uint8_t{ 0 } : id;
}

void ProceduralGrass::RebuildGrassPresence(const int32_t originQuadX, const int32_t originQuadY)
{
	uint64_t contentHash = PGrassCommon::GrassHashOffsetBasis;
	PGrassCommon::GrassHashValue(contentHash, quadrantsPresence.size());
	for (const auto& quadrant : quadrantsPresence) {
		PGrassCommon::GrassHashValue(contentHash, PGrassCommon::GrassCellKey(quadrant.cellX, quadrant.cellY));
		PGrassCommon::GrassHashValue(contentHash, PGrassCommon::GrassQuadrantKey(quadrant.x, quadrant.y));
		PGrassCommon::GrassHashValue(contentHash, quadrant.cacheVersion);
	}

	// Rebuild the presence texture only when its window or cached LAND data changes.
	if (grassPresenceOriginQuadX == originQuadX && grassPresenceOriginQuadY == originQuadY && grassPresenceContentHash == contentHash)
		return;

	std::fill(grassPresenceStaging.begin(), grassPresenceStaging.end(), uint8_t{ 0 });
	constexpr int32_t pitch = static_cast<int32_t>(PGrassCommon::QuadrantGrassPitch);
	const int32_t dim = static_cast<int32_t>(grassPresenceDim);
	for (const auto& quadrant : quadrantsPresence) {
		if (!quadrant.grassIds)
			continue;

		const int32_t sx0 = (quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x) - originQuadX) * (pitch - 1);
		const int32_t sy0 = (quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y) - originQuadY) * (pitch - 1);
		if (sx0 < 0 || sy0 < 0 || sx0 + pitch > dim || sy0 + pitch > dim)
			continue;

		const auto filled = PGrassCommon::FillQuadrantGrassIds(quadrant.grassIds,
			quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x), quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y));
		// Match the generator's quadrant-local fill and union only the shared border samples. Filling the assembled
		// window would reach into bare cells that the generator never fills.
		for (int32_t row = 0; row < pitch; ++row) {
			uint8_t* dstRow = grassPresenceStaging.data() + static_cast<size_t>(sy0 + row) * grassPresenceDim + sx0;
			for (int32_t column = 0; column < pitch; ++column) {
				const uint8_t id = LimitGrassIdToSlope(filled[row * pitch + column], quadrant, column, row);
				dstRow[column] = PGrassCommon::MergeGrassIds(dstRow[column], id);
			}
		}
	}

	grassPresenceOriginQuadX = originQuadX;
	grassPresenceOriginQuadY = originQuadY;
	grassPresenceContentHash = contentHash;
	grassPresenceUploadDirty = true;
}

void ProceduralGrass::UpdateFarQuadrants(RE::TESWorldSpace* landWorldSpace, const RE::GridCellArray* cells, const VisibilityOrigin& origin)
{
	if (!landWorldSpace) {
		farRequestWorldSpace = nullptr;
		farRequestCenterX = (std::numeric_limits<int32_t>::min)();
		farRequestCenterY = (std::numeric_limits<int32_t>::min)();
		farRequestRadius = -1;
		farRequestCursor = 0;
		if (!quadrantsFarLOD.empty()) {
			quadrantsFarLOD.clear();
			farVisibleStamp = (std::numeric_limits<uint64_t>::max)();
			++quadrantsFarVersion;
		}
		return;
	}

	// Stream Far LAND data with bounded request work and rebuild its quadrant list only when the cache changes.
	const int32_t loadedGridLength = cells ? static_cast<int32_t>(cells->length) : 5;
	const int32_t loadedCellRadius = loadedGridLength / 2;
	const int32_t extraRadius = std::clamp(settings.grassCellRadius, 0,
		std::max(0, PGrassCommon::FarCellRadiusCap - PGrassCommon::FarStreamGuardCells - loadedCellRadius));
	const int32_t radius = std::min(loadedCellRadius + std::max(extraRadius, 1) + PGrassCommon::FarStreamGuardCells, PGrassCommon::FarCellRadiusCap);
	const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();

	static const std::vector<std::pair<int32_t, int32_t>> farRequestOffsets = [] {
		std::vector<std::pair<int32_t, int32_t>> offsets;
		offsets.reserve(static_cast<size_t>((PGrassCommon::FarCellRadiusCap * 2 + 1) * (PGrassCommon::FarCellRadiusCap * 2 + 1)));
		offsets.emplace_back(0, 0);
		for (int32_t ring = 1; ring <= PGrassCommon::FarCellRadiusCap; ++ring)
			for (int32_t y = -ring; y <= ring; ++y)
				for (int32_t x = -ring; x <= ring; ++x)
					if (std::max(std::abs(x), std::abs(y)) == ring)
						offsets.emplace_back(x, y);
		return offsets;
	}();

	if (farRequestWorldSpace != landWorldSpace || farRequestCenterX != origin.cellX || farRequestCenterY != origin.cellY || farRequestRadius != radius) {
		farRequestWorldSpace = landWorldSpace;
		farRequestCenterX = origin.cellX;
		farRequestCenterY = origin.cellY;
		farRequestRadius = radius;
		farRequestCursor = 0;
	}

	const size_t activeOffsetCount = static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1));
	constexpr size_t farRequestBudget = 12;
	constexpr size_t farProbeBudget = 64;
	size_t queued = 0;
	const size_t probeCount = std::min(activeOffsetCount, farProbeBudget);
	for (size_t probe = 0; probe < probeCount && queued < farRequestBudget; ++probe) {
		if (farRequestCursor >= activeOffsetCount)
			farRequestCursor = 0;
		const auto [dx, dy] = farRequestOffsets[farRequestCursor++];
		queued += grassCellCache.Request(origin.cellX + dx, origin.cellY + dy) ? 1u : 0u;
	}

	constexpr float farVisibilityMotionStep = 8.0f;
	uint64_t currentFarStamp = PGrassCommon::GrassHashOffsetBasis;
	PGrassCommon::GrassHashValue(currentFarStamp, reinterpret_cast<uintptr_t>(landWorldSpace));
	PGrassCommon::GrassHashValue(currentFarStamp, origin.quadrantX);
	PGrassCommon::GrassHashValue(currentFarStamp, origin.quadrantY);
	PGrassCommon::GrassHashValue(currentFarStamp, origin.cellX);
	PGrassCommon::GrassHashValue(currentFarStamp, origin.cellY);
	PGrassCommon::GrassHashValue(currentFarStamp, radius);
	PGrassCommon::GrassHashValue(currentFarStamp, quadrantsLowVersion);
	PGrassCommon::GrassHashValue(currentFarStamp, static_cast<int32_t>(std::floor(cameraPosAdjust.x / farVisibilityMotionStep)));
	PGrassCommon::GrassHashValue(currentFarStamp, static_cast<int32_t>(std::floor(cameraPosAdjust.y / farVisibilityMotionStep)));
	PGrassCommon::GrassHashValue(currentFarStamp, grassCellCache.GetReadyVersion());
	if (currentFarStamp == farVisibleStamp)
		return;

	size_t quadrantCount = 0;
	bool contentChanged = false;
	const float farFallbackStart = PGrassCommon::HighTierQuadrantRadius * 2048.0f;
	const float farFallbackStartSq = farFallbackStart * farFallbackStart;

	for (int32_t cy = origin.cellY - radius; cy <= origin.cellY + radius; ++cy) {
		for (int32_t cx = origin.cellX - radius; cx <= origin.cellX + radius; ++cx) {
			const CellGrass* cellGrass = grassCellCache.Get(cx, cy);
			if (!cellGrass)
				continue;

			for (uint32_t j = 0; j < 4 && quadrantCount < PGrassCommon::FarQuadrantCount; ++j) {
				const int32_t worldQuadrantX = cx * 2 + static_cast<int32_t>(j % 2);
				const int32_t worldQuadrantY = cy * 2 + static_cast<int32_t>(j / 2);
				const int32_t coverageIndex = NearCoverageIndex(worldQuadrantX, worldQuadrantY, origin);
				const bool nearCovered = coverageIndex >= 0 && nearCoveredQuadrants[coverageIndex];

				const float worldX = worldQuadrantX * 2048.0f;
				const float worldY = worldQuadrantY * 2048.0f;
				const float farthestX = std::max(std::abs(worldX - cameraPosAdjust.x), std::abs(worldX + 2048.0f - cameraPosAdjust.x));
				const float farthestY = std::max(std::abs(worldY - cameraPosAdjust.y), std::abs(worldY + 2048.0f - cameraPosAdjust.y));
				if (nearCovered && farthestX * farthestX + farthestY * farthestY < farFallbackStartSq)
					continue;

				const auto quadrant = MakeStreamedQuadrant(*cellGrass, cx, cy, j, nearCovered);
				if (quadrantCount < quadrantsFarLOD.size()) {
					contentChanged |= quadrantsFarLOD[quadrantCount] != quadrant;
					quadrantsFarLOD[quadrantCount] = quadrant;
				} else {
					quadrantsFarLOD.push_back(quadrant);
					contentChanged = true;
				}
				++quadrantCount;
			}
		}
	}

	farVisibleStamp = currentFarStamp;
	contentChanged |= quadrantCount != quadrantsFarLOD.size();
	quadrantsFarLOD.resize(quadrantCount);
	if (contentChanged)
		++quadrantsFarVersion;
}
