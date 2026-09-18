#include "PGrassRenderer.h"

#include "Features/ProceduralGrass.h"
#include "TopDownOcclusion.h"

#include "Features/GrassCollision.h"
#include "HiZPyramid.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/WetnessEffects.h"
#include "ShaderCache.h"
#include "State.h"
#include "TerrainHeightMap.h"

using namespace PGrassCommon;
using namespace PGrassRendererQuads;

namespace
{
	template <class T>
	void ReleaseAndNull(T*& resource)
	{
		if (resource) {
			resource->Release();
			resource = nullptr;
		}
	}

	bool IsOccupiedGrassTile(const uint16_t* occupancyRows, const uint32_t patchStartX, const uint32_t patchEndX, const uint32_t patchStartY,
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

	QuadrantFrustumState ClassifyQuadrantFrustum(const Quadrant& quadrant, const SideFrustum& frustum, const float4& cameraPosAdjust, float xyPadding, bool& hasLand)
	{
		hasLand = quadrant.maxHeight > QuadrantNoHeight && quadrant.minHeight <= quadrant.maxHeight;
		if (!hasLand)
			return QuadrantFrustumState::Intersecting;

		const float minZ = quadrant.minHeight - 256.0f;
		const float maxZ = quadrant.maxHeight + 300.0f;
		const float3 center = { quadrant.worldPos.x + 1024.0f - cameraPosAdjust.x, quadrant.worldPos.y + 1024.0f - cameraPosAdjust.y, (minZ + maxZ) * 0.5f - cameraPosAdjust.z };
		const float3 extent = { 1024.0f + xyPadding, 1024.0f + xyPadding, (maxZ - minZ) * 0.5f };

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
	slopeExtraBladesString = std::to_string(slopeExtra);
	bladeStrideBytes = bladeStride;

	CreateArgsBuffer();
	SetDensity(grassDensity);
	SetThreadGroupSize(tgSize);

	GetBladeGeneratorCS();
	GetDepthVS();
	GetVS();
	if (outerVertexIndicesBuffer) {
		GetOuterDepthVS();
		GetOuterVS();
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
	quadrantsCB = new ConstantBuffer(ConstantBufferDesc<QuadrantDataArray<QuadrantCount>>());

	constexpr uint32_t grassSampleCount = QuadrantCount * QuadrantGrassSamples;

	constexpr uint32_t grassCellCount = QuadrantCount * (QuadrantGrassPitch - 1) * (QuadrantGrassPitch - 1);
	quadrantGrassCellsSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(grassCellCount, true), grassCellCount, "PGrass::QuadrantGrassCells");
	quadrantGrassCellsSB->CreateSRV();
	quadrantGrassCellsStaging.assign(grassCellCount, 0u);

	quadrantHeightSB = new StructuredBuffer(StructuredBufferDesc<float>(grassSampleCount, true), grassSampleCount, "PGrass::QuadrantHeights");
	quadrantHeightSB->CreateSRV();
	quadrantHeightStaging.assign(grassSampleCount, QuadrantNoHeight);

	constexpr uint32_t compactWorkItemCapacity = QuadrantCount * PatchBladeCount;
	constexpr uint32_t workItemCapacity = compactWorkItemCapacity * OccupancyTileCount;
	visibleWorkSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(workItemCapacity, true), workItemCapacity, "PGrass::VisibleWork");
	visibleWorkSB->CreateSRV();
	visibleCompactWorkSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(compactWorkItemCapacity, true), compactWorkItemCapacity, "PGrass::VisibleCompactWork");
	visibleCompactWorkSB->CreateSRV();
	visibleWorkStaging.reserve(workItemCapacity);
	visibleCompactWorkStaging.reserve(compactWorkItemCapacity);
	visibleWorkCandidates.reserve(QuadrantCount);
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

	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.ByteWidth = 10 * sizeof(uint32_t);
	globals::d3d::device->CreateBuffer(&stagingDesc, nullptr, argsStaging.put());
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::SetDensity(uint32_t grassDensity)
{
	density = grassDensity;
	patchesPerQuadrant = grassDensity * grassDensity / 4;
	densityString = std::to_string(grassDensity);
	occupancyCache.clear();

	ResetBladeCapacity();

	ReleaseAndNull(bladeGeneratorCS);
	bladeGeneratorCompileAttempted = false;
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
	const uint64_t maximumCandidateCount = allocationQuantum * QuadrantCount * (PatchBladeCount + slopeExtraBlades);
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
	threadGroupSizeString = std::to_string(threadGroupSize);

	ReleaseAndNull(bladeGeneratorCS);
	bladeGeneratorCompileAttempted = false;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::ClearShaderCache()
{
	ReleaseAndNull(bladeGeneratorCS);
	bladeGeneratorCompileAttempted = false;
	ReleaseAndNull(depthVS);
	ReleaseAndNull(outerDepthVS);
	ReleaseAndNull(vs);
	ReleaseAndNull(outerVS);

	for (auto& pixelShader : pixelShaders)
		ReleaseAndNull(pixelShader);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<Quadrant>& quadrants, const uint64_t contentVersion, const int32_t cellXOffset, const int32_t cellYOffset,
	const float2& lodOrigin, const float4& lodFadeIn, const float4& lodFadeOut, const float frustumPadding,
	const bool disableGeneratorCulls, const float compactStartDistance, const float compactKeep)
{
	auto* bladeGenerator = GetBladeGeneratorCS();
	if (!bladeGenerator) {
		const uint32_t emptyArgs[10] = {
			vertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t), 0, 0, 0, 0,
			outerVertexIndicesBuffer ? outerVertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t) : 0, 0, 0, 0, 0
		};
		ctx->UpdateSubresource(argsBuffer->resource.get(), 0, nullptr, emptyArgs, 0, 0);
		return;
	}

	const bool fadesChanged =
		lodFadeIn.x != lastUploadLodFadeIn.x || lodFadeIn.y != lastUploadLodFadeIn.y || lodFadeIn.z != lastUploadLodFadeIn.z || lodFadeIn.w != lastUploadLodFadeIn.w ||
		lodFadeOut.x != lastUploadLodFadeOut.x || lodFadeOut.y != lastUploadLodFadeOut.y || lodFadeOut.z != lastUploadLodFadeOut.z || lodFadeOut.w != lastUploadLodFadeOut.w;
	if (!hasUploadedQuadrants || contentVersion != lastUploadVersion || fadesChanged) {
		auto quadrantDataArray = QuadrantDataArray<QuadrantCount>{};
		quadrantDataArray.lodFadeIn = lodFadeIn;
		quadrantDataArray.lodFadeOut = lodFadeOut;

		for (uint32_t i = 0; i < quadrants.size(); i++) {
			const auto& quadrant = quadrants[i];
			auto grassIds = quadrant.grassIds;
			auto& quadrantData = quadrantDataArray.data[i];

			quadrantData.quadWorldPos = quadrant.worldPos;
			const uint32_t hashX = static_cast<uint32_t>((quadrant.cellX + cellXOffset) * 32 + quadrant.x * 16);
			const uint32_t hashY = static_cast<uint32_t>((quadrant.cellY + cellYOffset) * 32 + quadrant.y * 16);
			quadrantData.quadrantHash = QuadrantHash(hashX, hashY);
			quadrantData.flags = quadrant.maxHeight > QuadrantNoHeight ? WorkHasLand : 0u;

			// Fill each bare sample from one neighbouring grass sample. Read from the original map so the fill cannot spread farther.
			std::array<uint8_t, QuadrantGrassSamples> distantGrassIds{};
			const uint8_t* generatorGrassIds = grassIds;
			if (grassIds) {
				std::copy_n(grassIds, QuadrantGrassSamples, distantGrassIds.begin());
				const int32_t worldSampleBaseX = (quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x)) * static_cast<int32_t>(QuadrantGrassPitch - 1);
				const int32_t worldSampleBaseY = (quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y)) * static_cast<int32_t>(QuadrantGrassPitch - 1);

				for (uint32_t y = 0; y < QuadrantGrassPitch; ++y) {
					for (uint32_t x = 0; x < QuadrantGrassPitch; ++x) {
						const uint32_t sample = y * QuadrantGrassPitch + x;
						if (grassIds[sample] == 0) {
							distantGrassIds[sample] = FindAdjacentGrassId(grassIds, QuadrantGrassPitch, QuadrantGrassPitch, x, y,
								worldSampleBaseX + static_cast<int32_t>(x), worldSampleBaseY + static_cast<int32_t>(y));
						}
					}
				}
				generatorGrassIds = distantGrassIds.data();
			}


			// Pack each 2x2 LAND cell into one uint so bilinear sampling needs one structured-buffer load.
			auto* cellDst = quadrantGrassCellsStaging.data() + i * (QuadrantGrassPitch - 1) * (QuadrantGrassPitch - 1);
			for (uint32_t y = 0; y < QuadrantGrassPitch - 1; ++y) {
				for (uint32_t x = 0; x < QuadrantGrassPitch - 1; ++x) {
					const uint32_t base = y * QuadrantGrassPitch + x;
					const uint32_t ll = generatorGrassIds ? generatorGrassIds[base] : 0u;
					const uint32_t lr = generatorGrassIds ? generatorGrassIds[base + 1] : 0u;
					const uint32_t ul = generatorGrassIds ? generatorGrassIds[base + QuadrantGrassPitch] : 0u;
					const uint32_t ur = generatorGrassIds ? generatorGrassIds[base + QuadrantGrassPitch + 1] : 0u;
					cellDst[y * (QuadrantGrassPitch - 1) + x] = ll | lr << 8 | ul << 16 | ur << 24;
				}
			}

			auto* heightDst = quadrantHeightStaging.data() + i * QuadrantGrassSamples;
			if (quadrant.heights)
				std::copy_n(quadrant.heights, QuadrantGrassSamples, heightDst);
			else
				std::fill_n(heightDst, QuadrantGrassSamples, QuadrantNoHeight);
		}

		const size_t activeSamples = quadrants.size() * QuadrantGrassSamples;
		const size_t activeCells = quadrants.size() * (QuadrantGrassPitch - 1) * (QuadrantGrassPitch - 1);

		quadrantsCB->Update(&quadrantDataArray, offsetof(QuadrantDataArray<QuadrantCount>, data) + quadrants.size() * sizeof(QuadrantData));

		quadrantGrassCellsSB->UpdatePartial(quadrantGrassCellsStaging.data(), activeCells * sizeof(uint32_t));
		quadrantHeightSB->UpdatePartial(quadrantHeightStaging.data(), activeSamples * sizeof(float));

		lastUploadVersion = contentVersion;
		lastUploadLodFadeIn = lodFadeIn;
		lastUploadLodFadeOut = lodFadeOut;
		hasUploadedQuadrants = true;
	}

	const auto quadrantsBuffer = quadrantsCB->CB();
	ctx->CSSetConstantBuffers(7, 1, &quadrantsBuffer);

	// Build compact Far work and cache the visible quadrants needed by the normal generator pass.
	visibleWorkStaging.clear();
	visibleCompactWorkStaging.clear();
	visibleWorkCandidates.clear();
	uint64_t requiredBladeCount = 0;
	uint64_t compactRequiredBladeCount = 0;
	uint64_t tiledGroupCount = 0;
	uint64_t legacyGroupCount = 0;
	const uint32_t compactPatchCount = std::max(1u, static_cast<uint32_t>(std::ceil(patchesPerQuadrant * std::clamp(compactKeep, 0.01f, 1.0f))));
	const uint32_t patchesPerRow = density / 2u;
	const uint32_t patchRows = (patchesPerQuadrant + patchesPerRow - 1u) / patchesPerRow;
	const uint32_t maxTilePatchWidth = (patchesPerRow + OccupancyTilesPerAxis - 1u) / OccupancyTilesPerAxis;
	const uint32_t maxTilePatchHeight = (patchRows + OccupancyTilesPerAxis - 1u) / OccupancyTilesPerAxis;
	const uint32_t maxTilePatchCount = maxTilePatchWidth * maxTilePatchHeight;
	const uint32_t fullGX = (patchesPerQuadrant + threadGroupSize - 1u) / threadGroupSize;
	const uint32_t tileGX = (maxTilePatchCount + threadGroupSize - 1u) / threadGroupSize;
	const float compactStartSq = compactStartDistance * compactStartDistance;
	const auto viewProj = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Transpose();
	const auto frustum = BuildSideFrustum(viewProj);
	const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float grassMapEdgeNoise = globals::features::proceduralGrass.settings.grassMapEdgeNoise;
	if (occupancyCache.size() > static_cast<size_t>(QuadrantCount) * 4u)
		occupancyCache.clear();

	const auto appendWork = [&](std::vector<uint32_t>& work, uint64_t& workRequiredBladeCount, const uint32_t patchCount, const uint32_t quadrantIndex, const uint32_t workFlags) {
		for (uint32_t lane = 0; lane < PatchBladeCount; ++lane) {
			work.push_back((quadrantIndex & WorkQuadrantMask) | lane << WorkLaneShift | workFlags);

			uint32_t ownedSlopeExtras = 0;
			if (slopeExtraBlades > lane && (!extraDefine || (workFlags & WorkAllowSlopeExtras) != 0u))
				ownedSlopeExtras = 1u + (slopeExtraBlades - 1u - lane) / PatchBladeCount;
			workRequiredBladeCount += static_cast<uint64_t>(patchCount) * (1u + ownedSlopeExtras);
		}
	};

	for (uint32_t i = 0; i < quadrants.size(); ++i) {
		bool hasLand = false;
		const auto frustumState = ClassifyQuadrantFrustum(quadrants[i], frustum, cameraPosAdjust, frustumPadding, hasLand);
		if (frustumState == QuadrantFrustumState::Outside)
			continue;

		const float worldX = quadrants[i].worldPos.x;
		const float worldY = quadrants[i].worldPos.y;
		const float closestX = std::clamp(lodOrigin.x, worldX, worldX + 2048.0f);
		const float closestY = std::clamp(lodOrigin.y, worldY, worldY + 2048.0f);
		const float minDistanceSq = (closestX - lodOrigin.x) * (closestX - lodOrigin.x) + (closestY - lodOrigin.y) * (closestY - lodOrigin.y);

		uint32_t flags = (hasLand ? WorkHasLand : 0u) |
		                 (frustumState == QuadrantFrustumState::Inside ? WorkInsideFrustum : 0u) |
		                 (quadrants[i].nearCovered ? WorkNearCovered : 0u);
		if (extraDefine) {
			const float extraRange = lodFadeOut.x + 4096.0f + 1448.0f;
			const float dx = worldX + 1024.0f - lodOrigin.x;
			const float dy = worldY + 1024.0f - lodOrigin.y;

			if (dx * dx + dy * dy <= extraRange * extraRange)
				flags |= WorkAllowSlopeExtras;
		}

		const bool compactFar = extraDefine && compactStartDistance > 0.0f && minDistanceSq >= compactStartSq && compactPatchCount < patchesPerQuadrant;
		if (compactFar)
			flags |= WorkCompactFar;

		if constexpr (PatchBladeCount > 1) {
			// High and Mid retain all lanes through their dithered tier transition.
			if (!extraDefine && !disableGeneratorCulls) {
				const float cullDistance = lodFadeOut.y > 0.0f ? lodFadeOut.x + 1.0f / lodFadeOut.y : lodFadeOut.x;
				if (minDistanceSq >= cullDistance * cullDistance)
					continue;
			}
		}

		if (compactFar) {
			appendWork(visibleCompactWorkStaging, compactRequiredBladeCount, compactPatchCount, i, flags);
			continue;
		}

		legacyGroupCount += static_cast<uint64_t>(PatchBladeCount) * fullGX;
		if (disableGeneratorCulls) {
			visibleWorkCandidates.push_back({ i, flags, 0 });
			continue;
		}

		const int32_t worldQuadrantX = quadrants[i].cellX * 2 + static_cast<int32_t>(quadrants[i].x);
		const int32_t worldQuadrantY = quadrants[i].cellY * 2 + static_cast<int32_t>(quadrants[i].y);
		const uint64_t occupancyKey = static_cast<uint64_t>(static_cast<uint32_t>(worldQuadrantX)) << 32 | static_cast<uint32_t>(worldQuadrantY);
		auto& occupancy = occupancyCache[occupancyKey];
		if (occupancy.cacheVersion != quadrants[i].cacheVersion || occupancy.density != density || occupancy.edgeNoise != grassMapEdgeNoise) {
			occupancy.occupiedTileCount = 0;
			for (uint32_t tileY = 0; tileY < OccupancyTilesPerAxis; ++tileY) {
				const uint32_t patchStartY = tileY * patchRows / OccupancyTilesPerAxis;
				const uint32_t patchEndY = (tileY + 1u) * patchRows / OccupancyTilesPerAxis;
				for (uint32_t tileX = 0; tileX < OccupancyTilesPerAxis; ++tileX) {
					const uint32_t patchStartX = tileX * patchesPerRow / OccupancyTilesPerAxis;
					const uint32_t patchEndX = (tileX + 1u) * patchesPerRow / OccupancyTilesPerAxis;
					const uint32_t tilePatchCount = (patchEndX - patchStartX) * (patchEndY - patchStartY);
					if (tilePatchCount == 0u || !IsOccupiedGrassTile(quadrants[i].occupancyRows, patchStartX, patchEndX, patchStartY, patchEndY, density, grassMapEdgeNoise))
						continue;

					const uint32_t tile = tileY * OccupancyTilesPerAxis + tileX;
					occupancy.occupiedTiles[occupancy.occupiedTileCount++] = { static_cast<uint16_t>(tile), static_cast<uint16_t>(tilePatchCount) };
				}
			}
			occupancy.cacheVersion = quadrants[i].cacheVersion;
			occupancy.density = density;
			occupancy.edgeNoise = grassMapEdgeNoise;
		}

		tiledGroupCount += static_cast<uint64_t>(PatchBladeCount) * occupancy.occupiedTileCount * tileGX;
		visibleWorkCandidates.push_back({ i, flags, occupancyKey });
	}

	const bool useOccupiedTiles = !disableGeneratorCulls && tiledGroupCount * 4u <= legacyGroupCount * 3u;
	for (const auto& candidate : visibleWorkCandidates) {
		if (useOccupiedTiles) {
			const auto& occupancy = occupancyCache.at(candidate.occupancyKey);
			for (uint32_t tileIndex = 0; tileIndex < occupancy.occupiedTileCount; ++tileIndex) {
				const auto& tile = occupancy.occupiedTiles[tileIndex];
				appendWork(visibleWorkStaging, requiredBladeCount, tile.patchCount, candidate.quadrantIndex,
					candidate.flags | WorkOccupiedTile | static_cast<uint32_t>(tile.tile) << WorkTileShift);
			}
		} else {
			appendWork(visibleWorkStaging, requiredBladeCount, patchesPerQuadrant, candidate.quadrantIndex, candidate.flags);
		}
	}

	const uint32_t workGX = useOccupiedTiles ? tileGX : fullGX;
	requiredBladeCount += compactRequiredBladeCount;

	if (!visibleWorkStaging.empty())
		visibleWorkSB->UpdatePartial(visibleWorkStaging.data(), visibleWorkStaging.size() * sizeof(uint32_t));
	if (!visibleCompactWorkStaging.empty())
		visibleCompactWorkSB->UpdatePartial(visibleCompactWorkStaging.data(), visibleCompactWorkStaging.size() * sizeof(uint32_t));

	EnsureBladeCapacity(requiredBladeCount);

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

	// Skylighting is only used by the High tier
	if constexpr (PatchBladeCount == 4) {
		auto& skylighting = globals::features::skylighting;
		ID3D11ShaderResourceView* skylightingSRV = skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
		ctx->CSSetShaderResources(50, 1, &skylightingSRV);
	}

	ctx->CSSetShader(bladeGenerator, nullptr, 0);
	if (UsesGrassCollision(globals::features::grassCollision.loaded))
		globals::features::grassCollision.BindProceduralGrassGenerationResources(ctx);

	if (!visibleWorkStaging.empty())
		ctx->Dispatch(workGX, 1, static_cast<uint32_t>(visibleWorkStaging.size()));
	if (!visibleCompactWorkStaging.empty()) {
		ID3D11ShaderResourceView* compactWorkSRV = visibleCompactWorkSB->SRV();
		ctx->CSSetShaderResources(5, 1, &compactWorkSRV);
		const uint32_t compactGX = (compactPatchCount + threadGroupSize - 1) / threadGroupSize;
		ctx->Dispatch(compactGX, 1, static_cast<uint32_t>(visibleCompactWorkStaging.size()));
	}

	ID3D11UnorderedAccessView* nullOutputUAVs[2] = {};
	ctx->CSSetUnorderedAccessViews(0, 2, nullOutputUAVs, nullptr);
	ID3D11ShaderResourceView* nullCollisionSRV = nullptr;
	ctx->CSSetShaderResources(100, 1, &nullCollisionSRV);
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
	const uint32_t instanceCount = args[1] + args[6];
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

	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
	ctx->VSSetShader(GetDepthVS(), nullptr, 0);
	ctx->PSSetShader(depthClipPS, nullptr, 0);
	ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 0);
	if (outerVertexIndicesBuffer) {
		ID3D11ShaderResourceView* argsSRV = argsBuffer->srv.get();
		ctx->VSSetShaderResources(1, 1, &argsSRV);
		ctx->IASetIndexBuffer(outerVertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
		ctx->VSSetShader(GetOuterDepthVS(), nullptr, 0);
		ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 5 * sizeof(uint32_t));
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
	ctx->VSSetShader(GetVS(), nullptr, 0);

	auto& wetnessEffects = globals::features::wetnessEffects;
	const auto sky = globals::game::sky;
	const auto precipitation = sky ? sky->precip : nullptr;

	const bool hasRain = wetnessEffects.loaded && wetnessEffects.settings.EnableWetnessEffects && sky && sky->mode.get() == RE::Sky::Mode::kFull && precipitation &&
	                     (WetnessEffects::GetRainIntensity(precipitation->currentPrecip, sky->currentWeather) > 0.0f ||
	                      WetnessEffects::GetRainIntensity(precipitation->lastPrecip, sky->lastWeather) > 0.0f);

	const bool simpleLighting = UsesSimpleLighting();

	const bool noWetness = !simpleLighting && !extraDefine && wetnessEffects.loaded && !hasRain;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool noLocalLights = !simpleLighting && !extraDefine && lightLimitFix.loaded && lightLimitFix.lightCount == 0 && lightLimitFix.strictLightDataTemp.NumStrictLights == 0;

	const bool innerHigh = outerVertexIndicesBuffer && std::string_view(lodDefine) == "HIGH_LOD";
	ctx->PSSetShader(GetPS(noWetness, noLocalLights, innerHigh), nullptr, 0);
	ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 0);
	if (outerVertexIndicesBuffer) {
		ctx->PSSetShader(GetPS(noWetness, noLocalLights), nullptr, 0);
		ID3D11ShaderResourceView* argsSRV = argsBuffer->srv.get();
		ctx->VSSetShaderResources(1, 1, &argsSRV);
		ctx->IASetIndexBuffer(outerVertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
		ctx->VSSetShader(GetOuterVS(), nullptr, 0);
		ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 5 * sizeof(uint32_t));
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
	if (UsesGrassCollision(globals::features::grassCollision.loaded))
		defines.push_back({ "PGRASS_CACHED_COLLISION", nullptr });
	if (extraDefine)
		defines.push_back({ extraDefine, nullptr });
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11ComputeShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetBladeGeneratorCS()
{
	if (!bladeGeneratorCS && !bladeGeneratorCompileAttempted) {
		bladeGeneratorCompileAttempted = true;
		ShaderDefines defines;
		defines.push_back({ lodDefine, nullptr });
		defines.push_back({ "THREADGROUP_SIZE", threadGroupSizeString.c_str() });
		defines.push_back({ "DENSITY", densityString.c_str() });
		defines.push_back({ "QUADRANT_DATA_SIZE", quadrantCountString.c_str() });
		defines.push_back({ "PATCH_BLADE_COUNT", patchBladeCountString.c_str() });
		defines.push_back({ "SLOPE_EXTRA_BLADES", slopeExtraBladesString.c_str() });

		if constexpr (PatchBladeCount == 4) {
			defines.push_back({ "HIGH_GEOMETRY_LOD", nullptr });
			if (globals::features::skylighting.loaded && globals::features::skylighting.texProbeArray)
				defines.push_back({ "SKYLIGHTING", nullptr });
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

		bladeGeneratorCS = CompileShader<ID3D11ComputeShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassBladeGeneratorCS.hlsl", defines, "cs_5_0");
	}

	return bladeGeneratorCS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetDepthVS()
{
	if (!depthVS) {
		ShaderDefines defines;
		defines.push_back({ "DEPTH", nullptr });
		AppendVertexShaderDefines(defines);

		depthVS = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}

	return depthVS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetOuterDepthVS()
{
	if (!outerDepthVS) {
		ShaderDefines defines{ { "DEPTH", nullptr }, { "HIGH_OUTER_VERTEX", nullptr }, { lodDefine, nullptr } };
		if (UsesGrassCollision(globals::features::grassCollision.loaded))
			defines.push_back({ "PGRASS_CACHED_COLLISION", nullptr });
		outerDepthVS = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}

	return outerDepthVS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetVS()
{
	if (!vs) {
		ShaderDefines defines;

		for (auto feature : Feature::GetFeatureList()) {
			if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting) &&
				(feature != &globals::features::skylighting || globals::features::skylighting.texProbeArray))
				defines.push_back({ feature->GetShaderDefineName().data(), nullptr });
		}

		AppendVertexShaderDefines(defines);
		if (std::string_view(lodDefine) == "HIGH_LOD")
			defines.push_back({ "HIGH_INNER", nullptr });

		vs = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}

	return vs;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetOuterVS()
{
	if (!outerVS) {
		ShaderDefines defines;

		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting) &&
				(feature != &globals::features::skylighting || globals::features::skylighting.texProbeArray))
				defines.push_back({ feature->GetShaderDefineName().data(), nullptr });
		}

		defines.push_back({ "HIGH_OUTER_VERTEX", nullptr });
		defines.push_back({ lodDefine, nullptr });
		if (UsesGrassCollision(globals::features::grassCollision.loaded))
			defines.push_back({ "PGRASS_CACHED_COLLISION", nullptr });

		outerVS = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}

	return outerVS;
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

		for (auto* feature : Feature::GetFeatureList()) {
			const auto featureName = feature->GetShaderDefineName();
			const bool requiredSimpleLightingFeature =
				featureName == "LINEAR_LIGHTING" ||
				featureName == "TERRAIN_SHADOWS" ||
				featureName == "CLOUD_SHADOWS";
			if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting) &&
				(!simpleLighting || requiredSimpleLightingFeature) &&
				(featureName != "SKYLIGHTING" || globals::features::skylighting.texProbeArray))
				defines.push_back({ featureName.data(), nullptr });
		}

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
	logger::info("[Procedural Grass] Compiling {} – {}", s, list);

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
template class PGrassRenderer<MidTierQuadrantCap, 2>;
template class PGrassRenderer<LowTierQuadrantCap, 1>;
template class PGrassRenderer<FarQuadrantCount, 1>;
