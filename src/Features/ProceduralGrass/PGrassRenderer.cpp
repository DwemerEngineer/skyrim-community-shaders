#include "PGrassRenderer.h"

#include "Features/ProceduralGrass.h"
#include "TopDownOcclusion.h"

#include "Features/GrassCollision.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/WetnessEffects.h"
#include "HiZPyramid.h"
#include "ShaderCache.h"
#include "State.h"
#include "TerrainHeightMap.h"
#include "Utils/D3D.h"

using namespace PGrassCommon;
using namespace PGrassRendererQuads;

namespace
{
	bool IsOccupiedGrassTile(const uint32_t* occupancyRows, const uint32_t patchStartX, const uint32_t patchEndX, const uint32_t patchStartY,
		const uint32_t patchEndY, const uint32_t density, const float edgeNoise)
	{
		if (!occupancyRows)
			return true;

		constexpr int32_t cellsPerAxis = PGrassCommon::QuadrantGrassPitch - 1;
		constexpr float cellWidth = 2048.0f / cellsPerAxis;
		const float patchWidth = 4096.0f / density;
		const float noise = std::max(edgeNoise, 0.0f);
		const int32_t minCellX = std::max(0, static_cast<int32_t>(std::floor((patchStartX * patchWidth - noise) / cellWidth)));
		const int32_t minCellY = std::max(0, static_cast<int32_t>(std::floor((patchStartY * patchWidth - noise) / cellWidth)));
		const int32_t maxCellX = std::min(cellsPerAxis - 1, static_cast<int32_t>(std::floor((patchEndX * patchWidth + noise) / cellWidth)));
		const int32_t maxCellY = std::min(cellsPerAxis - 1, static_cast<int32_t>(std::floor((patchEndY * patchWidth + noise) / cellWidth)));

		const uint32_t width = static_cast<uint32_t>(maxCellX - minCellX + 1);
		const uint32_t bits = width >= 16u ? 0xFFFFu : ((1u << width) - 1u) << minCellX;
		const uint16_t mask = static_cast<uint16_t>(bits);
		for (int32_t y = minCellY; y <= maxCellY; ++y)
			if ((occupancyRows[y] & mask) != 0u)
				return true;

		return false;
	}
}

namespace PGrassRendererQuads
{
	uint32_t QuadrantHash(uint32_t x, uint32_t y)
	{
		constexpr uint32_t multiplier = 1103515245u;
		const uint32_t qx = multiplier * ((x >> 1u) ^ y);
		const uint32_t qy = multiplier * ((y >> 1u) ^ x);
		return multiplier * (qx ^ (qy >> 3u));
	}

	SideFrustum BuildSideFrustum(const float4x4& viewProj)
	{
		SideFrustum frustum{};
		frustum.planes[0] = { viewProj._11 + viewProj._14, viewProj._21 + viewProj._24, viewProj._31 + viewProj._34, viewProj._41 + viewProj._44 };
		frustum.planes[1] = { -viewProj._11 + viewProj._14, -viewProj._21 + viewProj._24, -viewProj._31 + viewProj._34, -viewProj._41 + viewProj._44 };
		frustum.planes[2] = { viewProj._12 + viewProj._14, viewProj._22 + viewProj._24, viewProj._32 + viewProj._34, viewProj._42 + viewProj._44 };
		frustum.planes[3] = { -viewProj._12 + viewProj._14, -viewProj._22 + viewProj._24, -viewProj._32 + viewProj._34, -viewProj._42 + viewProj._44 };
		return frustum;
	}

	QuadrantFrustumState ClassifyQuadrantFrustum(const Quadrant& quadrant, const SideFrustum& frustum, const float4& cameraPosAdjust, float geometryPadding, bool& hasLand)
	{
		hasLand = quadrant.maxHeight > QuadrantNoHeight && quadrant.minHeight <= quadrant.maxHeight;
		if (!hasLand)
			return QuadrantFrustumState::Intersecting;

		const float minZ = quadrant.minHeight - 256.0f;
		const float maxZ = quadrant.maxHeight + 300.0f;
		const float3 center = { quadrant.worldPos.x + 1024.0f - cameraPosAdjust.x, quadrant.worldPos.y + 1024.0f - cameraPosAdjust.y, (minZ + maxZ) * 0.5f - cameraPosAdjust.z };

		// The top and bottom side planes have a Z component, so include the blade envelope on every axis.
		const float3 extent = { 1024.0f + geometryPadding, 1024.0f + geometryPadding, (maxZ - minZ) * 0.5f + geometryPadding };

		bool fullyInside = true;
		for (const auto& plane : frustum.planes) {
			const float distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
			const float radius = std::abs(plane.x) * extent.x + std::abs(plane.y) * extent.y + std::abs(plane.z) * extent.z;
			if (distance + radius < 0.0f)
				return QuadrantFrustumState::Outside;
			fullyInside &= distance - radius >= 0.0f;
		}

		return fullyInside ? QuadrantFrustumState::Inside : QuadrantFrustumState::Intersecting;
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
PGrassRenderer<QuadrantCount, PatchBladeCount>::PGrassRenderer(const uint32_t grassDensity, const uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef,
	const uint32_t slopeExtra, const uint32_t bladeStride, Buffer* outerVertexIndicesBuf)
{
	vertexIndicesBuffer = vertexIndicesBuf;
	outerVertexIndicesBuffer = outerVertexIndicesBuf;
	lodDefine = lodDef;
	vertCountDefine = vertCountDef;
	extraDefine = extraDef;
	slopeExtraBlades = slopeExtra;
	bladeStrideBytes = bladeStride;
	bladeBatchSizeString = std::to_string(extraDefine ? FarBladeBatchSize : (UsesBatchedLow() ? LowBladeBatchSize : MidBladeBatchSize));

	CreateArgsBuffer();
	SetDensity(grassDensity);
	SetThreadGroupSize(tgSize);

	GetBladeGeneratorCS();
	if (extraDefine)
		GetBladeGeneratorCS(true);

	// Only High and Mid have a depth prepass.
	const bool hasDepthPrepass = !UsesBatchedLow() && !extraDefine;
	GetVertexShader(false, false);
	if (hasDepthPrepass)
		GetVertexShader(true, false);
	if (outerVertexIndicesBuffer) {
		GetVertexShader(false, true);
		if (hasDepthPrepass)
			GetVertexShader(true, true);
	}

	bool noWetness = false;
	bool noLocalLights = false;

	GetPS(noWetness, noLocalLights);

	if (!extraDefine && globals::features::lightLimitFix.loaded) {
		noLocalLights = true;
		GetPS(noWetness, noLocalLights);
	}

	if (!extraDefine && globals::features::wetnessEffects.loaded) {
		noWetness = true;
		GetPS(noWetness, noLocalLights);
	}

	if (!extraDefine && globals::features::wetnessEffects.loaded && globals::features::lightLimitFix.loaded) {
		noWetness = true;
		noLocalLights = true;
		GetPS(noWetness, noLocalLights);
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::CreateArgsBuffer()
{
	quadrantsCB = new ConstantBuffer(ConstantBufferDesc<QuadrantDataArray<QuadrantCount>>(), "PGrass::QuadrantsCB");

	constexpr uint32_t grassSampleCount = QuadrantCount * QuadrantGrassSamples;

	constexpr uint32_t grassCellCount = QuadrantCount * (QuadrantGrassPitch - 1) * (QuadrantGrassPitch - 1);
	quadrantGrassCellsSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(grassCellCount, true), grassCellCount, "PGrass::QuadrantGrassCells");
	quadrantGrassCellsSB->CreateSRV();
	quadrantGrassCellsStaging.assign(grassCellCount, 0u);
	quadrantGrassIdsStaging.resize(QuadrantCount);
	constexpr uint32_t occupancyRowCount = QuadrantCount * OccupancyTilesPerAxis;
	quadrantOccupancySB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(occupancyRowCount, true), occupancyRowCount, "PGrass::OccupancyRows");
	quadrantOccupancySB->CreateSRV();
	quadrantOccupancyStaging.assign(occupancyRowCount, 0xFFFFu);
	quadrantOccupancyVersions.resize(QuadrantCount);

	quadrantHeightSB = new StructuredBuffer(StructuredBufferDesc<float>(grassSampleCount, true), grassSampleCount, "PGrass::QuadrantHeights");
	quadrantHeightStaging.assign(grassSampleCount, QuadrantNoHeight);
	quadrantHeightSB->CreateSRV();
	constexpr uint32_t tileCount = QuadrantCount * OccupancyTileCount;
	tileHeightBoundsSB = new StructuredBuffer(StructuredBufferDesc<float2>(tileCount, true), tileCount, "PGrass::TileHeightBounds");
	tileHeightBoundsSB->CreateSRV();
	tileHeightBoundsStaging.resize(tileCount);
	objectOccupancyRows.resize(QuadrantCount);
	objectHeightBounds.resize(QuadrantCount);

	constexpr uint32_t workItemCapacity = 2u * QuadrantCount * PatchBladeCount * OccupancyTileCount;
	visibleWorkSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(workItemCapacity, true), workItemCapacity, "PGrass::VisibleWork");
	visibleWorkSB->CreateSRV();
	workRangeCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<std::array<uint32_t, 4>>(), "PGrass::WorkRange");
	visibleWorkStaging.reserve(workItemCapacity);
	visibleWorkCandidates.reserve(QuadrantCount);
	visibleTilesStaging.reserve(QuadrantCount * OccupancyTileCount);
	occupancyCache.reserve(QuadrantCount * 2u);

	D3D11_BUFFER_DESC argsBufferDesc{};
	argsBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	argsBufferDesc.CPUAccessFlags = 0;
	argsBufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	argsBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	argsBufferDesc.ByteWidth = 10 * sizeof(uint32_t);

	const auto createIndirectArgs = [&](Buffer* indexBuffer, const char* name) {
		if (!indexBuffer)
			return static_cast<Buffer*>(nullptr);
		const uint32_t initialArgs[10] = {
			indexBuffer->desc.ByteWidth / sizeof(uint16_t),  // IndexCountPerInstance
			0,                                               // InstanceCount, written by the generator
			0,                                               // StartIndexLocation
			0,                                               // BaseVertexLocation
			0,                                               // StartInstanceLocation
			outerVertexIndicesBuffer ? outerVertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t) : 0, 0, 0, 0, 0
		};
		D3D11_SUBRESOURCE_DATA argsBufferInit{ initialArgs, 0, 0 };
		return new Buffer(argsBufferDesc, &argsBufferInit, name);
	};

	argsBuffer = createIndirectArgs(vertexIndicesBuffer, "PGrass::IndirectArgs");
	D3D11_UNORDERED_ACCESS_VIEW_DESC argsUAVDesc{};
	argsUAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	argsUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	argsUAVDesc.Buffer.NumElements = 10;
	argsUAVDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	argsBuffer->CreateUAV(argsUAVDesc);
	D3D11_SHADER_RESOURCE_VIEW_DESC argsSRVDesc{};
	argsSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	argsSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
	argsSRVDesc.BufferEx.NumElements = 10;
	argsSRVDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
	argsBuffer->CreateSRV(argsSRVDesc);
	if (UsesBatchedDraws()) {
		// Batched tiers draw fixed groups of blades and discard the final batch's unused tail in the VS.
		const uint32_t batchArgs[10]{};
		D3D11_SUBRESOURCE_DATA batchArgsInit{ batchArgs, 0, 0 };
		batchArgsBuffer = new Buffer(argsBufferDesc, &batchArgsInit, "PGrass::BatchArgs");
		batchArgsBuffer->CreateUAV(argsUAVDesc);
	}

	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.ByteWidth = 10 * sizeof(uint32_t);
	if (SUCCEEDED(globals::d3d::device->CreateBuffer(&stagingDesc, nullptr, argsStaging.put())))
		Util::SetResourceName(argsStaging.get(), "PGrass::IndirectArgsStaging");
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::SetDensity(uint32_t grassDensity)
{
	density = grassDensity;
	patchesPerQuadrant = grassDensity * grassDensity / 4;
	densityString = std::to_string(grassDensity);

	if (extraDefine) {
		// Near Low, Far fills to Low's blades per Far patch; steep slopes double that through the slope fill.
		const float lowDensity = static_cast<float>(globals::features::proceduralGrass.settings.lowGrassDensity);
		const float densityRatio = lowDensity * lowDensity / std::max(static_cast<float>(grassDensity * grassDensity), 1.0f);
		// Each handoff extra record draws a double blade.
		const float extraRecords = (2.0f * densityRatio - 1.0f) * (outerVertexIndicesBuffer ? 0.5f : 1.0f);
		const uint32_t extraSlots = static_cast<uint32_t>(std::clamp(std::ceil(extraRecords), 1.0f, 15.0f));
		if (extraSlots != handoffExtraBlades) {
			handoffExtraBlades = extraSlots;
			Util::ReleaseAndNull(handoffGeneratorCS);
			handoffGeneratorCompileAttempted = false;
		}
	}

	hasCachedWorkList = false;
	const uint32_t patchesPerRow = density / 2u;
	const uint32_t patchRows = (patchesPerQuadrant + patchesPerRow - 1u) / patchesPerRow;
	const float patchWorldSize = 4096.0f / static_cast<float>(density);

	for (uint32_t tileY = 0; tileY < OccupancyTilesPerAxis; ++tileY) {
		for (uint32_t tileX = 0; tileX < OccupancyTilesPerAxis; ++tileX) {
			const uint32_t tile = tileY * OccupancyTilesPerAxis + tileX;
			const uint32_t startX = tileX * patchesPerRow / OccupancyTilesPerAxis;
			const uint32_t startY = tileY * patchRows / OccupancyTilesPerAxis;
			const uint32_t endX = (tileX + 1u) * patchesPerRow / OccupancyTilesPerAxis;
			const uint32_t endY = (tileY + 1u) * patchRows / OccupancyTilesPerAxis;
			tilePatchCounts[tile] = (endX - startX) * (endY - startY);
			tileLocalBounds[tile] = {
				static_cast<float>(startX) * patchWorldSize,
				static_cast<float>(startY) * patchWorldSize,
				static_cast<float>(endX) * patchWorldSize,
				static_cast<float>(endY) * patchWorldSize
			};
		}
	}

	occupancyCache.clear();

	ResetBladeCapacity();

	ClearGeneratorCache();
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::ResetBladeCapacity()
{
	delete bladesSB;
	bladesSB = nullptr;
	bladeBufferCapacity = 0;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::EnsureBladeCapacity(const uint64_t requiredBladeCount)
{
	if (bladesSB && requiredBladeCount <= bladeBufferCapacity)
		return;

	const uint64_t allocationQuantum = std::max<uint64_t>(patchesPerQuadrant, 1u);
	const uint64_t maximumCandidateCount = 2u * allocationQuantum * QuadrantCount * (PatchBladeCount + std::max(slopeExtraBlades, handoffExtraBlades));
	const uint64_t maximumBufferElements = std::numeric_limits<UINT>::max() / bladeStrideBytes;
	const uint64_t maximumCapacity = std::min(maximumCandidateCount, maximumBufferElements);

	if (requiredBladeCount > maximumCapacity)
		throw std::overflow_error("Procedural grass blade buffer exceeds the D3D11 buffer-size limit");

	uint64_t targetCapacity = std::max(requiredBladeCount, allocationQuantum);
	if (requiredBladeCount > allocationQuantum) {
		// Reserve 12.5% for nearby work entering the frustum.
		const uint64_t demandMargin = std::max(requiredBladeCount / 8u, allocationQuantum);
		targetCapacity = requiredBladeCount + demandMargin;
	}
	if (bladeBufferCapacity != 0) {
		const uint64_t growthMargin = std::max<uint64_t>(bladeBufferCapacity / 2u, allocationQuantum);
		targetCapacity = std::max(targetCapacity, static_cast<uint64_t>(bladeBufferCapacity) + growthMargin);
	}

	targetCapacity = ((targetCapacity + allocationQuantum - 1u) / allocationQuantum) * allocationQuantum;
	targetCapacity = std::min(targetCapacity, maximumCapacity);
	if (targetCapacity < requiredBladeCount)
		targetCapacity = requiredBladeCount;

	const auto newCapacity = static_cast<uint32_t>(targetCapacity);
	auto bladesDesc = StructuredBufferDesc<Blade>(newCapacity, false);
	bladesDesc.StructureByteStride = bladeStrideBytes;
	bladesDesc.ByteWidth = bladeStrideBytes * newCapacity;

	delete bladesSB;
	bladesSB = new StructuredBuffer(bladesDesc, newCapacity, "PGrass::Blades");
	bladesSB->CreateUAV();
	bladesSB->CreateSRV();

	logger::info("[Procedural Grass] {} blade buffer high-water mark: {} blades ({:.1f} MiB), {} required",
		lodDefine, newCapacity, static_cast<double>(bladesDesc.ByteWidth) / (1024.0 * 1024.0), requiredBladeCount);
	bladeBufferCapacity = newCapacity;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::SetThreadGroupSize(uint32_t tgSize)
{
	threadGroupSize = tgSize;
	hasCachedWorkList = false;
	threadGroupSizeString = std::to_string(threadGroupSize);

	ClearGeneratorCache();
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::ClearGeneratorCache()
{
	Util::ReleaseAndNull(generatorCS);
	generatorCompileAttempted = false;
	Util::ReleaseAndNull(handoffGeneratorCS);
	handoffGeneratorCompileAttempted = false;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::ClearShaderCache()
{
	ClearGeneratorCache();
	Util::ReleaseAndNull(batchArgsCS);
	for (auto& vertexShader : vertexShaders)
		Util::ReleaseAndNull(vertexShader);

	for (auto& pixelShader : pixelShaders)
		Util::ReleaseAndNull(pixelShader);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<Quadrant>& quadrants, const uint64_t contentVersion, const int32_t cellXOffset, const int32_t cellYOffset,
	const float2& lodOrigin, const float4& lodFadeIn, const float4& lodFadeOut, const float frustumPadding,
	const bool disableGeneratorCulls, const float fadeInPositionPadding, const float4& farKeepParams)
{
	auto* bladeGenerator = GetBladeGeneratorCS();
	auto* batchArgsGenerator = batchArgsBuffer ? GetBatchArgsCS() : nullptr;

	if (!bladeGenerator || (batchArgsBuffer && !batchArgsGenerator)) {
		const uint32_t emptyArgs[10] = {
			vertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t), 0, 0, 0, 0,
			outerVertexIndicesBuffer ? outerVertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t) : 0, 0, 0, 0, 0
		};
		ctx->UpdateSubresource(argsBuffer->resource.get(), 0, nullptr, emptyArgs, 0, 0);

		if (batchArgsBuffer) {
			const uint32_t emptyBatchArgs[10]{};
			ctx->UpdateSubresource(batchArgsBuffer->resource.get(), 0, nullptr, emptyBatchArgs, 0, 0);
		}

		return;
	}

	const float tileReach = (extraDefine ? 0.0f : frustumPadding) + 4096.0f / density;
	uint64_t surfaceVersion = contentVersion;
	const auto* topDown = globals::topDownOcclusion;
	GrassHashValue(surfaceVersion, topDown->GetGrassSurfaceSRV() ? topDown->GetGrassSurfaceRevision() : 0u);
	UploadQuadrantInputs(quadrants, surfaceVersion, cellXOffset, cellYOffset, lodFadeIn, lodFadeOut, tileReach);

	const auto quadrantsBuffer = quadrantsCB->CB();
	ctx->CSSetConstantBuffers(7, 1, &quadrantsBuffer);

	const WorkListState workListState{ surfaceVersion, density, threadGroupSize, static_cast<uint32_t>(quadrants.size()),
		globals::game::frameBufferCached.GetCameraViewProjUnjittered().Transpose(), globals::game::frameBufferCached.GetCameraPosAdjust(),
		lodOrigin, lodFadeIn, lodFadeOut, frustumPadding, fadeInPositionPadding, farKeepParams,
		globals::features::proceduralGrass.settings.grassMapEdgeNoise, disableGeneratorCulls };

	// CPU visibility depends only on these inputs. Wind and Hi-Z still run in the generator every frame.
	if (!hasCachedWorkList || !(workListState == lastWorkListState))
		BuildVisibleWorkList(quadrants, workListState);

	EnsureBladeCapacity(cachedRequiredBladeCount);
	DispatchGeneration(ctx, bladeGenerator, batchArgsGenerator);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
uint32_t PGrassRenderer<QuadrantCount, PatchBladeCount>::SharedPatchCount(const uint32_t patchCount, const uint32_t shareStep)
{
	// Matches the generator's ceil(patchCount * workShare); eighths are exact in float.
	return std::max(1u, (patchCount * shareStep + WorkShareSteps - 1u) / WorkShareSteps);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::UploadQuadrantInputs(const std::vector<Quadrant>& quadrants, const uint64_t contentVersion, const int32_t cellXOffset,
	const int32_t cellYOffset, const float4& lodFadeIn, const float4& lodFadeOut, const float tileReach)
{
	const bool fadesChanged = lodFadeIn != lastUploadLodFadeIn || lodFadeOut != lastUploadLodFadeOut;
	const bool quadrantInputsChanged = !hasUploadedQuadrants || contentVersion != lastUploadVersion || tileReach != lastTileReach;
	if (!quadrantInputsChanged && !fadesChanged)
		return;

	auto& quadrantDataArray = quadrantDataStaging;
	quadrantDataArray.lodFadeIn = lodFadeIn;
	quadrantDataArray.lodFadeOut = lodFadeOut;

	if (quadrantInputsChanged) {
		for (uint32_t i = 0; i < quadrants.size(); i++) {
			const auto& quadrant = quadrants[i];
			auto& quadrantData = quadrantDataArray.data[i];
			quadrantData.quadWorldPos = quadrant.worldPos;
			const uint32_t hashX = static_cast<uint32_t>((quadrant.cellX + cellXOffset) * 32 + quadrant.x * 16);
			const uint32_t hashY = static_cast<uint32_t>((quadrant.cellY + cellYOffset) * 32 + quadrant.y * 16);
			quadrantData.quadrantHash = QuadrantHash(hashX, hashY);
			quadrantData.flags = quadrant.maxHeight > QuadrantNoHeight ? WorkHasLand : 0u;

			objectOccupancyRows[i].fill(0u);
			objectHeightBounds[i] = { QuadrantNoHeight, QuadrantNoHeight };
			if (const auto* surface = globals::topDownOcclusion->GetGrassSurfaceQuadrant(quadrant.cellX * 2 + quadrant.x, quadrant.cellY * 2 + quadrant.y)) {
				objectOccupancyRows[i] = surface->rows;
				objectHeightBounds[i] = { surface->minHeight, surface->maxHeight };
			}

			StageQuadrantGrassIds(i, quadrant);

			auto* heightDst = quadrantHeightStaging.data() + i * QuadrantGrassSamples;
			if (quadrant.heights)
				std::copy_n(quadrant.heights, QuadrantGrassSamples, heightDst);
			else
				std::fill_n(heightDst, QuadrantGrassSamples, QuadrantNoHeight);
			StageTileHeightBounds(i, quadrant, tileReach);
		}

		const size_t activeSamples = quadrants.size() * QuadrantGrassSamples;
		const size_t activeCells = quadrants.size() * (QuadrantGrassPitch - 1) * (QuadrantGrassPitch - 1);
		PackQuadrantGrassCells(quadrants);
		quadrantGrassCellsSB->UpdatePartial(quadrantGrassCellsStaging.data(), activeCells * sizeof(uint32_t));
		quadrantOccupancySB->UpdatePartial(quadrantOccupancyStaging.data(), quadrants.size() * OccupancyTilesPerAxis * sizeof(uint32_t));
		quadrantHeightSB->UpdatePartial(quadrantHeightStaging.data(), activeSamples * sizeof(float));
		tileHeightBoundsSB->UpdatePartial(tileHeightBoundsStaging.data(), quadrants.size() * OccupancyTileCount * sizeof(float2));
	}

	quadrantsCB->Update(&quadrantDataArray, offsetof(QuadrantDataArray<QuadrantCount>, data) + quadrants.size() * sizeof(QuadrantData));

	lastUploadVersion = contentVersion;
	lastUploadLodFadeIn = lodFadeIn;
	lastUploadLodFadeOut = lodFadeOut;
	lastTileReach = tileReach;
	hasUploadedQuadrants = true;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::StageQuadrantGrassIds(const uint32_t index, const Quadrant& quadrant)
{
	quadrantGrassIdsStaging[index] = FillQuadrantGrassIds(quadrant.grassIds,
		quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x), quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y));
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::PackQuadrantGrassCells(const std::vector<Quadrant>& quadrants)
{
	std::unordered_map<uint64_t, uint32_t> indices;
	indices.reserve(quadrants.size());
	for (uint32_t i = 0; i < quadrants.size(); ++i) {
		const auto& q = quadrants[i];
		indices.emplace(GrassQuadrantKey(q.cellX * 2 + q.x, q.cellY * 2 + q.y), i);
	}

	// A locally filled border can be bare in the adjacent quadrant. Union the shared samples so both sides fade
	// from the same values. Horizontal then vertical passes also reconcile corners shared by four quadrants.
	for (uint32_t axis = 0; axis < 2; ++axis) {
		for (uint32_t i = 0; i < quadrants.size(); ++i) {
			const auto& q = quadrants[i];
			if (!q.grassIds)
				continue;
			const uint32_t x = q.cellX * 2 + q.x;
			const uint32_t y = q.cellY * 2 + q.y;
			const auto neighbour = indices.find(GrassQuadrantKey(x + (axis == 0), y + (axis == 1)));
			if (neighbour == indices.end() || !quadrants[neighbour->second].grassIds)
				continue;

			auto& a = quadrantGrassIdsStaging[i];
			auto& b = quadrantGrassIdsStaging[neighbour->second];
			for (uint32_t sample = 0; sample < QuadrantGrassPitch; ++sample) {
				auto& first = a[axis == 0 ? sample * QuadrantGrassPitch + QuadrantCellPitch : QuadrantCellPitch * QuadrantGrassPitch + sample];
				auto& second = b[axis == 0 ? sample * QuadrantGrassPitch : sample];
				const auto shared = MergeGrassIds(first, second);
				first = second = shared;
			}
		}
	}

	// Pack each 2x2 LAND cell into one uint so bilinear sampling needs one structured-buffer load.
	for (uint32_t i = 0; i < quadrants.size(); ++i) {
		const auto& ids = quadrantGrassIdsStaging[i];
		auto* cellDst = quadrantGrassCellsStaging.data() + i * OccupancyTileCount;
		auto* rows = quadrantOccupancyStaging.data() + i * OccupancyTilesPerAxis;
		uint64_t version = GrassHashOffsetBasis;
		for (uint32_t y = 0; y < QuadrantCellPitch; ++y) {
			uint32_t row = 0u;
			for (uint32_t x = 0; x < QuadrantCellPitch; ++x) {
				const uint32_t base = y * QuadrantGrassPitch + x;
				const uint32_t packed = uint32_t(ids[base]) | uint32_t(ids[base + 1]) << 8 |
				                        uint32_t(ids[base + QuadrantGrassPitch]) << 16 | uint32_t(ids[base + QuadrantGrassPitch + 1]) << 24;
				cellDst[y * QuadrantCellPitch + x] = packed;
				if (packed != 0u)
					row |= 1u << x;
			}
			rows[y] = row;
			GrassHashValue(version, row);
		}
		quadrantOccupancyVersions[i] = version;
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::StageTileHeightBounds(const uint32_t index, const Quadrant& quadrant, const float tileReach)
{
	// Include neighboring LAND samples reached by candidate jitter and clump displacement.
	const uint32_t sampleReach = static_cast<uint32_t>(std::ceil(tileReach / 128.0f));

	for (uint32_t tileY = 0; tileY < OccupancyTilesPerAxis; ++tileY) {
		for (uint32_t tileX = 0; tileX < OccupancyTilesPerAxis; ++tileX) {
			float2 bounds = float2(QuadrantNoHeight, QuadrantNoHeight);

			if (quadrant.heights) {
				const uint32_t minX = tileX > sampleReach ? tileX - sampleReach : 0u;
				const uint32_t minY = tileY > sampleReach ? tileY - sampleReach : 0u;
				const uint32_t maxX = std::min(tileX + sampleReach + 1u, QuadrantGrassPitch - 1u);
				const uint32_t maxY = std::min(tileY + sampleReach + 1u, QuadrantGrassPitch - 1u);

				float maxDeltaX = 0.0f;
				float maxDeltaY = 0.0f;
				bounds = float2(std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());

				for (uint32_t y = minY; y <= maxY; ++y) {
					for (uint32_t x = minX; x <= maxX; ++x) {
						const float height = quadrant.heights[y * QuadrantGrassPitch + x];
						bounds.x = std::min(bounds.x, height);
						bounds.y = std::max(bounds.y, height);

						if (!extraDefine && x > minX)
							maxDeltaX = std::max(maxDeltaX, std::abs(height - quadrant.heights[y * QuadrantGrassPitch + x - 1u]));
						if (!extraDefine && y > minY)
							maxDeltaY = std::max(maxDeltaY, std::abs(height - quadrant.heights[(y - 1u) * QuadrantGrassPitch + x]));
					}
				}

				// Near roots can extend their sampled terrain plane after jitter and clump displacement.
				if (!extraDefine) {
					const float extension = (maxDeltaX + maxDeltaY) * (tileReach / 128.0f);
					bounds.x -= extension;
					bounds.y += extension;
				}
			}

			const auto objectBounds = objectHeightBounds[index];
			if (objectBounds.x > QuadrantNoHeight)
				bounds = bounds.x > QuadrantNoHeight ? float2(std::min(bounds.x, objectBounds.x), std::max(bounds.y, objectBounds.y)) : objectBounds;

			tileHeightBoundsStaging[index * OccupancyTileCount + tileY * OccupancyTilesPerAxis + tileX] = bounds;
		}
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
const typename PGrassRenderer<QuadrantCount, PatchBladeCount>::OccupancyCacheEntry& PGrassRenderer<QuadrantCount, PatchBladeCount>::GetOccupiedTiles(const Quadrant& quadrant, const uint32_t index, const float edgeNoise)
{
	const uint32_t patchesPerRow = density / 2u;
	const uint32_t patchRows = (patchesPerQuadrant + patchesPerRow - 1u) / patchesPerRow;
	const int32_t worldQuadrantX = quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x);
	const int32_t worldQuadrantY = quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y);
	const uint64_t occupancyKey = static_cast<uint64_t>(static_cast<uint32_t>(worldQuadrantX)) << 32 | static_cast<uint32_t>(worldQuadrantY);
	auto& occupancy = occupancyCache[occupancyKey];
	const auto* rows = quadrantOccupancyStaging.data() + index * OccupancyTilesPerAxis;
	if (occupancy.cacheVersion == quadrantOccupancyVersions[index] && occupancy.density == density && occupancy.edgeNoise == edgeNoise)
		return occupancy;

	occupancy.occupiedTileCount = 0;
	for (uint32_t tileY = 0; tileY < OccupancyTilesPerAxis; ++tileY) {
		const uint32_t patchStartY = tileY * patchRows / OccupancyTilesPerAxis;
		const uint32_t patchEndY = (tileY + 1u) * patchRows / OccupancyTilesPerAxis;
		for (uint32_t tileX = 0; tileX < OccupancyTilesPerAxis; ++tileX) {
			const uint32_t patchStartX = tileX * patchesPerRow / OccupancyTilesPerAxis;
			const uint32_t patchEndX = (tileX + 1u) * patchesPerRow / OccupancyTilesPerAxis;
			const uint32_t tilePatchCount = (patchEndX - patchStartX) * (patchEndY - patchStartY);
			if (tilePatchCount == 0u || !IsOccupiedGrassTile(rows, patchStartX, patchEndX, patchStartY, patchEndY, density, edgeNoise))
				continue;

			const uint32_t tile = tileY * OccupancyTilesPerAxis + tileX;
			occupancy.occupiedTiles[occupancy.occupiedTileCount++] = { static_cast<uint16_t>(tile), static_cast<uint16_t>(tilePatchCount) };
		}
	}
	occupancy.cacheVersion = quadrantOccupancyVersions[index];
	occupancy.density = density;
	occupancy.edgeNoise = edgeNoise;
	return occupancy;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::BuildVisibleWorkList(const std::vector<Quadrant>& quadrants, const WorkListState& state)
{
	visibleWorkStaging.clear();
	visibleWorkCandidates.clear();
	visibleTilesStaging.clear();

	const auto& lodOrigin = state.lodOrigin;
	const auto& lodFadeIn = state.lodFadeIn;
	const auto& lodFadeOut = state.lodFadeOut;
	const auto& cameraPosAdjust = state.cameraPosAdjust;
	const float frustumPadding = state.frustumPadding;
	const bool disableGeneratorCulls = state.disableGeneratorCulls;

	const uint32_t patchesPerRow = density / 2u;
	const uint32_t patchRows = (patchesPerQuadrant + patchesPerRow - 1u) / patchesPerRow;
	const uint32_t maxTilePatchWidth = (patchesPerRow + OccupancyTilesPerAxis - 1u) / OccupancyTilesPerAxis;
	const uint32_t maxTilePatchHeight = (patchRows + OccupancyTilesPerAxis - 1u) / OccupancyTilesPerAxis;
	const uint32_t fullGX = (patchesPerQuadrant + threadGroupSize - 1u) / threadGroupSize;
	const uint32_t tileGX = (maxTilePatchWidth * maxTilePatchHeight + threadGroupSize - 1u) / threadGroupSize;
	const bool isFarTier = extraDefine != nullptr;
	const bool isLowTier = !isFarTier && UsesSimpleLighting();
	const bool cullInnerFade = !disableGeneratorCulls && isLowTier && lodFadeIn.y > 0.0f;
	const bool cullLowOuter = !disableGeneratorCulls && isLowTier && lodFadeIn.w > 0.0f;
	const bool cullFarOuter = !disableGeneratorCulls && isFarTier && lodFadeOut.w > 0.0f;
	const float innerFadeRadius = std::max(lodFadeIn.x - state.fadeInPositionPadding, 0.0f);
	const float innerFadeRadiusSq = innerFadeRadius * innerFadeRadius;
	const float lowOuterRadius = lodFadeIn.w + state.fadeInPositionPadding;
	const float farOuterRadius = cullFarOuter ? lodFadeIn.w + 1.0f / lodFadeOut.w : 0.0f;
	const float farOuterRadiusSq = farOuterRadius * farOuterRadius;
	const auto frustum = BuildSideFrustum(state.viewProj);

	uint64_t requiredBladeCount = 0;
	uint64_t tiledGroupCount = 0;
	uint64_t legacyGroupCount = 0;
	const auto maxDistanceSqToRect = [&](const float minX, const float minY, const float maxX, const float maxY) {
		const float dx = std::max(std::abs(minX - lodOrigin.x), std::abs(maxX - lodOrigin.x));
		const float dy = std::max(std::abs(minY - lodOrigin.y), std::abs(maxY - lodOrigin.y));
		return dx * dx + dy * dy;
	};
	const auto minDistanceSqToRect = [&](const float minX, const float minY, const float maxX, const float maxY) {
		const float dx = std::clamp(lodOrigin.x, minX, maxX) - lodOrigin.x;
		const float dy = std::clamp(lodOrigin.y, minY, maxY) - lodOrigin.y;
		return dx * dx + dy * dy;
	};
	const auto tileRejected = [&](const uint32_t quadrantIndex, const uint32_t tile, const uint32_t workFlags) {
		const auto& quadrant = quadrants[quadrantIndex];
		const auto& bounds = tileLocalBounds[tile];
		const float minX = quadrant.worldPos.x + bounds.x;
		const float minY = quadrant.worldPos.y + bounds.y;
		const float maxX = quadrant.worldPos.x + bounds.z;
		const float maxY = quadrant.worldPos.y + bounds.w;

		if ((workFlags & WorkInsideFrustum) == 0u && (workFlags & (WorkHasLand | WorkObjectSurface)) != 0u) {
			const auto& heightBounds = tileHeightBoundsStaging[quadrantIndex * OccupancyTileCount + tile];
			const bool hasTileBounds = heightBounds.x > QuadrantNoHeight && heightBounds.y >= heightBounds.x;
			const float minZ = (hasTileBounds ? heightBounds.x : quadrant.minHeight) - 256.0f;
			const float maxZ = (hasTileBounds ? heightBounds.y : quadrant.maxHeight) + 300.0f;
			const float3 center = { (minX + maxX) * 0.5f - cameraPosAdjust.x, (minY + maxY) * 0.5f - cameraPosAdjust.y,
				(minZ + maxZ) * 0.5f - cameraPosAdjust.z };
			const float3 extent = { (maxX - minX) * 0.5f + frustumPadding, (maxY - minY) * 0.5f + frustumPadding,
				(maxZ - minZ) * 0.5f + frustumPadding };
			for (const auto& plane : frustum.planes) {
				const float distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
				const float radius = std::abs(plane.x) * extent.x + std::abs(plane.y) * extent.y + std::abs(plane.z) * extent.z;
				if (distance + radius < 0.0f)
					return true;
			}
		}

		if (cullInnerFade && maxDistanceSqToRect(minX, minY, maxX, maxY) < innerFadeRadiusSq)
			return true;

		if (cullLowOuter) {
			const float closestX = std::clamp(lodOrigin.x, minX, maxX);
			const float closestY = std::clamp(lodOrigin.y, minY, maxY);
			if (std::max(std::abs(closestX - lodOrigin.x), std::abs(closestY - lodOrigin.y)) > lowOuterRadius)
				return true;
		}

		return cullFarOuter && minDistanceSqToRect(minX, minY, maxX, maxY) >= farOuterRadiusSq;
	};
	if (occupancyCache.size() > static_cast<size_t>(QuadrantCount) * 4u)
		occupancyCache.clear();

	// Bounds the generator's Far keep, GetFarLODKeep times the view-facing keep, over a rectangle. Beyond Low's band
	// each factor falls with distance, so its value at the rectangle's nearest point bounds the rectangle.
	const auto farKeepBound = [&](const float minX, const float minY, const float maxX, const float maxY) {
		const auto& farKeep = state.farKeepParams;
		const float padding = 4096.0f / density;  // Candidate jitter and the last row of an odd-density quadrant.
		const float dx = std::abs(std::clamp(lodOrigin.x, minX - padding, maxX + padding) - lodOrigin.x);
		const float dy = std::abs(std::clamp(lodOrigin.y, minY - padding, maxY + padding) - lodOrigin.y);
		const float distance = std::sqrt(dx * dx + dy * dy);
		const float squareDistance = std::max(dx, dy);
		const auto saturate = [](const float value) { return std::clamp(value, 0.0f, 1.0f); };
		// Mirrors GetFarViewFacingKeep: full through Low's band, easing to FarViewFacingKeep as the handoff fill fades.
		const float handoffEnd = lodFadeIn.x + 1.0f / std::max(lodFadeIn.y, 1.0e-6f);
		const float facingT = saturate((squareDistance - handoffEnd) / FarHandoffFillFade);
		const float facingKeep = std::lerp(1.0f, farKeep.w, facingT * facingT * (3.0f - 2.0f * facingT));
		if (squareDistance <= std::min(lodFadeOut.x, farKeep.x))
			return facingKeep;

		const float unload = lodFadeOut.w > 0.0f ? 1.0f - saturate((distance - lodFadeIn.w) * lodFadeOut.w) : 1.0f;
		const float edgeKeep = lodFadeOut.z > 1.0f ? lodFadeOut.z : std::lerp(1.0f, lodFadeOut.z, saturate((distance - lodFadeOut.x) * lodFadeOut.y));
		const float distanceKeep = std::lerp(1.0f, farKeep.z, saturate((distance - farKeep.x) * farKeep.y));
		const float seamKeep = 1.0f - saturate((squareDistance - handoffEnd) * lodFadeIn.y);
		return std::max(edgeKeep * distanceKeep, seamKeep) * unload * facingKeep;
	};

	// Far work that can reach Low's band, or the fill's fade beyond it, takes the handoff fill.
	const auto withFarHandoff = [&](const float minX, const float minY, const float maxX, const float maxY, const uint32_t flags) {
		if (!isFarTier || disableGeneratorCulls)
			return isFarTier ? flags | WorkFarHandoff : flags;
		// Include candidate jitter and the last row of an odd-density quadrant.
		const float padding = 4096.0f / density;
		const float dx = std::clamp(lodOrigin.x, minX - padding, maxX + padding) - lodOrigin.x;
		const float dy = std::clamp(lodOrigin.y, minY - padding, maxY + padding) - lodOrigin.y;
		return std::max(std::abs(dx), std::abs(dy)) < lodFadeOut.x + FarHandoffFillFade ? flags | WorkFarHandoff : flags & ~WorkFarHandoff;
	};

	const auto appendWork = [&](std::vector<uint32_t>& work, uint64_t& workRequiredBladeCount, const uint32_t patchCount, const uint32_t quadrantIndex, const uint32_t workFlags) {
		const uint32_t extraSlots = (workFlags & WorkFarHandoff) != 0u ? handoffExtraBlades : slopeExtraBlades;
		for (uint32_t lane = 0; lane < PatchBladeCount; ++lane) {
			work.push_back((quadrantIndex & WorkQuadrantMask) | lane << WorkLaneShift | workFlags);

			uint32_t ownedSlopeExtras = 0;
			if (extraSlots > lane)
				ownedSlopeExtras = 1u + (extraSlots - 1u - lane) / PatchBladeCount;
			workRequiredBladeCount += static_cast<uint64_t>(patchCount) * (1u + ownedSlopeExtras);
		}
	};

	for (uint32_t i = 0; i < quadrants.size(); ++i) {
		const auto& quadrant = quadrants[i];
		if (!quadrant.grassIds)
			continue;
		bool hasLand = quadrant.maxHeight > QuadrantNoHeight && quadrant.minHeight <= quadrant.maxHeight;
		auto frustumState = QuadrantFrustumState::Inside;
		if (!disableGeneratorCulls) {
			auto cullBounds = quadrant;
			if (objectHeightBounds[i].x > QuadrantNoHeight) {
				cullBounds.minHeight = hasLand ? std::min(cullBounds.minHeight, objectHeightBounds[i].x) : objectHeightBounds[i].x;
				cullBounds.maxHeight = hasLand ? std::max(cullBounds.maxHeight, objectHeightBounds[i].y) : objectHeightBounds[i].y;
			}
			bool hasCullBounds;
			frustumState = ClassifyQuadrantFrustum(cullBounds, frustum, cameraPosAdjust, frustumPadding, hasCullBounds);
			if (frustumState == QuadrantFrustumState::Outside)
				continue;
		}

		const float worldX = quadrant.worldPos.x;
		const float worldY = quadrant.worldPos.y;
		const float quadrantMaxX = worldX + 2048.0f;
		const float quadrantMaxY = worldY + 2048.0f;
		if (cullInnerFade && maxDistanceSqToRect(worldX, worldY, quadrantMaxX, quadrantMaxY) < innerFadeRadiusSq)
			continue;

		const float closestDx = std::clamp(lodOrigin.x, worldX, quadrantMaxX) - lodOrigin.x;
		const float closestDy = std::clamp(lodOrigin.y, worldY, quadrantMaxY) - lodOrigin.y;
		const float minDistanceSq = closestDx * closestDx + closestDy * closestDy;
		if (cullLowOuter && std::max(std::abs(closestDx), std::abs(closestDy)) > lowOuterRadius)
			continue;
		if (cullFarOuter && minDistanceSq >= farOuterRadiusSq)
			continue;

		uint32_t flags = (hasLand ? WorkHasLand : 0u) |
		                 (frustumState == QuadrantFrustumState::Inside ? WorkInsideFrustum : 0u) |
		                 (quadrant.nearCovered ? WorkNearCovered : 0u);
		const auto* occupancyRows = quadrantOccupancyStaging.data() + i * OccupancyTilesPerAxis;
		if (std::all_of(occupancyRows, occupancyRows + OccupancyTilesPerAxis, [](const uint32_t row) { return row == 0xFFFFu; }))
			flags |= WorkFullGrass;
		flags = withFarHandoff(worldX, worldY, quadrantMaxX, quadrantMaxY, flags);

		// Far work beyond the handoff generates only the share of its patches that its keep can retain.
		uint32_t shareStep = WorkShareSteps;
		if (isFarTier && !disableGeneratorCulls && (flags & WorkFarHandoff) == 0u && state.farKeepParams.w > 0.0f) {
			const float keepBound = farKeepBound(worldX, worldY, quadrantMaxX, quadrantMaxY);
			shareStep = std::clamp(static_cast<uint32_t>(std::ceil(keepBound * WorkShareSteps)), 1u, WorkShareSteps);
			if (shareStep < WorkShareSteps)
				flags |= WorkCompactFar;
		}

		if constexpr (PatchBladeCount > 1) {
			// High and Mid retain all lanes through their dithered tier transition.
			if (!extraDefine && !disableGeneratorCulls) {
				const float cullDistance = lodFadeOut.y > 0.0f ? lodFadeOut.x + 1.0f / lodFadeOut.y : lodFadeOut.x;
				if (minDistanceSq >= cullDistance * cullDistance)
					continue;
			}
		}

		legacyGroupCount += static_cast<uint64_t>(PatchBladeCount) * fullGX;
		if (disableGeneratorCulls) {
			visibleWorkCandidates.push_back({ i, flags, 0, 0, nullptr, shareStep });
			continue;
		}

		const auto& occupancy = GetOccupiedTiles(quadrant, i, state.edgeNoise);

		// Odd densities can place the final patch row beyond the nominal quadrant edge.
		const float tileMaxX = worldX + tileLocalBounds.back().z;
		const float tileMaxY = worldY + tileLocalBounds.back().w;
		const bool needsTileTests = (frustumState != QuadrantFrustumState::Inside && hasLand) ||
		                            (cullInnerFade && minDistanceSqToRect(worldX, worldY, tileMaxX, tileMaxY) < innerFadeRadiusSq) ||
		                            (cullLowOuter && std::max(std::max(std::abs(worldX - lodOrigin.x), std::abs(tileMaxX - lodOrigin.x)),
														 std::max(std::abs(worldY - lodOrigin.y), std::abs(tileMaxY - lodOrigin.y))) > lowOuterRadius) ||
		                            (cullFarOuter && maxDistanceSqToRect(worldX, worldY, tileMaxX, tileMaxY) >= farOuterRadiusSq);
		if (!needsTileTests) {
			tiledGroupCount += static_cast<uint64_t>(PatchBladeCount) * occupancy.occupiedTileCount * tileGX;
			visibleWorkCandidates.push_back({ i, flags, 0, occupancy.occupiedTileCount, occupancy.occupiedTiles.data(), shareStep });
			continue;
		}

		// Reuse these culling results when emitting the selected work layout.
		const uint32_t tileOffset = static_cast<uint32_t>(visibleTilesStaging.size());
		for (uint32_t tileIndex = 0; tileIndex < occupancy.occupiedTileCount; ++tileIndex) {
			const auto& tile = occupancy.occupiedTiles[tileIndex];
			if (!tileRejected(i, tile.tile, flags))
				visibleTilesStaging.push_back(tile);
		}
		const uint32_t visibleTileCount = static_cast<uint32_t>(visibleTilesStaging.size()) - tileOffset;
		tiledGroupCount += static_cast<uint64_t>(PatchBladeCount) * visibleTileCount * tileGX;
		visibleWorkCandidates.push_back({ i, flags, tileOffset, visibleTileCount, nullptr, shareStep });
	}

	const bool useOccupiedTiles = !disableGeneratorCulls &&
	                              ((cullInnerFade || cullLowOuter || cullFarOuter) ? tiledGroupCount * 8u <= legacyGroupCount * 7u : tiledGroupCount * 4u <= legacyGroupCount * 3u);
	// Far's base work is dispatched once per share, so order it by share; handoff work always takes the full share.
	if (isFarTier)
		std::stable_sort(visibleWorkCandidates.begin(), visibleWorkCandidates.end(), [](const auto& a, const auto& b) { return a.shareStep < b.shareStep; });
	baseWorkShareCounts.fill(0u);
	for (const auto& candidate : visibleWorkCandidates) {
		const size_t workStart = visibleWorkStaging.size();
		if (useOccupiedTiles) {
			for (uint32_t tileIndex = candidate.tileOffset; tileIndex < candidate.tileOffset + candidate.tileCount; ++tileIndex) {
				const auto& tile = candidate.cachedTiles ? candidate.cachedTiles[tileIndex] : visibleTilesStaging[tileIndex];
				const auto& quadrant = quadrants[candidate.quadrantIndex];
				const auto& bounds = tileLocalBounds[tile.tile];
				const uint32_t flags = (candidate.flags & WorkFarHandoff) == 0u ? candidate.flags :
				                                                                  withFarHandoff(quadrant.worldPos.x + bounds.x, quadrant.worldPos.y + bounds.y, quadrant.worldPos.x + bounds.z, quadrant.worldPos.y + bounds.w, candidate.flags);
				appendWork(visibleWorkStaging, requiredBladeCount, SharedPatchCount(tile.patchCount, candidate.shareStep), candidate.quadrantIndex,
					flags | WorkOccupiedTile | static_cast<uint32_t>(tile.tile) << WorkTileShift);
			}
		} else {
			appendWork(visibleWorkStaging, requiredBladeCount, SharedPatchCount(patchesPerQuadrant, candidate.shareStep), candidate.quadrantIndex, candidate.flags);
		}
		// Count base items per share; refined tiles can shed the handoff flag, so test each item.
		if (isFarTier) {
			for (size_t item = workStart; item < visibleWorkStaging.size(); ++item) {
				if ((visibleWorkStaging[item] & WorkFarHandoff) == 0u)
					++baseWorkShareCounts[candidate.shareStep - 1u];
			}
		}
	}

	// Far dispatches the handoff work first with its fill generator, then the base work by share. A stable partition
	// keeps the base work in share order.
	visibleHandoffWorkCount = 0u;
	if (isFarTier) {
		const auto handoffEnd = std::stable_partition(visibleWorkStaging.begin(), visibleWorkStaging.end(), [](const uint32_t task) { return (task & WorkFarHandoff) != 0u; });
		visibleHandoffWorkCount = static_cast<uint32_t>(handoffEnd - visibleWorkStaging.begin());
	}

	visibleTerrainWorkCount = static_cast<uint32_t>(visibleWorkStaging.size());

	// Object work follows LAND work and always uses occupied tiles.
	const auto* player = RE::PlayerCharacter::GetSingleton();
	const float2 objectOrigin = player ? float2(player->GetPosition().x, player->GetPosition().y) : lodOrigin;
	const int32_t objectOriginX = static_cast<int32_t>(std::floor(objectOrigin.x / 2048.0f));
	const int32_t objectOriginY = static_cast<int32_t>(std::floor(objectOrigin.y / 2048.0f));
	constexpr int32_t objectNearRadius = LowTierQuadrantRadius + LowTierStreamGuardQuadrants;
	for (uint32_t i = 0; i < quadrants.size(); ++i) {
		const auto& quadrant = quadrants[i];
		if (objectHeightBounds[i].x <= QuadrantNoHeight)
			continue;
		// Object coverage is independent of accepted LAND, including the grass beneath overhangs.
		const bool objectNearCovered = std::max(std::abs(quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x) - objectOriginX),
										   std::abs(quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y) - objectOriginY)) <= objectNearRadius;
		const auto& rows = objectOccupancyRows[i];
		for (uint32_t y = 0; y < OccupancyTilesPerAxis; ++y) {
			for (uint32_t x = 0; x < OccupancyTilesPerAxis; ++x) {
				if ((rows[y] & (1u << x)) == 0u)
					continue;
				const uint32_t tile = y * OccupancyTilesPerAxis + x;
				const auto& bounds = tileLocalBounds[tile];
				uint32_t flags = WorkObjectSurface | WorkOccupiedTile | WorkFullGrass | (tile << WorkTileShift) |
				                 (objectNearCovered ? WorkNearCovered : 0u) | (quadrant.heights ? WorkHasLand : 0u);
				flags = withFarHandoff(quadrant.worldPos.x + bounds.x, quadrant.worldPos.y + bounds.y,
					quadrant.worldPos.x + bounds.z, quadrant.worldPos.y + bounds.w, flags);
				if (!disableGeneratorCulls && tileRejected(i, tile, flags))
					continue;
				const uint32_t patchCount = tilePatchCounts[tile];
				if (patchCount)
					appendWork(visibleWorkStaging, requiredBladeCount, patchCount, i, flags);
			}
		}
	}

	visibleObjectHandoffCount = 0;
	if (isFarTier) {
		const auto firstObject = visibleWorkStaging.begin() + visibleTerrainWorkCount;
		const auto handoffEnd = std::stable_partition(firstObject, visibleWorkStaging.end(), [](uint32_t task) { return (task & WorkFarHandoff) != 0u; });
		visibleObjectHandoffCount = static_cast<uint32_t>(handoffEnd - firstObject);
	}
	cachedObjectGX = tileGX;

	if (!visibleWorkStaging.empty())
		visibleWorkSB->UpdatePartial(visibleWorkStaging.data(), visibleWorkStaging.size() * sizeof(uint32_t));
	cachedWorkGX = useOccupiedTiles ? tileGX : fullGX;
	cachedWorkPatchCount = useOccupiedTiles ? maxTilePatchWidth * maxTilePatchHeight : patchesPerQuadrant;
	cachedRequiredBladeCount = requiredBladeCount;
	lastWorkListState = state;
	hasCachedWorkList = true;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::DispatchGeneration(ID3D11DeviceContext* ctx, ID3D11ComputeShader* bladeGenerator, ID3D11ComputeShader* batchArgsGenerator)
{
	const uint32_t initialArgs[10] = {
		vertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t), 0, 0, 0, 0,
		outerVertexIndicesBuffer ? outerVertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t) : 0, 0, 0, 0, outerVertexIndicesBuffer ? bladeBufferCapacity : 0
	};
	ctx->UpdateSubresource(argsBuffer->resource.get(), 0, nullptr, initialArgs, 0, 0);

	ID3D11UnorderedAccessView* outputUAVs[2] = { bladesSB->UAV(), argsBuffer->uav.get() };
	ctx->CSSetUnorderedAccessViews(0, 2, outputUAVs, nullptr);

	auto* topDown = globals::topDownOcclusion;
	ID3D11ShaderResourceView* mapSRVs[5] = { topDown->GetHighSRV(), quadrantHeightSB->SRV(), topDown->GetLowSRV(), visibleWorkSB->SRV(), quadrantGrassCellsSB->SRV() };
	ctx->CSSetShaderResources(2, 5, mapSRVs);
	ID3D11ShaderResourceView* hiZSRV = globals::hiZPyramid->GetSRV();
	ctx->CSSetShaderResources(8, 1, &hiZSRV);
	ID3D11ShaderResourceView* boundsSRVs[2] = { tileHeightBoundsSB->SRV(), quadrantOccupancySB->SRV() };
	ctx->CSSetShaderResources(11, 2, boundsSRVs);

	if (std::string_view(lodDefine) == "HIGH_LOD") {
		auto& skylighting = globals::features::skylighting;
		ID3D11ShaderResourceView* skylightingSRV = skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
		ctx->CSSetShaderResources(50, 1, &skylightingSRV);
	}

	ID3D11ShaderResourceView* objectSurfaces = topDown->GetGrassSurfaceSRV();
	ctx->CSSetShaderResources(66, 1, &objectSurfaces);
	const auto rangeBuffer = workRangeCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &rangeBuffer);
	const auto dispatchRange = [&](ID3D11ComputeShader* generator, const uint32_t offset, const uint32_t count, const uint32_t groupsX, const float workShare) {
		if (!count)
			return;
		workRangeCB->Update(std::array<uint32_t, 4>{ offset, std::bit_cast<uint32_t>(workShare), 0u, 0u });
		ctx->CSSetShader(generator, nullptr, 0);
		ctx->Dispatch(groupsX, 1, count);
	};
	if (UsesGrassCollision(globals::features::grassCollision.loaded))
		globals::features::grassCollision.BindProceduralGrassGenerationResources(ctx);

	if (extraDefine) {
		auto* handoffGenerator = GetBladeGeneratorCS(true);
		dispatchRange(handoffGenerator ? handoffGenerator : bladeGenerator, 0u, visibleHandoffWorkCount, cachedWorkGX, 1.0f);

		// Base work generates only its share of each item's patches, one dispatch per share.
		uint32_t workOffset = visibleHandoffWorkCount;
		for (uint32_t step = 1; step <= WorkShareSteps; ++step) {
			const uint32_t count = baseWorkShareCounts[step - 1u];
			const uint32_t groupsX = (SharedPatchCount(cachedWorkPatchCount, step) + threadGroupSize - 1) / threadGroupSize;
			dispatchRange(bladeGenerator, workOffset, count, groupsX, static_cast<float>(step) / WorkShareSteps);
			workOffset += count;
		}

		dispatchRange(handoffGenerator ? handoffGenerator : bladeGenerator, visibleTerrainWorkCount, visibleObjectHandoffCount, cachedObjectGX, 1.0f);
		dispatchRange(bladeGenerator, visibleTerrainWorkCount + visibleObjectHandoffCount,
			static_cast<uint32_t>(visibleWorkStaging.size()) - visibleTerrainWorkCount - visibleObjectHandoffCount, cachedObjectGX, 1.0f);
	} else {
		dispatchRange(bladeGenerator, 0u, visibleTerrainWorkCount, cachedWorkGX, 1.0f);
		dispatchRange(bladeGenerator, visibleTerrainWorkCount, static_cast<uint32_t>(visibleWorkStaging.size()) - visibleTerrainWorkCount, cachedObjectGX, 1.0f);
	}
	ID3D11Buffer* nullRangeBuffer = nullptr;
	ctx->CSSetConstantBuffers(0, 1, &nullRangeBuffer);
	ID3D11ShaderResourceView* nullObjectSurfaces = nullptr;
	ctx->CSSetShaderResources(66, 1, &nullObjectSurfaces);

	ID3D11UnorderedAccessView* nullOutputUAVs[2] = {};
	ctx->CSSetUnorderedAccessViews(0, 2, nullOutputUAVs, nullptr);
	ID3D11ShaderResourceView* nullCollisionSRV = nullptr;
	ctx->CSSetShaderResources(100, 1, &nullCollisionSRV);
	if (batchArgsBuffer) {
		ID3D11ShaderResourceView* bladeArgsSRV = argsBuffer->srv.get();
		ctx->CSSetShaderResources(13, 1, &bladeArgsSRV);
		ID3D11UnorderedAccessView* batchArgsUAV = batchArgsBuffer->uav.get();
		ctx->CSSetUnorderedAccessViews(0, 1, &batchArgsUAV, nullptr);
		ctx->CSSetShader(batchArgsGenerator, nullptr, 0);
		ctx->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* nullBatchArgsUAV = nullptr;
		ctx->CSSetUnorderedAccessViews(0, 1, &nullBatchArgsUAV, nullptr);
		ID3D11ShaderResourceView* nullBladeArgsSRV = nullptr;
		ctx->CSSetShaderResources(13, 1, &nullBladeArgsSRV);
	}
	ID3D11ShaderResourceView* nullBoundsSRVs[2] = {};
	ctx->CSSetShaderResources(11, 2, nullBoundsSRVs);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
uint32_t PGrassRenderer<QuadrantCount, PatchBladeCount>::ReadBladeCount() const
{
	if (!argsStaging)
		return 0;

	auto ctx = globals::d3d::context;

	ctx->CopyResource(argsStaging.get(), argsBuffer->resource.get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(ctx->Map(argsStaging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
		return 0;

	const auto* args = static_cast<const uint32_t*>(mapped.pData);
	// Far's outer records are double blades.
	const uint32_t instanceCount = args[1] + args[6] * (extraDefine ? 2u : 1u);
	ctx->Unmap(argsStaging.get(), 0);

	return instanceCount;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderDepth(ID3D11DeviceContext* ctx, ID3D11PixelShader* depthClipPS)
{
	if (!bladesSB)
		return;

	const auto bladesSRV = bladesSB->SRV();
	ctx->VSSetShaderResources(0, 1, &bladesSRV);

	if (batchArgsBuffer) {
		ID3D11ShaderResourceView* argsSRV = argsBuffer->srv.get();
		ctx->VSSetShaderResources(1, 1, &argsSRV);
	}

	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
	ctx->VSSetShader(GetVertexShader(true, false), nullptr, 0);
	ctx->PSSetShader(depthClipPS, nullptr, 0);
	ID3D11Buffer* drawArgs = batchArgsBuffer ? batchArgsBuffer->resource.get() : argsBuffer->resource.get();
	ctx->DrawIndexedInstancedIndirect(drawArgs, 0);
	if (outerVertexIndicesBuffer) {
		ID3D11ShaderResourceView* argsSRV = argsBuffer->srv.get();
		ctx->VSSetShaderResources(1, 1, &argsSRV);
		ctx->IASetIndexBuffer(outerVertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
		ctx->VSSetShader(GetVertexShader(true, true), nullptr, 0);
		ctx->DrawIndexedInstancedIndirect(drawArgs, 5 * sizeof(uint32_t));
	}
	if (outerVertexIndicesBuffer || batchArgsBuffer) {
		ID3D11ShaderResourceView* nullArgsSRV = nullptr;
		ctx->VSSetShaderResources(1, 1, &nullArgsSRV);
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderGrass(ID3D11DeviceContext* ctx)
{
	if (!bladesSB)
		return;

	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);

	const auto bladesSRV = bladesSB->SRV();
	ctx->VSSetShaderResources(0, 1, &bladesSRV);

	if (batchArgsBuffer) {
		ID3D11ShaderResourceView* argsSRV = argsBuffer->srv.get();
		ctx->VSSetShaderResources(1, 1, &argsSRV);
	}
	ctx->VSSetShader(GetVertexShader(false, false), nullptr, 0);

	// Blades stay wet while Wetness Effects reports wet surfaces, including as they dry after the rain stops.
	auto& wetnessEffects = globals::features::wetnessEffects;
	bool hasRainWetness = false;
	if (wetnessEffects.loaded && wetnessEffects.settings.EnableWetnessEffects) {
		const auto wetnessData = wetnessEffects.GetCommonBufferData();
		hasRainWetness = wetnessData.Wetness > 0.0f || wetnessData.Raining > 0.0f;
	}

	ID3D11ShaderResourceView* objectSurfaces = globals::topDownOcclusion->GetGrassSurfaceSRV();
	ctx->PSSetShaderResources(66, 1, &objectSurfaces);
	if (extraDefine)
		ctx->VSSetShaderResources(66, 1, &objectSurfaces);
	const bool simpleLighting = UsesSimpleLighting();

	const bool noWetness = !simpleLighting && !extraDefine && wetnessEffects.loaded && !hasRainWetness;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool noLocalLights = !simpleLighting && !extraDefine && lightLimitFix.loaded && lightLimitFix.lightCount == 0 && lightLimitFix.strictLightDataTemp.NumStrictLights == 0;

	const bool innerHigh = outerVertexIndicesBuffer && std::string_view(lodDefine) == "HIGH_LOD";
	ctx->PSSetShader(GetPS(noWetness, noLocalLights, innerHigh), nullptr, 0);
	ID3D11Buffer* drawArgs = batchArgsBuffer ? batchArgsBuffer->resource.get() : argsBuffer->resource.get();
	ctx->DrawIndexedInstancedIndirect(drawArgs, 0);
	if (outerVertexIndicesBuffer) {
		ctx->PSSetShader(GetPS(noWetness, noLocalLights), nullptr, 0);
		ID3D11ShaderResourceView* argsSRV = argsBuffer->srv.get();
		ctx->VSSetShaderResources(1, 1, &argsSRV);
		ctx->IASetIndexBuffer(outerVertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
		ctx->VSSetShader(GetVertexShader(false, true), nullptr, 0);
		ctx->DrawIndexedInstancedIndirect(drawArgs, 5 * sizeof(uint32_t));
	}
	ID3D11ShaderResourceView* nullObjectSurface = nullptr;
	ctx->PSSetShaderResources(66, 1, &nullObjectSurface);
	if (extraDefine)
		ctx->VSSetShaderResources(66, 1, &nullObjectSurface);
	if (outerVertexIndicesBuffer || batchArgsBuffer) {
		ID3D11ShaderResourceView* nullArgsSRV = nullptr;
		ctx->VSSetShaderResources(1, 1, &nullArgsSRV);
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
bool PGrassRenderer<QuadrantCount, PatchBladeCount>::UsesSimpleLighting() const
{
	return std::string_view(lodDefine) == "LOW_LOD";
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::AppendVertexShaderDefines(ShaderDefines& defines) const
{
	defines.push_back({ vertCountDefine, nullptr });
	defines.push_back({ lodDefine, nullptr });
	if (UsesBatchedDraws())
		defines.push_back({ "BLADE_BATCH_SIZE", bladeBatchSizeString.c_str() });
	if (UsesGrassCollision(globals::features::grassCollision.loaded))
		defines.push_back({ "PGRASS_CACHED_COLLISION", nullptr });
	if (extraDefine)
		defines.push_back({ extraDefine, nullptr });
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11ComputeShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetBladeGeneratorCS(const bool farHandoff)
{
	auto*& generator = farHandoff ? handoffGeneratorCS : generatorCS;
	auto& compileAttempted = farHandoff ? handoffGeneratorCompileAttempted : generatorCompileAttempted;
	if (!generator && !compileAttempted) {
		compileAttempted = true;
		ShaderDefines defines;
		defines.push_back({ lodDefine, nullptr });
		defines.push_back({ "THREADGROUP_SIZE", threadGroupSizeString.c_str() });
		defines.push_back({ "DENSITY", densityString.c_str() });
		defines.push_back({ "QUADRANT_DATA_SIZE", quadrantCountString.c_str() });
		defines.push_back({ "PATCH_BLADE_COUNT", patchBladeCountString.c_str() });
		const std::string extraSlots = std::to_string(farHandoff ? handoffExtraBlades : slopeExtraBlades);
		defines.push_back({ "SLOPE_EXTRA_BLADES", extraSlots.c_str() });
		if (farHandoff)
			defines.push_back({ "PGRASS_FAR_HANDOFF", nullptr });
		if (outerVertexIndicesBuffer && UsesBatchedLow())
			defines.push_back({ "LOW_OUTER_GEOMETRY", nullptr });
		else if (outerVertexIndicesBuffer && UsesBatchedMid())
			defines.push_back({ "MID_OUTER_GEOMETRY", nullptr });
		else if (outerVertexIndicesBuffer && farHandoff)
			defines.push_back({ "FAR_DOUBLE_GEOMETRY", nullptr });

		if (std::string_view(lodDefine) == "HIGH_LOD" && globals::features::skylighting.loaded && globals::features::skylighting.texProbeArray)
			defines.push_back({ "SKYLIGHTING", nullptr });

		if constexpr (PatchBladeCount == 4) {
			defines.push_back({ "HIGH_GEOMETRY_LOD", nullptr });
			for (auto* feature : Feature::GetFeatureList()) {
				const auto featureName = feature->GetShaderDefineName();
				if (feature->loaded && (featureName == "TERRAIN_SHADOWS" || featureName == "CLOUD_SHADOWS"))
					defines.push_back({ featureName.data(), nullptr });
			}
		}

		if (UsesGrassCollision(globals::features::grassCollision.loaded))
			defines.push_back({ "PGRASS_CACHED_COLLISION", nullptr });

		if (extraDefine)
			defines.push_back({ extraDefine, nullptr });

		generator = CompileShader<ID3D11ComputeShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassBladeGeneratorCS.hlsl", defines, "cs_5_0");
	}

	return generator;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11ComputeShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetBatchArgsCS()
{
	if (!batchArgsCS) {
		ShaderDefines defines{ { "BLADE_BATCH_SIZE", bladeBatchSizeString.c_str() } };
		if (outerVertexIndicesBuffer)
			defines.push_back({ "LOW_OUTER_GEOMETRY", nullptr });
		batchArgsCS = CompileShader<ID3D11ComputeShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassBatchArgsCS.hlsl", defines, "cs_5_0");
	}
	return batchArgsCS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::AppendFeatureDefines(ShaderDefines& defines, const bool simpleLighting) const
{
	for (auto* feature : Feature::GetFeatureList()) {
		const auto featureName = feature->GetShaderDefineName();
		if (!feature->loaded || !feature->HasShaderDefine(RE::BSShader::Type::Lighting))
			continue;
		if (featureName == "SKYLIGHTING" && !globals::features::skylighting.texProbeArray)
			continue;

		// Simple lighting keeps only the colour-space and shadowing features it evaluates.
		// Far evaluates screen-space shadows only as Low's statistical stand-in, so it needs the define but not skylighting.
		if (simpleLighting && featureName != "LINEAR_LIGHTING" && featureName != "TERRAIN_SHADOWS" && featureName != "CLOUD_SHADOWS" &&
			featureName != "SCREEN_SPACE_SHADOWS" && (extraDefine || featureName != "SKYLIGHTING"))
			continue;
		defines.push_back({ featureName.data(), nullptr });
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetVertexShader(const bool depth, const bool outer)
{
	auto& shader = vertexShaders[(depth ? 2 : 0) + (outer ? 1 : 0)];
	if (shader)
		return shader;

	const bool high = std::string_view(lodDefine) == "HIGH_LOD";
	ShaderDefines defines;
	if (depth) {
		defines.push_back({ "DEPTH", nullptr });
		if (high)
			defines.push_back({ "DEPTH_CLIP", nullptr });
	} else {
		AppendFeatureDefines(defines, false);
	}

	if (outer && !UsesBatchedDraws()) {
		defines.push_back({ "HIGH_OUTER_VERTEX", nullptr });
		defines.push_back({ lodDefine, nullptr });
		if (UsesGrassCollision(globals::features::grassCollision.loaded))
			defines.push_back({ "PGRASS_CACHED_COLLISION", nullptr });
	} else {
		AppendVertexShaderDefines(defines);
		if (outer)
			defines.push_back({ UsesBatchedLow() ? "LOW_OUTER_VERTEX" : (extraDefine ? "FAR_DOUBLE_VERTEX" : "MID_OUTER_VERTEX"), nullptr });
		else if (high && !depth)
			defines.push_back({ "HIGH_INNER", nullptr });
	}

	shader = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	return shader;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11PixelShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetPS(bool noWetness, bool noLocalLights, bool innerHigh)
{
	const bool simpleLighting = UsesSimpleLighting();
	if (simpleLighting) {
		noWetness = false;
		noLocalLights = false;
	}
	innerHigh = innerHigh && std::string_view(lodDefine) == "HIGH_LOD";

	const size_t variant = (noWetness ? 1 : 0) + (noLocalLights ? 2 : 0) + (innerHigh ? 4 : 0);
	auto& selectedPS = pixelShaders[variant];
	if (!selectedPS) {
		ShaderDefines defines;
		AppendFeatureDefines(defines, simpleLighting);

		defines.push_back({ lodDefine, nullptr });
		defines.push_back({ vertCountDefine, nullptr });
		if (extraDefine)
			defines.push_back({ extraDefine, nullptr });

		if (noWetness)
			defines.push_back({ "PGRASS_DRY_WETNESS", nullptr });

		if (noLocalLights)
			defines.push_back({ "PGRASS_NO_LOCAL_LIGHTS", nullptr });

		if (innerHigh)
			defines.push_back({ "HIGH_INNER", nullptr });

		selectedPS = CompileShader<ID3D11PixelShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassPS.hlsl", defines, "ps_5_0");
	}

	return selectedPS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
template <typename ShaderT>
ShaderT* PGrassRenderer<QuadrantCount, PatchBladeCount>::CompileShader(const wchar_t* path, std::vector<std::pair<const char*, const char*>>& defines, const char* programType)
{
	auto list = BuildDefineList(defines);
	const std::wstring ws(path);
	std::string s = std::filesystem::path(ws).string();
	logger::info("[Procedural Grass] Compiling {} - {}", s, list);

	return static_cast<ShaderT*>(Util::CompileShader(path, defines, programType));
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
std::string PGrassRenderer<QuadrantCount, PatchBladeCount>::BuildDefineList(std::span<const std::pair<const char*, const char*>> defines)
{
	std::string out;
	out.reserve(defines.size() * 16);
	bool first = true;
	for (const auto& [name, value] : defines) {
		if (!first)
			out += ", ";
		first = false;

		out += name;

		if (value) {
			out += ' ';
			out += value;
		}
	}
	return out;
}

template class PGrassRenderer<HighTierQuadrantCap, 4>;
template class PGrassRenderer<MidTierQuadrantCap, MidPatchBladeCount>;
template class PGrassRenderer<LowTierQuadrantCap, 1>;
template class PGrassRenderer<FarQuadrantCount, 1>;
