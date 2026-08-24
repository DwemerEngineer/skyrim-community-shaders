#include "PGrassRenderer.h"

#include "TopDownOcclusion.h"
#include "Features/ProceduralGrass.h"

#include "Features/GrassCollision.h"
#include "Features/LightLimitFix.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/WetnessEffects.h"
#include "ShaderCache.h"
#include "State.h"
#include "TerrainHeightMap.h"

using namespace PGrassCommon;
using namespace PGrassRendererQuads;

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
					outsideLeft &= clip.x < -clip.w; outsideRight &= clip.x > clip.w;
					outsideBottom &= clip.y < -clip.w; outsideTop &= clip.y > clip.w;
					fullyInside &= clip.x >= -clip.w && clip.x <= clip.w && clip.y >= -clip.w && clip.y <= clip.w;
				}

		if (outsideLeft || outsideRight || outsideBottom || outsideTop)
			return QuadrantFrustumState::Outside;

		return fullyInside ? QuadrantFrustumState::Inside : QuadrantFrustumState::Intersecting;
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
PGrassRenderer<QuadrantCount, PatchBladeCount>::PGrassRenderer(const uint32_t grassDensity, const uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef, const uint32_t slopeExtra, const uint32_t slopeExtraQuads, const uint32_t bladeStride, const uint32_t bladeCapacity)
{
	vertexIndicesBuffer = vertexIndicesBuf;
	lodDefine = lodDef;
	vertCountDefine = vertCountDef;
	extraDefine = extraDef;
	slopeExtraBlades = slopeExtra;
	slopeExtraQuadrants = slopeExtraQuads ? std::min(slopeExtraQuads, QuadrantCount) : QuadrantCount;
	slopeExtraBladesString = std::to_string(slopeExtra);
	bladeStrideBytes = bladeStride;
	bladeQuadrantCapacity = std::clamp(bladeCapacity, 1u, QuadrantCount);

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
	visibleWorkStaging.reserve(workItemCapacity);

	D3D11_BUFFER_DESC argsBufferDesc{};
	argsBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	argsBufferDesc.CPUAccessFlags = 0;
	argsBufferDesc.BindFlags = 0;
	argsBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
	argsBufferDesc.ByteWidth = 5 * sizeof(uint32_t);

	const uint32_t initialArgs[5] = {
		vertexIndicesBuffer->desc.ByteWidth / sizeof(uint16_t),  // IndexCountPerInstance
		0,                                                      // InstanceCount; overwritten by CopyStructureCount
		0,                                                      // StartIndexLocation
		0,                                                      // BaseVertexLocation
		0                                                       // StartInstanceLocation
	};

	D3D11_SUBRESOURCE_DATA argsBufferInit{ initialArgs, 0, 0 };
	argsBuffer = new Buffer(argsBufferDesc, &argsBufferInit, "PGrass::IndirectArgs");

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
	// Allocate base blades for every active quadrant and slope extras only for the configured quadrant subset.
	totalBladeCount = patchesPerQuadrant * (PatchBladeCount * bladeQuadrantCapacity + slopeExtraBlades * std::min(slopeExtraQuadrants, bladeQuadrantCapacity));
	densityString = std::to_string(grassDensity);

	// Match the buffer stride to this tier's packed Blade layout.
	auto bladesDesc = StructuredBufferDesc<Blade>(totalBladeCount, false);
	bladesDesc.StructureByteStride = bladeStrideBytes;
	bladesDesc.ByteWidth = bladeStrideBytes * totalBladeCount;

	delete bladesSB;
	bladesSB = new StructuredBuffer(bladesDesc, totalBladeCount, "PGrass::Blades");
	bladesSB->CreateUAV(true);
	bladesSB->CreateSRV();
	
	if (bladeGeneratorCS) {
		bladeGeneratorCS->Release();
		bladeGeneratorCS = nullptr;
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::SetBladeQuadrantCapacity(uint32_t quadrantCapacity)
{
	quadrantCapacity = std::clamp(quadrantCapacity, 1u, QuadrantCount);
	if (bladeQuadrantCapacity == quadrantCapacity)
		return;

	bladeQuadrantCapacity = quadrantCapacity;
	SetDensity(density);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::SetThreadGroupSize(uint32_t tgSize)
{
	threadGroupSize = tgSize;
	threadGroupSizeString = std::to_string(threadGroupSize);
	
	if (bladeGeneratorCS) {
		bladeGeneratorCS->Release();
		bladeGeneratorCS = nullptr;
	}
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::ClearShaderCache()
{
	if (bladeGeneratorCS) {
		bladeGeneratorCS->Release();
		bladeGeneratorCS = nullptr;
	}

	if (depthVS) {
		depthVS->Release();
		depthVS = nullptr;
	}

	if (vs) {
		vs->Release();
		vs = nullptr;
	}

	if (ps) {
		ps->Release();
		ps = nullptr;
	}

	if (noWetnessPS) {
		noWetnessPS->Release();
		noWetnessPS = nullptr;
	}

	if (noLocalLightsPS) {
		noLocalLightsPS->Release();
		noLocalLightsPS = nullptr;
	}

	if (noWetnessNoLocalLightsPS) {
		noWetnessNoLocalLightsPS->Release();
		noWetnessNoLocalLightsPS = nullptr;
	}

}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<Quadrant>& quadrants, const int32_t cellXOffset, const int32_t cellYOffset, const float4& lodFadeIn, 
	const float4& lodFadeOut, const uint64_t contentGeneration, const float frustumPadding, const bool disableGeneratorCulls)
{
	uint64_t hash = 14695981039346656037ull;
	const auto mix = [&hash](const uint64_t value) {
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		for (size_t i = 0; i < sizeof(value); ++i) {
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
	};

	const auto mixFloat = [&mix](float f) { uint32_t u; std::memcpy(&u, &f, sizeof(u)); mix(u); };

	mix(contentGeneration);
	mix(quadrants.size());
	mixFloat(lodFadeIn.x); mixFloat(lodFadeIn.y); mixFloat(lodFadeIn.z);
	mixFloat(lodFadeOut.x); mixFloat(lodFadeOut.y); mixFloat(lodFadeOut.z);

	for (const auto& q : quadrants) {
		mix((static_cast<uint64_t>(static_cast<uint32_t>(q.cellX)) << 32) | static_cast<uint32_t>(q.cellY));
		mix((static_cast<uint64_t>(q.x) << 32) | q.y);
		mix(reinterpret_cast<uintptr_t>(q.grassIds));  // changes when a far cell streams in / a LAND array is replaced
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

			auto* dst = quadrantGrassStaging.data() + i * QuadrantGrassSamples;  // one grass id per byte
			if (grassIds)
				std::memcpy(dst, grassIds, QuadrantGrassSamples);
			else
				std::memset(dst, 0, QuadrantGrassSamples);

			// Pack each 2x2 LAND cell for Near tiers into one uint so bilinear sampling needs one structured-buffer load.
			// Far keeps the byte-packed map for nearest-sample lookups.
			auto* cellDst = quadrantGrassCellsStaging.data() + i * (QuadrantGrassPitch - 1) * (QuadrantGrassPitch - 1);
			for (uint32_t y = 0; y < QuadrantGrassPitch - 1; ++y) {
				for (uint32_t x = 0; x < QuadrantGrassPitch - 1; ++x) {
					const uint32_t base = y * QuadrantGrassPitch + x;
					const uint32_t ll = grassIds ? grassIds[base] : 0u;
					const uint32_t lr = grassIds ? grassIds[base + 1] : 0u;
					const uint32_t ul = grassIds ? grassIds[base + QuadrantGrassPitch] : 0u;
					const uint32_t ur = grassIds ? grassIds[base + QuadrantGrassPitch + 1] : 0u;
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

	// Build a compact list of visible work items, excluding High and Mid lanes outside their fixed distance bands.
	// Per-work flags let the generator skip redundant per-blade tests.
	visibleWorkStaging.clear();
	const auto viewProj = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
	const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();

	for (uint32_t i = 0; i < quadrants.size(); ++i) {

		bool hasLand = false;
		const auto frustumState = ClassifyQuadrantFrustum(quadrants[i], viewProj, cameraPosAdjust, frustumPadding, hasLand);
		if (frustumState == QuadrantFrustumState::Outside)
			continue;

		const float worldX = quadrants[i].worldPos.x;
		const float worldY = quadrants[i].worldPos.y;
		const float closestX = std::clamp(cameraPosAdjust.x, worldX, worldX + 2048.0f);
		const float closestY = std::clamp(cameraPosAdjust.y, worldY, worldY + 2048.0f);
		const float minDistanceSq = (closestX - cameraPosAdjust.x) * (closestX - cameraPosAdjust.x) + (closestY - cameraPosAdjust.y) * (closestY - cameraPosAdjust.y);

		uint32_t flags = (hasLand ? WorkHasLand : 0u) | (frustumState == QuadrantFrustumState::Inside ? WorkInsideFrustum : 0u);
		if (extraDefine) {

			const float extraRange = lodFadeOut.x + 4096.0f + 1448.0f;
			const float dx = worldX + 1024.0f - cameraPosAdjust.x;
			const float dy = worldY + 1024.0f - cameraPosAdjust.y;

			if (dx * dx + dy * dy <= extraRange * extraRange)
				flags |= WorkAllowSlopeExtras;
		}

		for (uint32_t lane = 0; lane < PatchBladeCount; ++lane) {
			if constexpr (PatchBladeCount > 1) {
				// Low and Far use one lane and skip the near distance bands.
				// Base and slope-extra positions stay within quadrant bounds, so whole-work-item rejection is exact.
				if (!extraDefine && !disableGeneratorCulls) {
					const float cullDistance = static_cast<float>(8192u >> lane);
					if (minDistanceSq >= cullDistance * cullDistance)
						continue;
				}
			}

			visibleWorkStaging.push_back((i & WorkQuadrantMask) | lane << WorkLaneShift | flags);
		}
	}
	if (!visibleWorkStaging.empty())
		visibleWorkSB->UpdatePartial(visibleWorkStaging.data(), visibleWorkStaging.size() * sizeof(uint32_t));

	constexpr uint32_t initialCount = 0;
	const auto bladesUAV = bladesSB->UAV();
	ctx->CSSetUnorderedAccessViews(0, 1, &bladesUAV, &initialCount);

	auto* topDown = TopDownOcclusion::GetSingleton();
	ID3D11ShaderResourceView* mapSRVs[6] = { quadrantGrassSB->SRV(), topDown->GetHighSRV(), quadrantHeightSB->SRV(), topDown->GetLowSRV(), visibleWorkSB->SRV(), quadrantGrassCellsSB->SRV() };
	ctx->CSSetShaderResources(1, 6, mapSRVs);

	// Skylighting is only used by the High tier
	if constexpr (PatchBladeCount == 4) {
		auto& skylighting = globals::features::skylighting;
		ID3D11ShaderResourceView* skylightingSRV = skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
		ctx->CSSetShaderResources(50, 1, &skylightingSRV);
	}

	ctx->CSSetShader(GetBladeGeneratorCS(), nullptr, 0);

	// Pad the group count with groupSize - 1, so it doesn't get truncated. The shader discards any excess threads.
	const uint32_t gx = (patchesPerQuadrant + threadGroupSize - 1) / threadGroupSize;

	if (!visibleWorkStaging.empty())
		ctx->Dispatch(gx, 1, static_cast<uint32_t>(visibleWorkStaging.size()));

	ctx->CopyStructureCount(argsBuffer->resource.get(), sizeof(uint32_t), bladesSB->UAV());
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
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderDepth(ID3D11DeviceContext* ctx)
{
	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
	if (UsesGrassCollision(globals::features::grassCollision.loaded))
		globals::features::grassCollision.BindGrassShaderResources(ctx);

	const auto bladesSRV = bladesSB->SRV();
	ctx->VSSetShaderResources(0, 1, &bladesSRV);

	ctx->VSSetShader(GetDepthVS(), nullptr, 0);
	// Avoid setting a null pixel shader here, since the blending against the terrain needs to bind a PS
	ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 0);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderGrass(ID3D11DeviceContext* ctx)
{
	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);
	if (UsesGrassCollision(globals::features::grassCollision.loaded))
		globals::features::grassCollision.BindGrassShaderResources(ctx);

	const auto bladesSRV = bladesSB->SRV();
	ctx->VSSetShaderResources(0, 1, &bladesSRV);
	ctx->VSSetShader(GetVS(), nullptr, 0);

	auto& wetnessEffects = globals::features::wetnessEffects;
	const auto sky = globals::game::sky;
	const auto precipitation = sky ? sky->precip : nullptr;

	const bool hasRain = wetnessEffects.loaded && wetnessEffects.settings.EnableWetnessEffects && sky && sky->mode.get() == RE::Sky::Mode::kFull && precipitation &&
		(WetnessEffects::GetRainIntensity(precipitation->currentPrecip, sky->currentWeather) > 0.0f ||
			WetnessEffects::GetRainIntensity(precipitation->lastPrecip, sky->lastWeather) > 0.0f);

	const bool noWetness = !extraDefine && wetnessEffects.loaded && !hasRain;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool noLocalLights = !extraDefine && lightLimitFix.loaded && lightLimitFix.lightCount == 0 && lightLimitFix.strictLightDataTemp.NumStrictLights == 0;

	ctx->PSSetShader(GetPS(noWetness, noLocalLights), nullptr, 0);
	ctx->DrawIndexedInstancedIndirect(argsBuffer->resource.get(), 0);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11ComputeShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetBladeGeneratorCS()
{
	if (!bladeGeneratorCS) {

		std::vector<std::pair<const char*, const char*>> defines;
		defines.push_back({lodDefine, nullptr});
		defines.push_back({"THREADGROUP_SIZE", threadGroupSizeString.c_str()});
		defines.push_back({"DENSITY", densityString.c_str()});
		defines.push_back({"QUADRANT_DATA_SIZE", quadrantCountString.c_str()});
		defines.push_back({"PATCH_BLADE_COUNT", patchBladeCountString.c_str()});
		defines.push_back({"SLOPE_EXTRA_BLADES", slopeExtraBladesString.c_str()});

		if constexpr (PatchBladeCount == 4) {
			if (globals::features::skylighting.loaded && globals::features::skylighting.texProbeArray)
				defines.push_back({"SKYLIGHTING", nullptr});
		}

		if (extraDefine)
			defines.push_back({extraDefine, nullptr});
		
		bladeGeneratorCS = CompileShader<ID3D11ComputeShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassBladeGeneratorCS.hlsl", defines, "cs_5_0");
	}

	return bladeGeneratorCS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetDepthVS()
{
	if (!depthVS) {

		std::vector<std::pair<const char*, const char*>> defines;
		defines.push_back({ vertCountDefine, nullptr });
		defines.push_back({ "DEPTH", nullptr });
		defines.push_back({ lodDefine, nullptr });  // per-tier width boost (Low/Far); must match GetVS so depth == colour geometry

		if (UsesGrassCollision(globals::features::grassCollision.loaded))
			defines.push_back({ "GRASS_COLLISION", nullptr });
		if (extraDefine)
			defines.push_back({ extraDefine, nullptr });

		depthVS = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}

	return depthVS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetVS()
{
	if (!vs) {
		std::vector<std::pair<const char*, const char*>> defines;

		for (auto feature : Feature::GetFeatureList()) {
			if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting) &&
				(feature != &globals::features::skylighting || globals::features::skylighting.texProbeArray))
				defines.push_back({ feature->GetShaderDefineName().data(), nullptr });
		}

		defines.push_back({ vertCountDefine, nullptr });
		defines.push_back({ lodDefine, nullptr }); 

		if (UsesGrassCollision(globals::features::grassCollision.loaded))
			defines.push_back({ "GRASS_COLLISION", nullptr });

		if (extraDefine)
			defines.push_back({ extraDefine, nullptr });

		vs = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}

	return vs;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11PixelShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetPS(const bool noWetness, const bool noLocalLights)
{
	auto& selectedPS = noLocalLights ? (noWetness ? noWetnessNoLocalLightsPS : noLocalLightsPS) : (noWetness ? noWetnessPS : ps);
	if (!selectedPS) {

		std::vector<std::pair<const char*, const char*>> defines;

		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting) &&
				(feature != &globals::features::skylighting || globals::features::skylighting.texProbeArray))
				defines.push_back({ feature->GetShaderDefineName().data(), nullptr });
		}

		defines.push_back({ lodDefine, nullptr });
		defines.push_back({ vertCountDefine, nullptr });

		if (extraDefine)
			defines.push_back({ extraDefine, nullptr });  // FAR_LOD: lets the PS take a cheaper shading path for far grass

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
