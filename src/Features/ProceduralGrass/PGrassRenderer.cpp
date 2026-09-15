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

	QuadrantFrustumState ClassifyQuadrantFrustum(const Quadrant& quadrant, const float4x4& viewProj, const float4& cameraPosAdjust, float xyPadding, bool& hasLand)
	{
		hasLand = quadrant.maxHeight > QuadrantNoHeight && quadrant.minHeight <= quadrant.maxHeight;
		if (!hasLand)
			return QuadrantFrustumState::Intersecting;

		const float xs[2] = { quadrant.worldPos.x - xyPadding, quadrant.worldPos.x + 2048.0f + xyPadding };
		const float ys[2] = { quadrant.worldPos.y - xyPadding, quadrant.worldPos.y + 2048.0f + xyPadding };
		const float zs[2] = { quadrant.minHeight - 256.0f, quadrant.maxHeight + 300.0f };

		bool outsideLeft = true, outsideRight = true, outsideBottom = true, outsideTop = true, fullyInside = true;

		for (const float x : xs)
			for (const float y : ys)
				for (const float z : zs) {
					const float4 clip = float4::Transform(float4{ x - cameraPosAdjust.x, y - cameraPosAdjust.y, z - cameraPosAdjust.z, 1.0f }, viewProj);
					outsideLeft &= clip.x < -clip.w;
					outsideRight &= clip.x > clip.w;
					outsideBottom &= clip.y < -clip.w;
					outsideTop &= clip.y > clip.w;
					fullyInside &= clip.x >= -clip.w && clip.x <= clip.w && clip.y >= -clip.w && clip.y <= clip.w;
				}

		if (outsideLeft || outsideRight || outsideBottom || outsideTop)
			return QuadrantFrustumState::Outside;

		return fullyInside ? QuadrantFrustumState::Inside : QuadrantFrustumState::Intersecting;
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
PGrassRenderer<QuadrantCount, PatchBladeCount>::PGrassRenderer(const uint32_t grassDensity, const uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef, const uint32_t slopeExtra, const uint32_t bladeStride)
{
	vertexIndicesBuffer = vertexIndicesBuf;
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
	constexpr uint32_t packedGrassIds = (grassSampleCount + 3) / 4;  // grass ids are 0/1 bytes, packed 4 per uint
	quadrantGrassSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(packedGrassIds, true), packedGrassIds, "PGrass::QuadrantGrass");
	quadrantGrassSB->CreateSRV();
	quadrantGrassStaging.assign(static_cast<size_t>(packedGrassIds) * 4, 0u);  // padded to a whole uint

	constexpr uint32_t grassCellCount = QuadrantCount * (QuadrantGrassPitch - 1) * (QuadrantGrassPitch - 1);
	quadrantGrassCellsSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(grassCellCount, true), grassCellCount, "PGrass::QuadrantGrassCells");
	quadrantGrassCellsSB->CreateSRV();
	quadrantGrassCellsStaging.assign(grassCellCount, 0u);

	quadrantHeightSB = new StructuredBuffer(StructuredBufferDesc<float>(grassSampleCount, true), grassSampleCount, "PGrass::QuadrantHeights");
	quadrantHeightSB->CreateSRV();
	quadrantHeightStaging.assign(grassSampleCount, QuadrantNoHeight);

	constexpr uint32_t workItemCapacity = QuadrantCount * PatchBladeCount;
	visibleWorkSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(workItemCapacity, true), workItemCapacity, "PGrass::VisibleWork");
	visibleWorkSB->CreateSRV();
	visibleCompactWorkSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(workItemCapacity, true), workItemCapacity, "PGrass::VisibleCompactWork");
	visibleCompactWorkSB->CreateSRV();
	visibleWorkStaging.reserve(workItemCapacity);
	visibleCompactWorkStaging.reserve(workItemCapacity);

	D3D11_BUFFER_DESC argsBufferDesc{};
	argsBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	argsBufferDesc.CPUAccessFlags = 0;
	argsBufferDesc.BindFlags = 0;
	argsBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
	argsBufferDesc.ByteWidth = 5 * sizeof(uint32_t);

	const auto createIndirectArgs = [&](Buffer* indexBuffer, const char* name) {
		if (!indexBuffer)
			return static_cast<Buffer*>(nullptr);
		const uint32_t initialArgs[5] = {
			indexBuffer->desc.ByteWidth / sizeof(uint16_t),  // IndexCountPerInstance
			0,                                               // InstanceCount; overwritten by CopyStructureCount
			0,                                               // StartIndexLocation
			0,                                               // BaseVertexLocation
			0                                                // StartInstanceLocation
		};
		D3D11_SUBRESOURCE_DATA argsBufferInit{ initialArgs, 0, 0 };
		return new Buffer(argsBufferDesc, &argsBufferInit, name);
	};

	argsBuffer = createIndirectArgs(vertexIndicesBuffer, "PGrass::IndirectArgs");

	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.ByteWidth = 5 * sizeof(uint32_t);
	globals::d3d::device->CreateBuffer(&stagingDesc, nullptr, argsStaging.put());
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::SetDensity(uint32_t grassDensity)
{
	density = grassDensity;
	patchesPerQuadrant = grassDensity * grassDensity / 4;
	densityString = std::to_string(grassDensity);

	ResetBladeCapacity();

	ReleaseAndNull(bladeGeneratorCS);
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
		throw std::overflow_error("Procedural grass blade append buffer exceeds the D3D11 buffer-size limit");

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
	bladesSB->CreateUAV(true);
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
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::ClearShaderCache()
{
	ReleaseAndNull(bladeGeneratorCS);
	ReleaseAndNull(depthVS);
	ReleaseAndNull(vs);

	for (auto& pixelShader : pixelShaders)
		ReleaseAndNull(pixelShader);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<Quadrant>& quadrants, const int32_t cellXOffset, const int32_t cellYOffset, const float2& lodOrigin, const float4& lodFadeIn,
	const float4& lodFadeOut, const float frustumPadding, const bool disableGeneratorCulls, const float compactStartDistance, const float compactKeep)
{
	uint64_t hash = GrassHashOffsetBasis;
	const size_t quadrantCount = quadrants.size();
	GrassHashValue(hash, quadrantCount);
	GrassHashValue(hash, lodFadeIn.x);
	GrassHashValue(hash, lodFadeIn.y);
	GrassHashValue(hash, lodFadeIn.z);
	GrassHashValue(hash, lodFadeOut.x);
	GrassHashValue(hash, lodFadeOut.y);
	GrassHashValue(hash, lodFadeOut.z);

	for (const auto& q : quadrants) {
		const uint64_t cellKey = (static_cast<uint64_t>(static_cast<uint32_t>(q.cellX)) << 32) | static_cast<uint32_t>(q.cellY);
		const uint64_t quadrantKey = (static_cast<uint64_t>(q.x) << 32) | q.y;
		GrassHashValue(hash, cellKey);
		GrassHashValue(hash, quadrantKey);
		GrassHashValue(hash, q.cacheVersion);
	}

	if (!hasUploadedQuadrants || hash != lastUploadHash) {
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

			auto* dst = quadrantGrassStaging.data() + i * QuadrantGrassSamples;  // one grass id per byte
			if (generatorGrassIds)
				std::memcpy(dst, generatorGrassIds, QuadrantGrassSamples);
			else
				std::memset(dst, 0, QuadrantGrassSamples);

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
		quadrantGrassSB->UpdatePartial(quadrantGrassStaging.data(), activeSamples);  // one byte per sample, packed 4 per uint
		quadrantGrassCellsSB->UpdatePartial(quadrantGrassCellsStaging.data(), activeCells * sizeof(uint32_t));
		quadrantHeightSB->UpdatePartial(quadrantHeightStaging.data(), activeSamples * sizeof(float));

		lastUploadHash = hash;
		hasUploadedQuadrants = true;
	}

	const auto quadrantsBuffer = quadrantsCB->CB();
	ctx->CSSetConstantBuffers(7, 1, &quadrantsBuffer);

	// Build visible work lists and their per-work culling flags.
	visibleWorkStaging.clear();
	visibleCompactWorkStaging.clear();
	uint64_t requiredBladeCount = 0;
	const uint32_t compactPatchCount = std::max(1u, static_cast<uint32_t>(std::ceil(patchesPerQuadrant * std::clamp(compactKeep, 0.01f, 1.0f))));
	const float compactStartSq = compactStartDistance * compactStartDistance;
	const auto viewProj = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Transpose();
	const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();

	for (uint32_t i = 0; i < quadrants.size(); ++i) {
		bool hasLand = false;
		const auto frustumState = ClassifyQuadrantFrustum(quadrants[i], viewProj, cameraPosAdjust, frustumPadding, hasLand);
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

		for (uint32_t lane = 0; lane < PatchBladeCount; ++lane) {
			if constexpr (PatchBladeCount > 1) {
				// High and Mid retain all lanes through their dithered tier transition.
				if (!extraDefine && !disableGeneratorCulls) {
					const float cullDistance = lodFadeOut.y > 0.0f ? lodFadeOut.x + 1.0f / lodFadeOut.y : lodFadeOut.x;
					if (minDistanceSq >= cullDistance * cullDistance)
						continue;
				}
			}

			auto& work = compactFar ? visibleCompactWorkStaging : visibleWorkStaging;
			work.push_back((i & WorkQuadrantMask) | lane << WorkLaneShift | flags);

			// Count the conservative append capacity owned by this lane.
			uint32_t ownedSlopeExtras = 0;
			if (slopeExtraBlades > lane && (!extraDefine || (flags & WorkAllowSlopeExtras) != 0u))
				ownedSlopeExtras = 1u + (slopeExtraBlades - 1u - lane) / PatchBladeCount;
			requiredBladeCount += static_cast<uint64_t>(compactFar ? compactPatchCount : patchesPerQuadrant) * (1u + ownedSlopeExtras);
		}
	}

	if (!visibleWorkStaging.empty())
		visibleWorkSB->UpdatePartial(visibleWorkStaging.data(), visibleWorkStaging.size() * sizeof(uint32_t));
	if (!visibleCompactWorkStaging.empty())
		visibleCompactWorkSB->UpdatePartial(visibleCompactWorkStaging.data(), visibleCompactWorkStaging.size() * sizeof(uint32_t));

	EnsureBladeCapacity(requiredBladeCount);

	constexpr uint32_t initialCount = 0;
	const auto bladesUAV = bladesSB->UAV();
	ctx->CSSetUnorderedAccessViews(0, 1, &bladesUAV, &initialCount);

	auto* topDown = globals::topDownOcclusion;
	ID3D11ShaderResourceView* mapSRVs[6] = { quadrantGrassSB->SRV(), topDown->GetHighSRV(), quadrantHeightSB->SRV(), topDown->GetLowSRV(), visibleWorkSB->SRV(), quadrantGrassCellsSB->SRV() };
	ctx->CSSetShaderResources(1, 6, mapSRVs);
	ID3D11ShaderResourceView* hiZSRV = globals::hiZPyramid->GetSRV();
	ctx->CSSetShaderResources(8, 1, &hiZSRV);

	// Skylighting is only used by the High tier
	if constexpr (PatchBladeCount == 4) {
		auto& skylighting = globals::features::skylighting;
		ID3D11ShaderResourceView* skylightingSRV = skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
		ctx->CSSetShaderResources(50, 1, &skylightingSRV);
	}

	ctx->CSSetShader(GetBladeGeneratorCS(), nullptr, 0);
	if (UsesGrassCollision(globals::features::grassCollision.loaded))
		globals::features::grassCollision.BindProceduralGrassGenerationResources(ctx);

	// Pad the group count with groupSize - 1, so it doesn't get truncated. The shader discards any excess threads.
	const uint32_t gx = (patchesPerQuadrant + threadGroupSize - 1) / threadGroupSize;

	if (!visibleWorkStaging.empty())
		ctx->Dispatch(gx, 1, static_cast<uint32_t>(visibleWorkStaging.size()));
	if (!visibleCompactWorkStaging.empty()) {
		ID3D11ShaderResourceView* compactWorkSRV = visibleCompactWorkSB->SRV();
		ctx->CSSetShaderResources(5, 1, &compactWorkSRV);
		const uint32_t compactGX = (compactPatchCount + threadGroupSize - 1) / threadGroupSize;
		ctx->Dispatch(compactGX, 1, static_cast<uint32_t>(visibleCompactWorkStaging.size()));
	}

	ctx->CopyStructureCount(argsBuffer->resource.get(), sizeof(uint32_t), bladesSB->UAV());
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

	const uint32_t instanceCount = static_cast<const uint32_t*>(mapped.pData)[1];
	ctx->Unmap(argsStaging.get(), 0);

	return instanceCount;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderDepth(ID3D11DeviceContext* ctx, ID3D11PixelShader* depthClipPS)
{
	const auto bladesSRV = bladesSB->SRV();
	ctx->VSSetShaderResources(0, 1, &bladesSRV);

	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
	ctx->VSSetShader(GetDepthVS(), nullptr, 0);
	ctx->PSSetShader(depthClipPS, nullptr, 0);
	ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 0);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderGrass(ID3D11DeviceContext* ctx)
{
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

	ctx->PSSetShader(GetPS(noWetness, noLocalLights), nullptr, 0);
	ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 0);
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
	if (!bladeGeneratorCS) {
		ShaderDefines defines;
		defines.push_back({ lodDefine, nullptr });
		defines.push_back({ "THREADGROUP_SIZE", threadGroupSizeString.c_str() });
		defines.push_back({ "DENSITY", densityString.c_str() });
		defines.push_back({ "QUADRANT_DATA_SIZE", quadrantCountString.c_str() });
		defines.push_back({ "PATCH_BLADE_COUNT", patchBladeCountString.c_str() });
		defines.push_back({ "SLOPE_EXTRA_BLADES", slopeExtraBladesString.c_str() });

		if constexpr (PatchBladeCount == 4) {
			if (globals::features::skylighting.loaded && globals::features::skylighting.texProbeArray)
				defines.push_back({ "SKYLIGHTING", nullptr });
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

		vs = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}

	return vs;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11PixelShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetPS(bool noWetness, bool noLocalLights)
{
	const bool simpleLighting = UsesSimpleLighting();
	if (simpleLighting) {
		noWetness = false;
		noLocalLights = false;
	}

	const size_t variant = (noWetness ? 1 : 0) + (noLocalLights ? 2 : 0);
	auto& selectedPS = pixelShaders[variant];
	if (!selectedPS) {
		ShaderDefines defines;

		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting) &&
				(!simpleLighting || feature == &globals::features::linearLighting) &&
				(feature != &globals::features::skylighting || globals::features::skylighting.texProbeArray))
				defines.push_back({ feature->GetShaderDefineName().data(), nullptr });
		}

		defines.push_back({ lodDefine, nullptr });
		defines.push_back({ vertCountDefine, nullptr });
		if (extraDefine)
			defines.push_back({ extraDefine, nullptr });

		if (noWetness)
			defines.push_back({ "PGRASS_DRY_WETNESS", nullptr });

		if (noLocalLights)
			defines.push_back({ "PGRASS_NO_LOCAL_LIGHTS", nullptr });

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
