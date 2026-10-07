#include "ProceduralGrass.h"

#include "DynamicCubemaps.h"
#include "GrassCollision.h"
#include "HiZPyramid.h"
#include "IBL.h"
#include "LightLimitFix.h"
#include "LinearLighting.h"
#include "ProceduralGrass/TopDownOcclusion.h"
#include "ScreenSpaceGI.h"
#include "ScreenSpaceShadows.h"
#include "Skylighting.h"
#include "State.h"
#include "TerrainBlending.h"
#include "TerrainHeightMap.h"
#include "Utils/D3D.h"
#include "Utils/game.h"

#include <numbers>

using namespace PGrassCommon;

namespace
{
	// Preserve full density at the Low/Far handoff, then retain this fraction in distant Far regions.
	constexpr float FarPerformanceKeep = 0.55f;
}

void ProceduralGrass::PostPostLoad()
{
	// SE 12E3520, 100421 | AE 14CCB30, 107139
	REL::safe_fill(REL::RelocationID(100421, 107139).address() + REL::Relocate(0x523, 0xA3F), REL::NOP, 7);

	// SE 12E3AC0, 100422
	stl::write_thunk_call<Main_RenderShadowmasks_UpdateCamera>(REL::RelocationID(100422, 107140).address() + REL::Relocate(0x7B, 0x69));

	logger::info("[Procedural Grass] Installed hooks");
}

void ProceduralGrass::DataLoaded()
{
	if (!vanillaToggled) {
		vanillaToggled = true;
		ConsoleFunc_ToggleGrass();
	}
}

void ProceduralGrass::GameLoaded()
{
	globals::topDownOcclusion->Invalidate();
}

void ProceduralGrass::ClearShaderCache()
{
	globals::topDownOcclusion->ClearShaderCache();
	grassRendererHighLOD->ClearShaderCache();
	grassRendererMidLOD->ClearShaderCache();
	grassRendererLowLOD->ClearShaderCache();
	grassRendererFarLOD->ClearShaderCache();

	Util::ReleaseAndNull(densityAOVS);
	Util::ReleaseAndNull(densityAOPS);
	Util::ReleaseAndNull(depthClipPS);
	Util::ReleaseAndNull(densityGatherCS);
	Util::ReleaseAndNull(distantAmbientLUTCS);
	Util::ReleaseAndNull(terrainLiftCS);
	Util::ReleaseAndNull(terrainCanopyCS);
	terrainLiftOriginValid = false;

	CompileSupportShaders();
}

void ProceduralGrass::Main_RenderShadowmasks_UpdateCamera::thunk(RE::BSGraphics::State* state, RE::NiCamera* camera, bool flag)
{
	func(state, camera, flag);
	globals::features::proceduralGrass.PostDepthRendering();
}

bool ProceduralGrass::ConsoleFunc_ToggleGrass()
{
	using func_t = decltype(&ConsoleFunc_ToggleGrass);
	static REL::Relocation<func_t> func{ REL::RelocationID(22391, 22866) };
	return func();
}

void ProceduralGrass::PostDepthRendering()
{
	const auto ctx = globals::d3d::context;
	const auto renderer = globals::game::renderer;

	if (settings.Enabled && globals::game::grassManager && globals::game::grassManager->enableGrass)
		ConsoleFunc_ToggleGrass();

	const auto player = RE::PlayerCharacter::GetSingleton();

	if (!settings.Enabled || !player || globals::state->isMapMenuOpen) {
		CopyDepthBuffer(ctx, renderer);
		return;
	}

	GetVisibleQuadrants();

	ID3D11RasterizerState* oldRS = nullptr;
	ID3D11DepthStencilState* oldDSS = nullptr;
	UINT oldRef = 0;

	ID3D11BlendState* oldBS = nullptr;
	float oldBlendFactor[4];
	UINT oldSampleMask = 0;

	ctx->RSGetState(&oldRS);
	ctx->OMGetDepthStencilState(&oldDSS, &oldRef);
	ctx->OMGetBlendState(&oldBS, oldBlendFactor, &oldSampleMask);

	globals::topDownOcclusion->Render();
	// Grass Optimizations reuses this shared pyramid later in the frame.
	auto* grassHiZ = globals::hiZPyramid;
	grassHiZ->Build(globals::d3d::device, ctx, true);

	PostDepthRenderPrep(ctx, renderer);
	GenerateBlades(ctx, true);
	RenderDepth(ctx);

	// High and Mid form a dense wall close to the camera; rebuild Hi-Z so Low and Far generation can reject blades behind it.
	if (grassHiZ->HasCurrentSceneDepth() && !grassHiZ->Build(globals::d3d::device, ctx, true)) {
		// An unbound pyramid reads as zero depth and would reject everything.
		grassGlobalsStaging->grassHiZParams = float4::Zero;
		grassGlobalsCB->Update(*grassGlobalsStaging);
	}

	// The terrain lift map measures rendered terrain from this copy; the bound depth target cannot be sampled.
	CopyDepthBuffer(ctx, renderer);
	UpdateTerrainLift(ctx, renderer);

	// Low writes depth in its deferred pass; its PS samples the shadow mask at the blade root instead.
	GenerateBlades(ctx, false);

	// Merge grass depth after terrain blending so grass does not appear transparent over terrain.
	auto& terrainBlending = globals::features::terrainBlending;
	if (terrainBlending.loaded && terrainBlending.settings.Enabled) {
		terrainBlending.MergeSceneDepthIntoBlend();
		ID3D11ShaderResourceView* sceneDepthSRV = Util::GetCurrentSceneDepthSRV(true);
		ctx->PSSetShaderResources(17, 1, &sceneDepthSRV);
	}

	ctx->RSSetState(oldRS);
	ctx->OMSetDepthStencilState(oldDSS, oldRef);
	ctx->OMSetBlendState(oldBS, oldBlendFactor, oldSampleMask);

	Util::ReleaseAndNull(oldRS);
	Util::ReleaseAndNull(oldDSS);
	Util::ReleaseAndNull(oldBS);
}

void ProceduralGrass::CopyDepthBuffer(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer)
{
	const auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	const auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	ID3D11Resource* zPrepassCopyResource;
	ID3D11Resource* mainDepthResource;
	zPrepassCopy.views[0]->GetResource(&zPrepassCopyResource);
	mainDepth.views[0]->GetResource(&mainDepthResource);

	ctx->CopyResource(zPrepassCopyResource, mainDepthResource);

	zPrepassCopyResource->Release();
	mainDepthResource->Release();
}

void ProceduralGrass::UpdateDepthBaseCutoff()
{
	if (depthBlendStrength == settings.grassTerrainBlendStrength && depthBlendHeight == settings.grassTerrainBlendHeight)
		return;

	depthBlendStrength = settings.grassTerrainBlendStrength;
	depthBlendHeight = settings.grassTerrainBlendHeight;
	depthBaseCutoff = -1.0f;
	if (1.0f - depthBlendStrength < 0.999f) {
		// Invert the depth shader's smoothstep once per blend-setting change.
		float lower = 0.0f;
		float upper = 1.0f;
		for (uint32_t iteration = 0; iteration < 24; ++iteration) {
			const float midpoint = (lower + upper) * 0.5f;
			const float opacityRamp = midpoint * midpoint * (3.0f - 2.0f * midpoint);
			const float opacity = 1.0f - (1.0f - opacityRamp) * depthBlendStrength;
			if (opacity < 0.999f)
				lower = midpoint;
			else
				upper = midpoint;
		}
		depthBaseCutoff = upper * std::max(depthBlendHeight, 0.01f);
	}
}

void ProceduralGrass::PostDepthRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer)
{
	// Update the grass collision here, to cover when vanilla grass is disabled
	auto& grassCollision = globals::features::grassCollision;
	if (grassCollision.loaded)
		grassCollision.Update();

	const auto linearLightingData = globals::features::linearLighting.GetCommonBufferData();
	const bool prelinearizeTypeColors = linearLightingData.enableLinearLighting != 0;
	const float typeColorGamma = prelinearizeTypeColors ? linearLightingData.colorGamma : 1.0f;
	if (resolvedTypeColorsLinear != prelinearizeTypeColors || resolvedTypeColorGamma != typeColorGamma) {
		resolvedTypeColorsLinear = prelinearizeTypeColors;
		resolvedTypeColorGamma = typeColorGamma;
		grassTypesDirty = true;
		distantAmbientLUTFrame = UINT32_MAX;
	}

	const float shaderTimer = globals::state->timer;
	float timerDelta = shaderTimer - previousShaderTimer;
	if (timerDelta < 0.0f || timerDelta > 0.1f) {
		timerDelta = 0.0f;
		previousWindDirection = windDirection;
		previousWindSpeed = settings.windSpeed;
	}

	// Resolve frame-uniform TRUE_PBR lighting conversions once.
	constexpr float vanillaPBRLightingScale = 0.65f;
	const float grassLightingScale = prelinearizeTypeColors ? std::pow(vanillaPBRLightingScale, typeColorGamma) : vanillaPBRLightingScale;
	float3 resolvedDirLightColor = float3::Zero;

	if (const auto shaderManager = globals::game::smState) {
		if (const auto shadowSceneNode = shaderManager->shadowSceneNode[0]) {
			const auto sunLight = shadowSceneNode->GetRuntimeData().sunLight;
			if (sunLight) {
				if (const auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(sunLight->light.get())) {
					const auto& lightData = dirLight->GetLightRuntimeData();
					float sunlightScale = 1.0f;
					if (const auto imageSpaceManager = globals::game::imageSpaceManager)
						sunlightScale = imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale;

					const float rawLightScale = lightData.fade * sunlightScale;
					const float3 rawDirLightColor = float3(lightData.diffuse.red, lightData.diffuse.green, lightData.diffuse.blue) * rawLightScale;

					if (!prelinearizeTypeColors) {
						resolvedDirLightColor = rawDirLightColor * std::numbers::pi_v<float>;
					} else if (linearLightingData.isDirLightLinear) {
						resolvedDirLightColor = rawDirLightColor;
					} else {
						const float dirLightMult = linearLightingData.dirLightMult;
						const float invDirLightMult = 1.0f / std::max(dirLightMult, 1.0e-5f);
						const auto convertLight = [&](const float channel) {
							return std::pow(std::abs(channel * invDirLightMult), linearLightingData.lightGamma);
						};
						resolvedDirLightColor = float3(convertLight(rawDirLightColor.x), convertLight(rawDirLightColor.y), convertLight(rawDirLightColor.z));
						resolvedDirLightColor *= std::numbers::pi_v<float> * linearLightingData.directionalLightMult * dirLightMult;
					}
				}
			}
		}
	}
	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC texDesc;
	mainTex.texture->GetDesc(&texDesc);

	const float2 renderSize = Util::ConvertToDynamic(float2((float)texDesc.Width, (float)texDesc.Height));
	SetViewport(ctx, renderSize);

	const auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Transpose();
	const auto& row0 = viewProjMat.m[0];
	const auto& row1 = viewProjMat.m[1];
	const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();

	// Keep idle camera motion inside a dead zone, then follow continuously at its edge.
	constexpr float lodOriginDeadZone = 8.0f;
	if (!grassLodOriginInitialized) {
		grassLodOrigin = float2(cameraPosAdjust.x, cameraPosAdjust.y);
		grassLodOriginInitialized = true;
	} else {
		const float dx = cameraPosAdjust.x - grassLodOrigin.x;
		const float dy = cameraPosAdjust.y - grassLodOrigin.y;
		const float distanceSq = dx * dx + dy * dy;
		if (distanceSq > lodOriginDeadZone * lodOriginDeadZone) {
			const float distance = std::sqrt(distanceSq);
			const float follow = (distance - lodOriginDeadZone) / distance;
			grassLodOrigin.x += dx * follow;
			grassLodOrigin.y += dy * follow;
		}
	}

	auto& grassGlobals = *grassGlobalsStaging;
	grassGlobals = GrassGlobals{};
	grassGlobals.cameraViewRow0Sum = abs(row0[0]) + abs(row0[1]) + abs(row0[2]);
	grassGlobals.cameraViewRow1Sum = abs(row1[0]) + abs(row1[1]) + abs(row1[2]);
	const auto clipPlaneExtent = [&](int axis, float sign) {
		return abs(viewProjMat.m[0][3] + sign * viewProjMat.m[0][axis]) +
		       abs(viewProjMat.m[1][3] + sign * viewProjMat.m[1][axis]) +
		       abs(viewProjMat.m[2][3] + sign * viewProjMat.m[2][axis]);
	};
	grassGlobals.frustumPlaneExtent = float4(clipPlaneExtent(0, 1.0f), clipPlaneExtent(0, -1.0f),
		clipPlaneExtent(1, 1.0f), clipPlaneExtent(1, -1.0f));

	// Convert viewport-space SV_Position to normalized coordinates before dynamic-resolution adjustment.
	grassGlobals.dynamicResolutionInverted = float2(1.0f / renderSize.x, 1.0f / renderSize.y);

	grassGlobals.windSpeed = settings.windSpeed;
	grassGlobals.previousWindSpeed = previousWindSpeed;
	grassGlobals.windDir = windDirection;
	grassGlobals.windAngle = atan2(windDirection.y, windDirection.x);
	if (grassGlobals.windAngle < 0.0f)
		grassGlobals.windAngle += 2.0f * std::numbers::pi_v<float>;
	grassGlobals.windRotationScale = settings.windSpeed * settings.windSpeed * settings.windSpeed * 0.5f;
	grassGlobals.previousWindDir = previousWindDirection;
	grassGlobals.grassPBRLightingScale = prelinearizeTypeColors ? 1.0f : vanillaPBRLightingScale;

	const auto topDown = globals::topDownOcclusion;
	topDown->SetPaddingWorld(settings.occlusionPadding);  // Pre-pad the map for one generator centre tap.
	grassGlobals.occlusionHalfExtent = topDown->GetHalfExtent();
	grassGlobals.occlusionInvExtent = 1.0f / (topDown->GetHalfExtent() * 2.0f);
	grassGlobals.occlusionMapDim = topDown->GetMapDim();
	const auto window = topDown->GetWindowCentre();

	// z is underside clearance. A large negative value disables object culling.
	grassGlobals.occlusionParams = float4(window.x, window.y, settings.debugIgnoreObjectOcclusion ? -1.0e9f : settings.occlusionClearance, settings.occlusionBias);

	grassGlobals.grassAOParams = float4((float)grassDensityDim, settings.grassAOStrength, 64.0f, settings.grassHeight);
	// Full canopy sky occlusion leaves a sideways-facing surface at the base of dense grass about a third of its sky.
	constexpr float CanopySkyExtinction = 4.0f;
	const float canopySkyExponent = std::clamp(settings.grassCanopySkyOcclusion, 0.0f, 1.0f) * CanopySkyExtinction;
	grassGlobals.grassLightParams = float4(settings.grassDensityAO, canopySkyExponent, 0.0f, settings.grassBaseAO);
	grassGlobals.grassFrameLight = float4(resolvedDirLightColor.x, resolvedDirLightColor.y, resolvedDirLightColor.z, grassLightingScale);

	const auto farGridCells = globals::game::tes ? globals::game::tes->gridCells : nullptr;
	const int32_t loadedGridLength = farGridCells ? farGridCells->length : 5;
	const int32_t loadedCellRadius = loadedGridLength / 2;
	const int32_t farExtraCells = std::clamp(settings.grassCellRadius, 0, std::max(0, PGrassCommon::FarCellRadiusCap - PGrassCommon::FarStreamGuardCells - loadedCellRadius));
	const float farStart = loadedGridLength * 2048.0f;  // Loaded-grid half extent
	const float farEnd = farStart + std::max(farExtraCells, 1) * 4096.0f;
	grassGlobals.farParams = float4(farStart, 1.0f / (farEnd - farStart), 2048.0f / static_cast<float>(FarPatchDensity()), FarPerformanceKeep);
	const float lowToFarDensity = static_cast<float>(settings.lowGrassDensity) / static_cast<float>(FarPatchDensity());
	grassGlobals.farHandoffDensityRatio = lowToFarDensity * lowToFarDensity;

	// Terrain LOD is only rendered outside the cells with attached LAND. Empty bounds treat everything as outside.
	float4 loadedLandBounds{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
	if (farGridCells) {
		for (uint32_t i = 0; i < farGridCells->length * farGridCells->length; ++i) {
			const auto* cell = farGridCells->cells[i];
			if (!cell || cell->IsInteriorCell() || !cell->IsAttached())
				continue;
			const auto* exterior = cell->GetRuntimeData().cellData.exterior;
			if (!exterior)
				continue;
			const float minX = exterior->cellX * 4096.0f;
			const float minY = exterior->cellY * 4096.0f;
			loadedLandBounds = float4(std::min(loadedLandBounds.x, minX), std::min(loadedLandBounds.y, minY),
				std::max(loadedLandBounds.z, minX + 4096.0f), std::max(loadedLandBounds.w, minY + 4096.0f));
		}
	}
	grassGlobals.loadedLandBounds = loadedLandBounds;

	// Centre the terrain lift window on the camera. A new heightmap means a new worldspace, whose cells share
	// coordinates with the old one, so restart from an origin that maps every texel to a different cell.
	const int32_t liftOriginX = static_cast<int32_t>(std::floor(cameraPosAdjust.x / PGrassCommon::TerrainLiftCellSize)) - PGrassCommon::TerrainLiftDim / 2;
	const int32_t liftOriginY = static_cast<int32_t>(std::floor(cameraPosAdjust.y / PGrassCommon::TerrainLiftCellSize)) - PGrassCommon::TerrainLiftDim / 2;
	const uint32_t heightMapGeneration = globals::terrainHeightMap->GetLoadGeneration();
	if (!terrainLiftOriginValid || terrainLiftHeightMapGeneration != heightMapGeneration) {
		terrainLiftOriginCell[0] = liftOriginX + PGrassCommon::TerrainLiftDim * 8;
		terrainLiftOriginCell[1] = liftOriginY + PGrassCommon::TerrainLiftDim * 8;
		terrainLiftHeightMapGeneration = heightMapGeneration;
		terrainLiftOriginValid = true;
	}
	grassGlobals.terrainLiftOrigin[0] = liftOriginX;
	grassGlobals.terrainLiftOrigin[1] = liftOriginY;
	grassGlobals.terrainLiftOrigin[2] = terrainLiftOriginCell[0];
	grassGlobals.terrainLiftOrigin[3] = terrainLiftOriginCell[1];
	grassGlobals.terrainLiftPhase = globals::state->frameCount & 3u;
	terrainLiftOriginCell[0] = liftOriginX;
	terrainLiftOriginCell[1] = liftOriginY;

	previousShaderTimer = shaderTimer;
	previousWindDirection = windDirection;
	previousWindSpeed = settings.windSpeed;
	grassGlobals.miscParams = float4(settings.grassMapEdgeNoise, 0.0f, settings.grassViewThicken, timerDelta);
	grassGlobals.grassTerrainBlend = float4(settings.grassTerrainBlendStrength, settings.grassTerrainBlendHeight, settings.grassTerrainBlendNormal, settings.grassTerrainBlendRough);
	UpdateDepthBaseCutoff();

	auto heightMap = globals::terrainHeightMap;
	heightMap->LoadForCurrentWorldspace();

	const auto heightMapScale = heightMap->GetScale();
	grassGlobals.heightMapScale = float2(heightMapScale.x, heightMapScale.y);
	grassGlobals.heightMapOffset = heightMap->GetOffset();
	grassGlobals.heightMapZRange = heightMap->GetPosRange();
	grassGlobals.debugFlags = float2(settings.debugDisableAllCulls ? 1.0f : 0.0f, settings.debugTierView ? 1.0f : 0.0f);

	// Presence-map origin, inverse sample spacing, and dimension.
	grassGlobals.grassPresenceParams = float4(grassPresenceOrigin.x, grassPresenceOrigin.y, (float)(QuadrantGrassPitch - 1) / 2048.0f, (float)grassPresenceDim);
	const auto* grassHiZ = globals::hiZPyramid;
	grassGlobals.grassHiZParams = grassHiZ->HasCurrentSceneDepth() ?
	                                  float4((float)grassHiZ->GetWidth(), (float)grassHiZ->GetHeight(), 0.0f, (float)grassHiZ->GetMipCount()) :
	                                  float4::Zero;
	grassGlobals.grassLodOrigin = grassLodOrigin;

	if (grassTypesDirty)
		ResolveGrassTypes(prelinearizeTypeColors, typeColorGamma);

	const float canopyStart = std::max(24576.0f, farStart + 4096.0f);
	const float canopyEnd = std::min(canopyStart + 8192.0f, farEnd);
	if (canopyEnd > canopyStart && !settings.debugDisableAllCulls)
		UpdateTerrainCanopy(ctx);
	for (uint32_t axis = 0; axis < 2; ++axis) {
		grassGlobals.terrainCanopyWindow[axis] = terrainCanopyOrigin[axis];
		grassGlobals.terrainCanopyWindow[axis + 2] = terrainCanopyOrigin[axis] + TerrainCanopyQuadrants;
	}
	const bool canopyReady = terrainCanopyCS && terrainCanopyTexture && terrainCanopyTypedLoadSupported &&
	                         mainTex.UAV && renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS_DOWNSCALED].SRV &&
	                         distantAmbientLUTCS && Util::GetCurrentSceneDepthSRV(false) && grassHiZ->HasCurrentSceneDepth() &&
	                         terrainCanopyWorldSpace && !settings.debugDisableAllCulls && canopyEnd > canopyStart;
	grassGlobals.terrainCanopyParams = float4(canopyStart, canopyReady ? 1.0f / (canopyEnd - canopyStart) : 0.0f,
		terrainCanopyMaxHeight, settings.farDensityFalloff);

	if (grassHiZ->IsValid()) {
		grassGlobals.grassHiZParams.z = std::max(nearHiZRadius, settings.grassHeight);
		// Bound height response, gust strength, and the waveform before tightening depth rejection.
		const float windReach = nearHiZRadius * std::abs(settings.windSpeed) * (1.2f * 1.35f * 1.35f * 0.8f * 0.5f);
		grassGlobals.grassHiZBounds = float4(std::max(farQuadrantFrustumPadding, settings.grassHeight), hiZClumpReach, windReach, 0.0f);
	}
	grassGlobals.grassHiZBounds.w = depthBaseCutoff;
	grassGlobalsCB->Update(grassGlobals);

	ID3D11Buffer* buffers[2] = { *globals::game::perFrame, nullptr };
	ctx->VSSetConstantBuffers(12, 2, buffers);
	ctx->CSSetConstantBuffers(12, 2, buffers);

	ID3D11Buffer* grassBuffers[2] = { grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ctx->CSSetConstantBuffers(8, 2, grassBuffers);
	const auto generatorTypesCB = grassGeneratorTypesCB->CB();
	ctx->CSSetConstantBuffers(10, 1, &generatorTypesCB);
	ctx->VSSetConstantBuffers(10, 1, &generatorTypesCB);
	ctx->VSSetConstantBuffers(8, 2, grassBuffers);

	const auto state = globals::state;
	auto sharedDataCB = state->sharedDataCB->CB();
	auto featureDataCB = state->featureDataCB->CB();
	ctx->VSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->CSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->VSSetConstantBuffers(6, 1, &featureDataCB);

	if (auto heightMapSRV = heightMap->GetSRV())
		ctx->CSSetShaderResources(0, 1, &heightMapSRV);

	ctx->CSSetSamplers(0, 1, &linearClampSampler);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ProceduralGrass::SetViewport(ID3D11DeviceContext* ctx, const float2 size)
{
	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = size.x;
	vp.Height = size.y;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;

	ctx->RSSetViewports(1, &vp);
}

void ProceduralGrass::GenerateBlades(ID3D11DeviceContext* ctx, const bool nearTiers) const
{
	const float quad = 2048.0f;
	const float invBand = 1.0f / quad;

	const float highToMid = (HighTierQuadrantRadius - 1) * quad;  // High and Mid transition here
	const float midToLow = (MidTierQuadrantRadius - 1) * quad;    // Mid and Low transition here
	const float invMidLowBand = 1.0f / PGrassCommon::MidLowHandoffBand;

	const float gridEdge = LowTierQuadrantRadius * quad;
	// Extend the shared fade inward so Low keeps its existing outer bound.
	const float lowToFar = gridEdge - 2.0f * quad;
	const float invFarBand = 1.0f / (gridEdge - lowToFar);
	const auto farGridCells = globals::game::tes ? globals::game::tes->gridCells : nullptr;
	const int32_t loadedGridLength = farGridCells ? farGridCells->length : 5;
	const int32_t loadedCellRadius = loadedGridLength / 2;
	const int32_t farExtraCells = std::clamp(settings.grassCellRadius, 0, std::max(0, PGrassCommon::FarCellRadiusCap - PGrassCommon::FarStreamGuardCells - loadedCellRadius));
	const float farStart = loadedGridLength * 2048.0f;
	const float radiusEdge = farStart + std::max(farExtraCells, 1) * 4096.0f;
	const float4 noFadeIn = float4(0.0f, 1.0e9f, 0.0f, highToMid + quad);

	if (nearTiers) {
		// Build the canopy-density field before High generation so each emitted blade can cache one density sample.
		if (grassPresenceUploadDirty) {
			ctx->UpdateSubresource(grassPresenceTexture->resource.get(), 0, nullptr, grassPresenceStaging.data(), grassPresenceDim, 0);
			grassPresenceUploadDirty = false;
		}
		ID3D11ShaderResourceView* presSRV = grassPresenceTexture->srv.get();
		ctx->CSSetShaderResources(0, 1, &presSRV);
		ID3D11UnorderedAccessView* densityUAV = grassDensityTexture->uav.get();
		ctx->CSSetUnorderedAccessViews(0, 1, &densityUAV, nullptr);
		ctx->CSSetShader(densityGatherCS, nullptr, 0);
		const uint32_t gatherGroups = (grassDensityDim + 7) / 8;
		ctx->Dispatch(gatherGroups, gatherGroups, 1);

		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		ID3D11ShaderResourceView* densitySRV = grassDensityTexture->srv.get();
		ctx->CSSetShaderResources(7, 1, &densitySRV);
	}

	// The density gather and the Hi-Z rebuild both use CS t0 and b0.
	ID3D11ShaderResourceView* heightMapSRV = globals::terrainHeightMap->GetSRV();
	ctx->CSSetShaderResources(0, 1, &heightMapSRV);

	if (nearTiers) {
		// Near bounds cover clumping and Low width. Far bounds cover wider billboard blades.
		globals::profiler->BeginPass("ProceduralGrass::High Generation");
		grassRendererHighLOD->GenerateBlades(ctx, quadrantsHighLOD, quadrantsHighVersion, 61, 60, grassLodOrigin, noFadeIn,
			float4(highToMid, invBand, 0.0f, 0.0f), nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
		globals::profiler->EndPass();
		globals::profiler->BeginPass("ProceduralGrass::Mid Generation");
		grassRendererMidLOD->GenerateBlades(ctx, quadrantsMidLOD, quadrantsMidVersion, 61, 60, grassLodOrigin, float4(highToMid, invBand, 0.0f, midToLow + PGrassCommon::MidLowHandoffBand),
			float4(midToLow, invMidLowBand, 0.0f, 0.0f), nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
		globals::profiler->EndPass();
		UnbindGeneratorResources(ctx);
		return;
	}

	// An unbound map reads zero, leaving roots on LAND where there is no heightmap to measure against.
	const bool terrainLiftReady = terrainLiftTexture && terrainLiftSurfaceTexture && globals::terrainHeightMap->IsReady();
	ID3D11ShaderResourceView* terrainLiftSRVs[2] = {
		terrainLiftReady ? terrainLiftTexture->srv.get() : nullptr,
		terrainLiftReady ? terrainLiftSurfaceTexture->srv.get() : nullptr
	};
	ctx->CSSetShaderResources(9, ARRAYSIZE(terrainLiftSRVs), terrainLiftSRVs);
	globals::profiler->BeginPass("ProceduralGrass::Low Generation");
	grassRendererLowLOD->GenerateBlades(ctx, quadrantsLowLOD, quadrantsLowVersion, 61, 60, grassLodOrigin, float4(midToLow, invMidLowBand, 0.0f, gridEdge),
		float4(lowToFar, invFarBand, 0.0f, 0.0f), nearQuadrantFrustumPadding, settings.debugDisableAllCulls, lowFadeInPositionPadding);
	globals::profiler->EndPass();
	globals::profiler->BeginPass("ProceduralGrass::Far Generation");
	ID3D11ShaderResourceView* canopySRV = terrainCanopyTexture ? terrainCanopyTexture->srv.get() : nullptr;
	ctx->CSSetShaderResources(64, 1, &canopySRV);
	grassRendererFarLOD->GenerateBlades(ctx, quadrantsFarLOD, quadrantsFarVersion, 61, 60, grassLodOrigin, float4(lowToFar, invFarBand, 0.0f, radiusEdge),
		float4(gridEdge, 1.0f / std::max(radiusEdge - gridEdge, 1.0f), settings.farDensityFalloff, 1.0f / PGrassCommon::FarUnloadFadeWidth),
		farQuadrantFrustumPadding, settings.debugDisableAllCulls, 0.0f,
		float4(farStart, 1.0f / std::max(radiusEdge - farStart, 1.0f), FarPerformanceKeep, PGrassCommon::FarViewFacingKeep));
	globals::profiler->EndPass();

	UnbindGeneratorResources(ctx);
}

void ProceduralGrass::UnbindGeneratorResources(ID3D11DeviceContext* ctx)
{
	ID3D11UnorderedAccessView* uavs[3] = { nullptr, nullptr, nullptr };
	ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

	// Includes Hi-Z at t8 and terrain surface height and weight at t9-t10, before their UAVs are used again.
	ID3D11ShaderResourceView* nullGeneratorSRVs[11]{};
	ctx->CSSetShaderResources(0, ARRAYSIZE(nullGeneratorSRVs), nullGeneratorSRVs);
	ID3D11ShaderResourceView* nullSkylightingSRV = nullptr;
	ctx->CSSetShaderResources(50, 1, &nullSkylightingSRV);
	ctx->CSSetShaderResources(64, 1, &nullSkylightingSRV);
	ctx->CSSetShader(nullptr, nullptr, 0);
}

void ProceduralGrass::RenderDepth(ID3D11DeviceContext* ctx) const
{
	const auto& mainDepth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ctx->OMSetRenderTargets(0, nullptr, mainDepth.views[0]);

	ctx->RSSetState(noCullRS);
	ctx->OMSetDepthStencilState(depthWriteDS, 0);
	ctx->OMSetBlendState(depthOnlyBlend, nullptr, 0xFFFFFFFF);

	ID3D11Buffer* grassCB = grassGlobalsCB->CB();
	const auto generatorTypesCB = grassGeneratorTypesCB->CB();
	ctx->VSSetConstantBuffers(10, 1, &generatorTypesCB);
	ctx->VSSetConstantBuffers(8, 1, &grassCB);
	ctx->PSSetConstantBuffers(8, 1, &grassCB);

	// Screen-space lighting needs near blade depth before the shadow and occlusion passes.
	globals::profiler->BeginPass("ProceduralGrass::High Depth");
	grassRendererHighLOD->RenderDepth(ctx, depthClipPS);
	globals::profiler->EndPass();
	globals::profiler->BeginPass("ProceduralGrass::Mid Depth");
	grassRendererMidLOD->RenderDepth(ctx);
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullBladeSRV = nullptr;
	ctx->VSSetShaderResources(0, 1, &nullBladeSRV);
}

void ProceduralGrass::DeferredRendering() const
{
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!player || globals::state->isMapMenuOpen)
		return;

	const auto ctx = globals::d3d::context;
	const auto renderer = globals::game::renderer;

	ID3D11RasterizerState* oldRS = nullptr;
	ID3D11DepthStencilState* oldDSS = nullptr;
	UINT oldRef = 0;

	ID3D11BlendState* oldBS = nullptr;
	float oldBlendFactor[4];
	UINT oldSampleMask = 0;

	ctx->RSGetState(&oldRS);
	ctx->OMGetDepthStencilState(&oldDSS, &oldRef);
	ctx->OMGetBlendState(&oldBS, oldBlendFactor, &oldSampleMask);

	DeferredRenderPrep(ctx, renderer);

	RenderGrass(ctx);

	ctx->RSSetState(oldRS);
	ctx->OMSetDepthStencilState(oldDSS, oldRef);
	ctx->OMSetBlendState(oldBS, oldBlendFactor, oldSampleMask);

	Util::ReleaseAndNull(oldRS);
	Util::ReleaseAndNull(oldDSS);
	Util::ReleaseAndNull(oldBS);

	ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void ProceduralGrass::DeferredRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer) const
{
	const auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	const auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	ID3D11RenderTargetView* rtvs[8] = {
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_DOWNSCALED].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kINDIRECT].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kINDIRECT_DOWNSCALED].RTV,
		globals::features::dynamicCubemaps.loaded ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT].RTV : nullptr,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS_DOWNSCALED].RTV,
	};

	D3D11_TEXTURE2D_DESC texDesc;
	mainTex.texture->GetDesc(&texDesc);

	SetViewport(ctx, Util::ConvertToDynamic(float2((float)texDesc.Width, (float)texDesc.Height)));

	ctx->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, mainDepth.views[0]);

	auto& shadowMask = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];
	ctx->PSSetShaderResources(14, 1, &shadowMask.SRV);
	ctx->PSSetSamplers(14, 1, &shadowSampler);

	static auto& precipOcclusionTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
	ctx->PSSetShaderResources(70, 1, &precipOcclusionTexture.depthSRV);

	ctx->PSSetSamplers(0, 1, &linearClampSampler);

	const auto state = globals::state;
	auto sharedDataCB = state->sharedDataCB->CB();
	auto featureDataCB = state->featureDataCB->CB();
	ctx->PSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->VSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->PSSetConstantBuffers(6, 1, &featureDataCB);

	UpdateDistantAmbientLUT(ctx);

	ID3D11Buffer* buffers[1] = { *globals::game::perFrame };
	ctx->PSSetConstantBuffers(12, 1, buffers);
	ctx->VSSetConstantBuffers(12, 1, buffers);

	if (globals::features::lightLimitFix.loaded) {
		auto strictLightDataCB = globals::features::lightLimitFix.strictLightDataCB->CB();
		ctx->PSSetConstantBuffers(3, 1, &strictLightDataCB);
	}

	if (globals::features::skylighting.loaded && globals::features::skylighting.texProbeArray) {
		ID3D11ShaderResourceView* srv = { globals::features::skylighting.texProbeArray->srv.get() };
		ctx->PSSetShaderResources(50, 1, &srv);
	}

	// Mid colour uses the same geometry table as its depth pass.
	const auto generatorTypesCB = grassGeneratorTypesCB->CB();
	ctx->VSSetConstantBuffers(10, 1, &generatorTypesCB);

	const auto grassTypesCBa = grassTypesArrayCB->CB();
	ctx->VSSetConstantBuffers(9, 1, &grassTypesCBa);

	ctx->OMSetDepthStencilState(depthEqualDS, 0);
	ctx->RSSetState(noCullRS);
	ctx->OMSetBlendState(defaultBlend, nullptr, 0xFFFFFFFF);

	ID3D11Buffer* grassBuffers[2] = { grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ctx->VSSetConstantBuffers(8, 2, grassBuffers);
	ctx->PSSetConstantBuffers(8, 2, grassBuffers);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void ProceduralGrass::DarkenTerrainUnderGrass() const
{
	if (!settings.Enabled || globals::state->isMapMenuOpen || settings.grassAOStrength <= 0.0f || !densityAOVS || !densityAOPS)
		return;

	const auto ctx = globals::d3d::context;
	const auto renderer = globals::game::renderer;

	globals::profiler->BeginPass("ProceduralGrass::Terrain Shadow");

	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc;
	mainTex.texture->GetDesc(&texDesc);
	const float2 viewportSize = Util::ConvertToDynamic(float2((float)texDesc.Width, (float)texDesc.Height));
	SetViewport(ctx, viewportSize);

	const LONG viewportWidth = std::max(1l, static_cast<LONG>(std::ceil(viewportSize.x)));
	const LONG viewportHeight = std::max(1l, static_cast<LONG>(std::ceil(viewportSize.y)));
	D3D11_RECT shadowRect{ 0, 0, viewportWidth, viewportHeight };
	bool useShadowScissor = false;

	const auto* heightMap = globals::terrainHeightMap;
	if (noCullScissorRS && heightMap->IsReady()) {
		const auto zRange = heightMap->GetZRange();
		const auto farGridCells = globals::game::tes ? globals::game::tes->gridCells : nullptr;
		const int32_t loadedGridLength = farGridCells ? farGridCells->length : 5;
		const int32_t loadedCellRadius = loadedGridLength / 2;
		const int32_t farExtraCells = std::clamp(settings.grassCellRadius, 0, std::max(0, PGrassCommon::FarCellRadiusCap - PGrassCommon::FarStreamGuardCells - loadedCellRadius));
		const float farStart = loadedGridLength * 2048.0f;
		const float farEnd = farStart + std::max(farExtraCells, 1) * 4096.0f + PGrassCommon::FarUnloadFadeWidth;
		const auto centre = globals::topDownOcclusion->GetWindowCentre();
		const float minZ = std::min(zRange.x, zRange.y) - 256.0f;
		const float maxZ = std::max(zRange.x, zRange.y) + std::max(settings.grassHeight, 256.0f);
		const auto viewProj = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Transpose();
		const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();

		float minNdcX = FLT_MAX;
		float minNdcY = FLT_MAX;
		float maxNdcX = -FLT_MAX;
		float maxNdcY = -FLT_MAX;
		bool projectable = true;
		for (uint32_t corner = 0; corner < 8; ++corner) {
			const float x = centre.x + ((corner & 1u) ? farEnd : -farEnd);
			const float y = centre.y + ((corner & 2u) ? farEnd : -farEnd);
			const float z = (corner & 4u) ? maxZ : minZ;
			const float4 clip = float4::Transform(float4{ x - cameraPosAdjust.x, y - cameraPosAdjust.y, z - cameraPosAdjust.z, 1.0f }, viewProj);
			if (clip.w <= 1.0e-3f) {
				projectable = false;
				break;
			}

			const float invW = 1.0f / clip.w;
			const float ndcX = clip.x * invW;
			const float ndcY = clip.y * invW;
			minNdcX = std::min(minNdcX, ndcX);
			minNdcY = std::min(minNdcY, ndcY);
			maxNdcX = std::max(maxNdcX, ndcX);
			maxNdcY = std::max(maxNdcY, ndcY);
		}

		if (projectable) {
			constexpr float scissorPadding = 4.0f;
			const float left = (minNdcX * 0.5f + 0.5f) * viewportSize.x - scissorPadding;
			const float right = (maxNdcX * 0.5f + 0.5f) * viewportSize.x + scissorPadding;
			const float top = (0.5f - maxNdcY * 0.5f) * viewportSize.y - scissorPadding;
			const float bottom = (0.5f - minNdcY * 0.5f) * viewportSize.y + scissorPadding;

			shadowRect.left = std::clamp(static_cast<LONG>(std::floor(left)), 0l, viewportWidth);
			shadowRect.right = std::clamp(static_cast<LONG>(std::ceil(right)), 0l, viewportWidth);
			shadowRect.top = std::clamp(static_cast<LONG>(std::floor(top)), 0l, viewportHeight);
			shadowRect.bottom = std::clamp(static_cast<LONG>(std::ceil(bottom)), 0l, viewportHeight);
			useShadowScissor = shadowRect.left < shadowRect.right && shadowRect.top < shadowRect.bottom;
		}
	}

	ctx->OMSetRenderTargets(0, nullptr, nullptr);
	if (terrainCanopyTypedLoadSupported) {
		ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &mainTex.UAV, nullptr);
	} else {
		// Older devices read a copy because they cannot load the scene's typed UAV format.
		if (!terrainDarkeningSceneCopy || terrainDarkeningSceneCopy->desc.Width != texDesc.Width ||
			terrainDarkeningSceneCopy->desc.Height != texDesc.Height || terrainDarkeningSceneCopy->desc.Format != texDesc.Format) {
			auto copyDesc = texDesc;
			copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.CPUAccessFlags = 0;
			copyDesc.MiscFlags = 0;
			terrainDarkeningSceneCopy = std::make_unique<Texture2D>(copyDesc, "PGrass::TerrainDarkeningSceneCopy");
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = copyDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = copyDesc.MipLevels;
			terrainDarkeningSceneCopy->CreateSRV(srvDesc);
		}
		ctx->CopyResource(terrainDarkeningSceneCopy->resource.get(), mainTex.texture);
		ID3D11ShaderResourceView* sceneSRV = terrainDarkeningSceneCopy->srv.get();
		ctx->PSSetShaderResources(6, 1, &sceneSRV);
		ctx->OMSetRenderTargets(1, &mainTex.RTV, nullptr);
	}
	ctx->OMSetBlendState(defaultBlend, nullptr, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState(noDepthDSS, 0);
	ctx->RSSetState(useShadowScissor ? noCullScissorRS : noCullRS);
	if (useShadowScissor)
		ctx->RSSetScissorRects(1, &shadowRect);

	auto terrainHeightSRV = globals::terrainHeightMap->GetSRV();
	auto& terrainBlending = globals::features::terrainBlending;
	auto renderedDepthSRV = terrainBlending.loaded && terrainBlending.settings.Enabled && terrainBlending.depthSRVBackup ?
	                            terrainBlending.depthSRVBackup :
	                            mainDepth.depthSRV;
	// The object masks keep the generator's registers so the darkening reuses its object test.
	auto* topDown = globals::topDownOcclusion;
	ID3D11ShaderResourceView* srvs[6] = { mainDepth.depthSRV, grassPresenceTexture ? grassPresenceTexture->srv.get() : nullptr, topDown->GetHighSRV(), renderedDepthSRV,
		topDown->GetLowSRV(), terrainHeightSRV };
	ctx->PSSetShaderResources(0, 6, srvs);
	ID3D11ShaderResourceView* canopySRV = terrainCanopyTexture ? terrainCanopyTexture->srv.get() : nullptr;
	ctx->PSSetShaderResources(64, 1, &canopySRV);
	ctx->PSSetSamplers(0, 1, &linearClampSampler);

	ID3D11Buffer* lightingCBs[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	ctx->PSSetConstantBuffers(5, 2, lightingCBs);
	ID3D11Buffer* grassCB = grassGlobalsCB->CB();
	ctx->PSSetConstantBuffers(8, 1, &grassCB);
	ID3D11Buffer* perFrame = *globals::game::perFrame;
	ctx->PSSetConstantBuffers(12, 1, &perFrame);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(densityAOVS, nullptr, 0);
	ctx->PSSetShader(densityAOPS, nullptr, 0);
	ctx->Draw(3, 0);
	if (useShadowScissor)
		ctx->RSSetState(noCullRS);

	ID3D11RenderTargetView* nullRTV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &nullUAV, nullptr);
	ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
	ID3D11ShaderResourceView* nullSRVs[7] = {};
	ctx->PSSetShaderResources(0, 7, nullSRVs);
	ctx->PSSetShaderResources(64, 1, nullSRVs);

	globals::profiler->EndPass();
}

void ProceduralGrass::UpdateDistantAmbientLUT(ID3D11DeviceContext* ctx) const
{
	if (!distantAmbientLUT || !distantAmbientLUTCS)
		return;
	if (distantAmbientLUTFrame == globals::state->frameCount) {
		ID3D11ShaderResourceView* ambientSRV = distantAmbientLUT->srv.get();
		ctx->PSSetShaderResources(73, 1, &ambientSRV);
		return;
	}

	// D3D11 cannot bind one resource for PS reads and CS writes simultaneously.
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->PSSetShaderResources(73, 1, &nullSRV);

	auto* state = globals::state;
	ID3D11Buffer* ambientCBs[2] = { state->sharedDataCB->CB(), state->featureDataCB->CB() };
	ctx->CSSetConstantBuffers(5, 2, ambientCBs);
	ID3D11Buffer* grassCB = grassGlobalsCB->CB();
	ctx->CSSetConstantBuffers(8, 1, &grassCB);

	auto& ibl = globals::features::ibl;
	ID3D11ShaderResourceView* iblSRVs[2] = {
		ibl.loaded && ibl.envIBLTexture ? ibl.envIBLTexture->srv.get() : nullptr,
		ibl.loaded && ibl.skyIBLTexture ? ibl.skyIBLTexture->srv.get() : nullptr,
	};
	ctx->CSSetShaderResources(76, 2, iblSRVs);

	ID3D11UnorderedAccessView* ambientUAV = distantAmbientLUT->uav.get();
	ctx->CSSetUnorderedAccessViews(0, 1, &ambientUAV, nullptr);
	ctx->CSSetShader(distantAmbientLUTCS, nullptr, 0);
	ctx->Dispatch((distantAmbientLUTDim + 7) / 8, (distantAmbientLUTDim + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ID3D11ShaderResourceView* nullIBLSRVs[2] = { nullptr, nullptr };
	ctx->CSSetShaderResources(76, 2, nullIBLSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);

	ID3D11ShaderResourceView* ambientSRV = distantAmbientLUT->srv.get();
	ctx->PSSetShaderResources(73, 1, &ambientSRV);
	distantAmbientLUTFrame = globals::state->frameCount;
}

void ProceduralGrass::UpdateTerrainLift(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer) const
{
	if (!terrainLiftTexture || !terrainLiftMeasuredTexture || !terrainLiftSurfaceTexture || !terrainLiftCS || !globals::terrainHeightMap->IsReady())
		return;

	globals::profiler->BeginPass("ProceduralGrass::Terrain Lift");

	ID3D11Buffer* perFrame = *globals::game::perFrame;
	ctx->CSSetConstantBuffers(12, 1, &perFrame);
	ID3D11Buffer* grassCB = grassGlobalsCB->CB();
	ctx->CSSetConstantBuffers(8, 1, &grassCB);
	ctx->CSSetSamplers(0, 1, &linearClampSampler);

	ID3D11ShaderResourceView* srvs[2] = {
		globals::terrainHeightMap->GetSRV(),
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV
	};
	ctx->CSSetShaderResources(0, 2, srvs);
	ID3D11UnorderedAccessView* liftUAVs[3] = { terrainLiftMeasuredTexture->uav.get(), terrainLiftTexture->uav.get(), terrainLiftSurfaceTexture->uav.get() };
	ctx->CSSetUnorderedAccessViews(0, ARRAYSIZE(liftUAVs), liftUAVs, nullptr);
	ctx->CSSetShader(terrainLiftCS, nullptr, 0);
	ctx->Dispatch((PGrassCommon::TerrainLiftDim + 7) / 8, (PGrassCommon::TerrainLiftDim + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[3]{};
	ctx->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
	ctx->CSSetShaderResources(0, 2, nullSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);

	globals::profiler->EndPass();
}

void ProceduralGrass::RenderGrass(ID3D11DeviceContext* ctx) const
{
	globals::profiler->BeginPass("ProceduralGrass::Deferred");

	ID3D11ShaderResourceView* densitySRV = grassDensityTexture->srv.get();
	ctx->PSSetShaderResources(71, 1, &densitySRV);
	ID3D11ShaderResourceView* detailSRV = grassMaterialDetailTexture->srv.get();
	ctx->PSSetShaderResources(75, 1, &detailSRV);
	ctx->PSSetSamplers(13, 1, &grassDetailSampler);
	ID3D11ShaderResourceView* sceneDepthSRV = Util::GetCurrentSceneDepthSRV(false);
	ctx->PSSetShaderResources(74, 1, &sceneDepthSRV);

	// Only High uses terrain-contact blending; other append buffers render opaque.
	ctx->OMSetBlendState(terrainFadeBlend, nullptr, 0xFFFFFFFF);
	grassRendererHighLOD->RenderGrass(ctx);
	ctx->OMSetBlendState(defaultBlend, nullptr, 0xFFFFFFFF);

	grassRendererMidLOD->RenderGrass(ctx);
	// Low has no depth prepass: its PS is cheap, so one pass with depth writes replaces a second full vertex pass.
	ctx->OMSetDepthStencilState(depthWriteDS, 0);
	grassRendererLowLOD->RenderGrass(ctx);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->VSSetShaderResources(0, 1, &nullSRV);
	ID3D11ShaderResourceView* nullGrassSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
	ctx->PSSetShaderResources(71, 5, nullGrassSRVs);

	globals::profiler->EndPass();
}

void ProceduralGrass::ForwardRenderFar() const
{
	if (!settings.Enabled || globals::state->isMapMenuOpen || !RE::PlayerCharacter::GetSingleton())
		return;

	auto* ctx = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	ID3D11RasterizerState* oldRS = nullptr;
	ID3D11DepthStencilState* oldDSS = nullptr;
	ID3D11BlendState* oldBS = nullptr;
	UINT oldRef = 0;
	float oldBlendFactor[4]{};
	UINT oldSampleMask = 0;
	ctx->RSGetState(&oldRS);
	ctx->OMGetDepthStencilState(&oldDSS, &oldRef);
	ctx->OMGetBlendState(&oldBS, oldBlendFactor, &oldSampleMask);
	RenderTerrainCanopy(ctx, renderer);
	globals::profiler->BeginPass("ProceduralGrass::Far Forward");
	DeferredRenderPrep(ctx, renderer);

	auto& renderTargets = renderer->GetRuntimeData().renderTargets;
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	// Far writes its own motion vectors so TAA reprojects its blades rather than the surface behind them.
	ID3D11RenderTargetView* farTargets[2] = { renderTargets[RE::RENDER_TARGETS::kMAIN].RTV, renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV };
	ctx->OMSetRenderTargets(2, farTargets, mainDepth.views[0]);
	ctx->OMSetBlendState(defaultBlend, nullptr, 0xFFFFFFFF);

	// Opaque depth makes Far output independent of append order.
	ctx->OMSetDepthStencilState(depthWriteDS, 0);
	ID3D11ShaderResourceView* screenAO = std::get<0>(globals::features::screenSpaceGI.GetOutputTextures());
	ctx->PSSetShaderResources(76, 1, &screenAO);
	ID3D11ShaderResourceView* sceneDepthSRV = Util::GetCurrentSceneDepthSRV(false);
	ctx->PSSetShaderResources(74, 1, &sceneDepthSRV);

	// Far reads the shadows cast onto its roots; rebind them in case a later pass replaced the slot.
	if (const auto* screenSpaceShadowsTexture = globals::features::screenSpaceShadows.screenSpaceShadowsTexture) {
		ID3D11ShaderResourceView* screenSpaceShadowsSRV = screenSpaceShadowsTexture->srv.get();
		ctx->PSSetShaderResources(45, 1, &screenSpaceShadowsSRV);
	}

	grassRendererFarLOD->RenderGrass(ctx);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->VSSetShaderResources(0, 1, &nullSRV);
	ID3D11ShaderResourceView* nullGrassSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
	ctx->PSSetShaderResources(71, 4, nullGrassSRVs);
	ctx->PSSetShaderResources(76, 1, &nullSRV);
	ctx->OMSetRenderTargets(0, nullptr, nullptr);

	// Transparent effects need the depth written by Low and Far, just as the hardware depth test does.
	ID3D11ShaderResourceView* previousEffectDepthSRV = nullptr;
	ID3D11ShaderResourceView* previousSceneDepthSRV = nullptr;
	ctx->PSGetShaderResources(3, 1, &previousEffectDepthSRV);
	ctx->PSGetShaderResources(17, 1, &previousSceneDepthSRV);
	ctx->PSSetShaderResources(3, 1, &nullSRV);
	ctx->PSSetShaderResources(17, 1, &nullSRV);

	auto& terrainBlending = globals::features::terrainBlending;
	if (terrainBlending.loaded && terrainBlending.settings.Enabled)
		terrainBlending.MergeSceneDepthIntoBlend();
	else
		CopyDepthBuffer(ctx, renderer);

	ctx->PSSetShaderResources(3, 1, &previousEffectDepthSRV);
	ctx->PSSetShaderResources(17, 1, &previousSceneDepthSRV);
	Util::ReleaseAndNull(previousEffectDepthSRV);
	Util::ReleaseAndNull(previousSceneDepthSRV);

	ctx->RSSetState(oldRS);
	ctx->OMSetDepthStencilState(oldDSS, oldRef);
	ctx->OMSetBlendState(oldBS, oldBlendFactor, oldSampleMask);

	Util::ReleaseAndNull(oldRS);
	Util::ReleaseAndNull(oldDSS);
	Util::ReleaseAndNull(oldBS);

	globals::profiler->EndPass();
}
