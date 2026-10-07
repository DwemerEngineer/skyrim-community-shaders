#include "Features/ProceduralGrass.h"

#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "HiZPyramid.h"
#include "State.h"
#include "Utils/D3D.h"

using namespace PGrassCommon;

void ProceduralGrass::CreateTerrainCanopyResources()
{
	auto* renderer = globals::game::renderer;
	D3D11_TEXTURE2D_DESC mainDesc{};
	renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(&mainDesc);
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{ mainDesc.Format, 0 };
	terrainCanopyTypedLoadSupported = SUCCEEDED(globals::d3d::device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))) &&
	                                  (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
	if (!terrainCanopyTypedLoadSupported) {
		logger::info("[Procedural Grass] Terrain canopy unavailable: scene format does not support typed UAV loads.");
		return;
	}

	if (!terrainCanopyTexture) {
		terrainCanopyTexture = Util::CreateSquareTexture(TerrainCanopyDim, DXGI_FORMAT_R32_UINT, true, "PGrass::TerrainCanopyTypes");
		terrainCanopyPending.reserve(FarQuadrantCount);
	}
}

void ProceduralGrass::UpdateTerrainCanopy(ID3D11DeviceContext* ctx)
{
	if (!terrainCanopyTexture || !terrainCanopyCS)
		return;

	const auto* player = RE::PlayerCharacter::GetSingleton();
	const auto& position = player->GetPosition();
	const int32_t originX = static_cast<int32_t>(std::floor(position.x / 2048.0f)) - TerrainCanopyQuadrants / 2;
	const int32_t originY = static_cast<int32_t>(std::floor(position.y / 2048.0f)) - TerrainCanopyQuadrants / 2;
	const bool moved = terrainCanopyOrigin[0] != originX || terrainCanopyOrigin[1] != originY;
	bool reset = terrainCanopyNeedsClear || terrainCanopyWorldSpace != farRequestWorldSpace ||
	             terrainCanopyPolicyVersion != grassMapCacheVersion ||
	             std::abs(static_cast<int64_t>(originX) - terrainCanopyOrigin[0]) >= TerrainCanopyQuadrants ||
	             std::abs(static_cast<int64_t>(originY) - terrainCanopyOrigin[1]) >= TerrainCanopyQuadrants;
	constexpr uint32_t slotMask = TerrainCanopyQuadrants - 1;
	// Long travel through unavailable LAND can leave a slot untouched for a complete coordinate-tag period.
	if (moved && !reset) {
		for (int32_t y = originY; y < originY + TerrainCanopyQuadrants && !reset; ++y) {
			for (int32_t x = originX; x < originX + TerrainCanopyQuadrants; ++x) {
				const auto& tile = terrainCanopyTiles[(static_cast<uint32_t>(y) & slotMask) * TerrainCanopyQuadrants + (static_cast<uint32_t>(x) & slotMask)];
				if (tile.version && (tile.x != x || tile.y != y) && ((tile.x ^ x) & 2047) == 0 && ((tile.y ^ y) & 2047) == 0) {
					reset = true;
					break;
				}
			}
		}
	}
	if (reset) {
		const UINT empty[4]{};
		ctx->ClearUnorderedAccessViewUint(terrainCanopyTexture->uav.get(), empty);
		terrainCanopyTiles.fill(CanopyTile{});
		terrainCanopyNeedsClear = false;
	}
	terrainCanopyWorldSpace = farRequestWorldSpace;
	terrainCanopyPolicyVersion = grassMapCacheVersion;
	terrainCanopyOrigin[0] = originX;
	terrainCanopyOrigin[1] = originY;

	constexpr uint32_t pitch = QuadrantCellPitch;
	if (reset || moved || terrainCanopyQuadrantsVersion != quadrantsFarVersion) {
		terrainCanopyPending.clear();
		terrainCanopyUploadCursor = 0;
		for (const auto& quadrant : quadrantsFarLOD) {
			const int32_t x = quadrant.cellX * 2 + static_cast<int32_t>(quadrant.x);
			const int32_t y = quadrant.cellY * 2 + static_cast<int32_t>(quadrant.y);
			if (!quadrant.grassIds || x < originX || y < originY || x >= originX + TerrainCanopyQuadrants || y >= originY + TerrainCanopyQuadrants)
				continue;
			const uint32_t slot = (static_cast<uint32_t>(y) & slotMask) * TerrainCanopyQuadrants + (static_cast<uint32_t>(x) & slotMask);
			const auto& previous = terrainCanopyTiles[slot];
			if (previous.x == x && previous.y == y && previous.version == quadrant.cacheVersion)
				continue;

			auto& upload = terrainCanopyPending.emplace_back();
			upload.tile = { x, y, quadrant.cacheVersion };
			// Coordinate tags distinguish stale wrapping slots; teleports clear the window before tags can alias.
			const uint32_t tag = (1u << 30) | ((static_cast<uint32_t>(x) & 2047u) << 8) | ((static_cast<uint32_t>(y) & 2047u) << 19);
			const auto grassIds = FillQuadrantGrassIds(quadrant.grassIds, x, y);
			for (uint32_t row = 0; row < pitch; ++row) {
				for (uint32_t column = 0; column < pitch; ++column) {
					const uint8_t id = grassIds[row * QuadrantGrassPitch + column];
					upload.samples[row * pitch + column] = tag | LimitGrassIdToSlope(id, quadrant, column, row);
				}
			}
		}
		terrainCanopyQuadrantsVersion = quadrantsFarVersion;
	}

	// LAND completions already have a frame budget. Bound initial fills and scrolling uploads as well.
	constexpr size_t uploadBudget = 64;
	const size_t uploadEnd = std::min(terrainCanopyUploadCursor + uploadBudget, terrainCanopyPending.size());
	for (; terrainCanopyUploadCursor < uploadEnd; ++terrainCanopyUploadCursor) {
		const auto& upload = terrainCanopyPending[terrainCanopyUploadCursor];
		const uint32_t slotX = static_cast<uint32_t>(upload.tile.x) & slotMask;
		const uint32_t slotY = static_cast<uint32_t>(upload.tile.y) & slotMask;
		const D3D11_BOX box{ slotX * pitch, slotY * pitch, 0, (slotX + 1) * pitch, (slotY + 1) * pitch, 1 };
		ctx->UpdateSubresource(terrainCanopyTexture->resource.get(), 0, &box, upload.samples.data(), pitch * sizeof(uint32_t), 0);
		terrainCanopyTiles[slotY * TerrainCanopyQuadrants + slotX] = upload.tile;
	}
}

void ProceduralGrass::RenderTerrainCanopy(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer) const
{
	if (grassGlobalsStaging->terrainCanopyParams.y == 0.0f)
		return;

	globals::profiler->BeginPass("ProceduralGrass::Terrain Canopy");
	ctx->OMSetRenderTargets(0, nullptr, nullptr);
	UpdateDistantAmbientLUT(ctx);

	auto& targets = renderer->GetRuntimeData().renderTargets;
	ID3D11ShaderResourceView* depth = Util::GetCurrentSceneDepthSRV(false);
	ID3D11ShaderResourceView* mask = targets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS_DOWNSCALED].SRV;
	ID3D11ShaderResourceView* shadow = targets[RE::RENDER_TARGETS::kSHADOW_MASK].SRV;
	ID3D11ShaderResourceView* hiZ = globals::hiZPyramid->GetSRV();
	ID3D11ShaderResourceView* types = terrainCanopyTexture->srv.get();
	ID3D11ShaderResourceView* ambient = distantAmbientLUT->srv.get();
	ID3D11ShaderResourceView* ao = std::get<0>(globals::features::screenSpaceGI.GetOutputTextures());
	const auto* screenShadowTexture = globals::features::screenSpaceShadows.screenSpaceShadowsTexture;
	ID3D11ShaderResourceView* screenShadow = screenShadowTexture ? screenShadowTexture->srv.get() : nullptr;
	ctx->CSSetShaderResources(2, 1, &mask);
	ctx->CSSetShaderResources(8, 1, &hiZ);
	ctx->CSSetShaderResources(14, 1, &shadow);
	ctx->CSSetShaderResources(45, 1, &screenShadow);
	ctx->CSSetShaderResources(64, 1, &types);
	ctx->CSSetShaderResources(73, 1, &ambient);
	ctx->CSSetShaderResources(74, 1, &depth);
	ctx->CSSetShaderResources(76, 1, &ao);

	// The world-shadow features maintain these resources on the pixel stage.
	ID3D11ShaderResourceView* cloudShadow = nullptr;
	ID3D11ShaderResourceView* terrainShadow = nullptr;
	ID3D11ShaderResourceView* previousCloudShadow = nullptr;
	ID3D11ShaderResourceView* previousTerrainShadow = nullptr;
	ctx->CSGetShaderResources(25, 1, &previousCloudShadow);
	ctx->CSGetShaderResources(60, 1, &previousTerrainShadow);
	ctx->PSGetShaderResources(25, 1, &cloudShadow);
	ctx->PSGetShaderResources(60, 1, &terrainShadow);
	ctx->CSSetShaderResources(25, 1, &cloudShadow);
	ctx->CSSetShaderResources(60, 1, &terrainShadow);
	Util::ReleaseAndNull(cloudShadow);
	Util::ReleaseAndNull(terrainShadow);

	ID3D11Buffer* commonCBs[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	ID3D11Buffer* grassCBs[2]{ grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ID3D11Buffer* frameCB = *globals::game::perFrame;
	ctx->CSSetConstantBuffers(5, 2, commonCBs);
	ctx->CSSetConstantBuffers(8, 2, grassCBs);
	ctx->CSSetConstantBuffers(12, 1, &frameCB);
	ctx->CSSetSamplers(0, 1, &linearClampSampler);
	ctx->CSSetSamplers(14, 1, &shadowSampler);

	ID3D11UnorderedAccessView* output = targets[RE::RENDER_TARGETS::kMAIN].UAV;
	ctx->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	ctx->CSSetShader(terrainCanopyCS, nullptr, 0);
	const float2 size = float2(1.0f / grassGlobalsStaging->dynamicResolutionInverted.x, 1.0f / grassGlobalsStaging->dynamicResolutionInverted.y);
	ctx->Dispatch(static_cast<uint32_t>(std::ceil(size.x / 8.0f)), static_cast<uint32_t>(std::ceil(size.y / 8.0f)), 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	for (const UINT slot : { 2u, 8u, 14u, 45u, 64u, 73u, 74u, 76u })
		ctx->CSSetShaderResources(slot, 1, &nullSRV);
	ctx->CSSetShaderResources(25, 1, &previousCloudShadow);
	ctx->CSSetShaderResources(60, 1, &previousTerrainShadow);
	Util::ReleaseAndNull(previousCloudShadow);
	Util::ReleaseAndNull(previousTerrainShadow);
	ctx->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();
}
