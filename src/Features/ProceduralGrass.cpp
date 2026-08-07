#include "ProceduralGrass.h"

#include "LightLimitFix.h"
#include "ShaderCache.h"
#include "Skylighting.h"
#include "State.h"

#include <DDSTextureLoader.h>
#include <numbers>
#include <chrono>

using namespace PGrassCommon;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralGrass::Settings,
	Enabled,
	Quality,
	ThreadGroupSize)

float grassHeight = 40.0f;
float grassWidth = 0.7f;
float stiffness = 0.24f;
float tipWeight = 0.54f;
float mid = 0.73f;

float3 color = float3(0.428f, 0.391f, 0.211f);

float rotationalStiffness = 1.0f;
float ao = 0.9f;
float specular = 0.04f;

float2 subsurfaceOpacity = float2(0.2f, 0.05f);

int voronoiGridSize = 256;
float clumpDistanceFactor = 0.1f;
float clumpFacingFactor = 0.25f;
float clumpHeightFactor = 0.3f;

float windAngle = 0.0f;
float windSpeed = 1.0f;
float2 windDirection = float2(1.0f, 0.0f);

float spatialFreq = 1.0f;
float phaseOffset = 0.5f;
float phaseLag = 0.5f;

float windTimer = 0.0f;

void ProceduralGrass::DrawSettings()
{
	ImGui::Checkbox("Enabled", &settings.Enabled);

	// logger::info("Seconds since last frame: {}", RE::GetSecondsSinceLastFrame());
	// logger::info("Chrono Seconds since last frame: {}", fpsTimer.tick());

	if (ImGui::Button("Toggle Vanilla Grass Rendering"))
		ConsoleFunc_ToggleGrass();

	ImGui::Separator();

	if (ImGui::Button("1. Press first")) {
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			player->SetPosition({ 40000.74f, 5069.26f, -4330.91f }, true);
			player->SetAngle({ 0, 0, DirectX::XMConvertToRadians(90.0f) });
		}
	}

	if (ImGui::Button("2. Move to benchmark location")) {
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			player->SetPosition({ 36087.74f, 5069.26f, -4330.91f }, true);
			player->SetAngle({ 0, 0, DirectX::XMConvertToRadians(90.0f) });
		}
	}

	ImGui::Separator();

	if (ImGui::SliderInt("Density", &settings.Quality, 0, static_cast<uint8_t>(Quality::Count) - 1, QualityNames[settings.Quality], ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		grassRendererHighLOD->SetDensity(QualityDensities[settings.Quality]);
		grassRendererMidLOD->SetDensity(QualityDensities[settings.Quality]);
		grassRendererLowLOD->SetDensity(QualityDensities[settings.Quality]);
	}

	if (ImGui::SliderInt("ThreadGroup Size", &settings.ThreadGroupSize, 2, 8, [&] {static std::string s; s = std::to_string(1 << settings.ThreadGroupSize); return s.c_str(); }(), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		uint32_t threadGroupSize = 1 << settings.ThreadGroupSize;
		grassRendererHighLOD->SetThreadGroupSize(threadGroupSize);
		grassRendererMidLOD->SetThreadGroupSize(threadGroupSize);
		grassRendererLowLOD->SetThreadGroupSize(threadGroupSize);
	}

	ImGui::Separator();

	if (ImGui::SliderAngle("Wind Direction", &windAngle)) {
		windDirection = float2(cos(windAngle), sin(windAngle));
	}

	ImGui::SliderFloat("Wind Speed", &windSpeed, 0.0f, 1.0f);

	ImGui::SliderFloat("Phase Offset", &phaseOffset, 0.0f, 10.0f);
	ImGui::SliderFloat("Phase Lag", &phaseLag, 0.0f, 1.0f);
	ImGui::SliderFloat("Spatial Freq", &spatialFreq, 0.0f, 100.0f);

	ImGui::Separator();

	ImGui::ColorEdit3("Color", reinterpret_cast<float*>(&color));

	ImGui::Separator();

	ImGui::SliderFloat("Height", &grassHeight, 0.0f, 100.0f, "%.1f");
	ImGui::SliderFloat("Width", &grassWidth, 0.0f, 10.0f, "%.1f");
	ImGui::SliderFloat("K1", &stiffness, -10.0f, 10.0f, "%.2f");
	ImGui::SliderFloat("K2", &tipWeight, -10.0f, 10.0f, "%.2f");
	ImGui::SliderFloat("Mid", &mid, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Rotational Stiffness", &rotationalStiffness, 0.0f, 10.0f, "%.2f");

	ImGui::Separator();

	ImGui::SliderFloat("Baked Min AO", &ao, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat2("Subsurface Opacity (Base>Tip)", reinterpret_cast<float*>(&subsurfaceOpacity), 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Specular", &specular, 0.0f, 1.0f, "%.2f");

	ImGui::Separator();

	ImGui::SliderInt("Clump Grid Size", &voronoiGridSize, 1, 4096);
	ImGui::SliderFloat("Clump Distance Factor", &clumpDistanceFactor, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Clump Facing Factor", &clumpFacingFactor, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Clump Height Factor", &clumpHeightFactor, 0.0f, 2.0f, "%.2f");
}

void ProceduralGrass::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ProceduralGrass::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ProceduralGrass::RestoreDefaultSettings()
{
	settings = {};
}

void ProceduralGrass::PostPostLoad()
{
	// SE 12E3520, 100421 | AE 14CCB30, 107139
	REL::safe_fill(REL::RelocationID(100421, 107139, 0).address() + REL::Relocate(0x523, 0xA3F, 0), REL::NOP, 7);

	// SE 12E3AC0, 100422
	stl::write_thunk_call<Main_RenderShadowmasks_UpdateCamera>(REL::RelocationID(100422, 107140, 0).address() + REL::Relocate(0x7B, 0x69, 0));

	logger::info("[Procedural Grass] Installed hooks");
}

void ProceduralGrass::DataLoaded()
{
	ConsoleFunc_ToggleGrass();
}

void ProceduralGrass::ClearShaderCache()
{
	grassRendererHighLOD->ClearShaderCache();
	grassRendererMidLOD->ClearShaderCache();
	grassRendererLowLOD->ClearShaderCache();
}

void ProceduralGrass::Main_RenderShadowmasks_UpdateCamera::thunk(RE::BSGraphics::State* state, RE::NiCamera* camera, bool flag)
{
	func(state, camera, flag);
	GetSingleton()->PostDepthRendering();
}

bool ProceduralGrass::ConsoleFunc_ToggleGrass()
{
	using func_t = decltype(&ConsoleFunc_ToggleGrass);
	static REL::Relocation<func_t> func{ REL::VariantOffset(0x0313600, 0x036ABF0, 0) };
	return func();
}

void ProceduralGrass::SetupResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	grassGlobalsCB = new ConstantBuffer(ConstantBufferDesc<GrassGlobals>());
	grassTypesArrayCB = new ConstantBuffer(ConstantBufferDesc<GrassTypesArray>());

	auto vertexIndicesHigh = CreateVertexIndicesArray(15);
	D3D11_BUFFER_DESC highIbd{};
	highIbd.Usage = D3D11_USAGE_IMMUTABLE;
	highIbd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	highIbd.ByteWidth = static_cast<UINT>(vertexIndicesHigh.size() * sizeof(uint16_t));
	highIbd.CPUAccessFlags = 0;
	D3D11_SUBRESOURCE_DATA highIbdInit{ vertexIndicesHigh.data(), 0, 0 };
	vertexIndicesHighBuffer = new Buffer(highIbd, &highIbdInit);

	auto vertexIndicesLow = CreateVertexIndicesArray(7);
	D3D11_BUFFER_DESC lowIbd{};
	lowIbd.Usage = D3D11_USAGE_IMMUTABLE;
	lowIbd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	lowIbd.ByteWidth = static_cast<UINT>(vertexIndicesLow.size() * sizeof(uint16_t));
	lowIbd.CPUAccessFlags = 0;
	D3D11_SUBRESOURCE_DATA lowIbdInit{ vertexIndicesLow.data(), 0, 0 };
	vertexIndicesLowBuffer = new Buffer(lowIbd, &lowIbdInit);

	uint32_t threadGroupSize = 1 << settings.ThreadGroupSize;
	grassRendererHighLOD = new PGrassRenderer<9, 4>(QualityDensities[settings.Quality], threadGroupSize, vertexIndicesHighBuffer, "HIGH_LOD", "HIGH_VERTEX");
	grassRendererMidLOD = new PGrassRenderer<16, 2>(QualityDensities[settings.Quality], threadGroupSize, vertexIndicesLowBuffer, "MID_LOD", "LOW_VERTEX");
	grassRendererLowLOD = new PGrassRenderer<75, 1>(QualityDensities[settings.Quality], threadGroupSize, vertexIndicesLowBuffer, "LOW_LOD", "LOW_VERTEX");

	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	device->CreateSamplerState(&samplerDesc, &linearClampSampler);

	D3D11_SAMPLER_DESC shadowSamplerDesc = {};
	shadowSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	shadowSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	shadowSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	shadowSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	shadowSamplerDesc.MipLODBias = 0;
	shadowSamplerDesc.MaxAnisotropy = 1;
	shadowSamplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	shadowSamplerDesc.MinLOD = -3.40282E+38;
	shadowSamplerDesc.MaxLOD = 0;
	device->CreateSamplerState(&shadowSamplerDesc, &shadowSampler);

	if (!noCullRS) {
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.FrontCounterClockwise = FALSE;
		rd.DepthClipEnable = TRUE;
		rd.DepthBiasClamp = -100.0f;
		device->CreateRasterizerState(&rd, &noCullRS);
	}

	if (!depthOnDSS) {
		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = TRUE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dd.DepthFunc = D3D11_COMPARISON_LESS;
		dd.StencilEnable = FALSE;
		device->CreateDepthStencilState(&dd, &depthOnDSS);
	}

	if (!depthWriteDS) {
		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = TRUE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dd.DepthFunc = D3D11_COMPARISON_LESS;
		dd.StencilEnable = FALSE;
		device->CreateDepthStencilState(&dd, &depthWriteDS);
	}

	if (!depthEqualDS) {
		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = TRUE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		dd.StencilEnable = FALSE;
		device->CreateDepthStencilState(&dd, &depthEqualDS);
	}

	if (!depthOnlyBlend) {
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = FALSE;
		bd.RenderTarget[0].RenderTargetWriteMask = 0;
		device->CreateBlendState(&bd, &depthOnlyBlend);
	}

	if (!defaultBlend) {
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = FALSE;
		bd.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_RED |
			D3D11_COLOR_WRITE_ENABLE_GREEN |
			D3D11_COLOR_WRITE_ENABLE_BLUE |
			D3D11_COLOR_WRITE_ENABLE_ALPHA;
		device->CreateBlendState(&bd, &defaultBlend);
	}

	{
		logger::info("[Procedural Grass] Loading Tamriel_h.dds...");
		auto hr = DirectX::CreateDDSTextureFromFile(device, context, L"Data\\textures\\procgrass\\Tamriel\\Tamriel_h.dds", nullptr, heightMap.put());
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to load Tamriel_h.dds");
	}

	{
		logger::info("[Procedural Grass] Loading Tamriel_g.dds...");
		auto hr = DirectX::CreateDDSTextureFromFile(device, context, L"Data\\textures\\procgrass\\Tamriel\\Tamriel_g.dds", nullptr, grassMap.put());
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to load Tamriel_g.dds");
	}

	{
		D3D11_TEXTURE1D_DESC desc = {};
		desc.Width = 16;
		desc.MipLevels = 1;
		desc.ArraySize = 1;  // todo calc based on num tex
		desc.Format = DXGI_FORMAT_R8G8_UNORM;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		auto hr = device->CreateTexture1D(&desc, nullptr, &grassDiffuseRoughnessArray);
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to create 1D diffuse roughness array");

		// todo loop over all textures and load them
		logger::info("[Procedural Grass] Loading grass_dr.dds...");
		ID3D11Resource* resource = nullptr;
		hr = DirectX::CreateDDSTextureFromFile(device, context, L"Data\\textures\\procgrass\\grass_dr.dds", &resource, nullptr);
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to load grass_dr.dds");

		// todo format checking make sure it matches
		ID3D11Texture2D* srcTex = nullptr;
		hr = resource->QueryInterface(IID_PPV_ARGS(&srcTex));
		resource->Release();
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to query resource");

		UINT dstSub = D3D11CalcSubresource(0, 0, 1);
		UINT srcSub = D3D11CalcSubresource(0, 0, 1);
		D3D11_BOX box = { 0, 0, 0, desc.Width, 1, 1 };
		context->CopySubresourceRegion(grassDiffuseRoughnessArray, dstSub, 0, 0, 0, srcTex, srcSub, &box);
		srcTex->Release();

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
		srvDesc.Texture1DArray.MostDetailedMip = 0;
		srvDesc.Texture1DArray.MipLevels = 1;
		srvDesc.Texture1DArray.FirstArraySlice = 0;
		srvDesc.Texture1DArray.ArraySize = 1;  // todo calc based on num tex
		hr = device->CreateShaderResourceView(grassDiffuseRoughnessArray, &srvDesc, &grassDiffuseRoughnessArraySRV);
		if (FAILED(hr)) {
			logger::info("[Procedural Grass] Failed to create diffuse roughness SRV");
		}
	}

	{
		D3D11_TEXTURE1D_DESC desc = {};
		desc.Width = 16;
		desc.MipLevels = 1;
		desc.ArraySize = 16;  // todo calc based on num tex
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		auto hr = device->CreateTexture1D(&desc, nullptr, &grassColorLUTsArray);
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to create 1D  array");

		// todo loop over all textures and load them
		logger::info("[Procedural Grass] Loading grassLUT.dds...");
		ID3D11Resource* resource = nullptr;
		hr = DirectX::CreateDDSTextureFromFile(device, context, L"Data\\textures\\procgrass\\grassLUT.dds", &resource, nullptr);
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to load grassLUT.dds");

		ID3D11Texture2D* srcTex = nullptr;
		hr = resource->QueryInterface(IID_PPV_ARGS(&srcTex));
		resource->Release();
		if (!SUCCEEDED(hr))
			logger::info("[Procedural Grass] Failed to query resource");

		UINT srcSub = D3D11CalcSubresource(0, 0, 1);

		for (uint32_t i = 0; i < 16; i++) {
			UINT dstSub = D3D11CalcSubresource(0, i, 1);
			D3D11_BOX box = { 0, i, 0, 16, i + 1, 1 };
			context->CopySubresourceRegion(grassColorLUTsArray, dstSub, 0, 0, 0, srcTex, srcSub, &box);
		}

		srcTex->Release();

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
		srvDesc.Texture1DArray.MostDetailedMip = 0;
		srvDesc.Texture1DArray.MipLevels = 1;
		srvDesc.Texture1DArray.FirstArraySlice = 0;
		srvDesc.Texture1DArray.ArraySize = 16;  // todo calc based on num tex
		hr = device->CreateShaderResourceView(grassColorLUTsArray, &srvDesc, &grassColorLUTsArraySRV);
		if (FAILED(hr)) {
			logger::info("[Procedural Grass] Failed to create LUT SRV");
		}
	}
}

std::vector<uint16_t> ProceduralGrass::CreateVertexIndicesArray(uint16_t vertCount)
{
	assert(vertCount >= 3 && ((vertCount - 3) % 2) == 0);
	const uint16_t segments = (vertCount - 3) / 2;

	// these vertex indices live along the fold for double blades
	// this code ensures that vertices along the fold are not used as provoking vertices
	// to allow for correct normal creation using nointerpolated values in the pixel shader
	const uint16_t fold0 = segments;
	const uint16_t fold1 = segments + 1;

	std::vector<uint16_t> indices;
	indices.reserve(segments * 6 + 3);

	auto addTri = [&](uint16_t a, uint16_t b, uint16_t c) {
		// if 'a' is on the fold
		if (a == fold0 || a == fold1) {
			// if 'b' is not along the fold
			if (b != fold0 && b != fold1) {
				// use 'b' as the provoking vertex
				indices.push_back(b);
				indices.push_back(c);
				indices.push_back(a);
				return;
			}
			// otherwise use 'c' as the provoking vertex
			indices.push_back(c);
			indices.push_back(a);
			indices.push_back(b);
			return;
		}

		// otherwise just emit in abc order
		indices.push_back(a);
		indices.push_back(b);
		indices.push_back(c);
	};

	for (uint16_t i = 0; i < segments; ++i) {
		uint16_t v0 = 2 * i + 0;
		uint16_t v1 = 2 * i + 1;
		uint16_t v2 = 2 * (i + 1) + 0;
		uint16_t v3 = 2 * (i + 1) + 1;

		// first triangle in the quad
		addTri(v0, v1, v2);

		// second triangle in the quad
		addTri(v2, v1, v3);
	}

	// last “cap” triangle
	uint16_t base = segments * 2;
	addTri(base, base + 1, base + 2);

	return indices;
}

void ProceduralGrass::PostDepthRendering()
{
	const auto ctx = globals::d3d::context;
	const auto renderer = globals::game::renderer;

	const auto player = RE::PlayerCharacter::GetSingleton();

	if (!settings.Enabled || !player) {
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

	PostDepthRenderPrep(ctx, renderer);
	GenerateBlades(ctx);
	RenderDepth(ctx);

	CopyDepthBuffer(ctx, renderer);

	ctx->RSSetState(oldRS);
	ctx->OMSetDepthStencilState(oldDSS, oldRef);
	ctx->OMSetBlendState(oldBS, oldBlendFactor, oldSampleMask);

	if (oldRS) {
		oldRS->Release();
		oldRS = nullptr;
	}
	if (oldDSS) {
		oldDSS->Release();
		oldDSS = nullptr;
	}
	if (oldBS) {
		oldBS->Release();
		oldBS = nullptr;
	}

	globals::state->EndPerfEvent();
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
}

void ProceduralGrass::GetVisibleQuadrants()
{
	quadrantsHighLOD.clear();
	quadrantsMidLOD.clear();
	quadrantsLowLOD.clear();

	const auto cells = globals::game::tes->gridCells;
	if (!cells)
		return;

	auto quadrant = Quadrant{};
	const auto cellCount = cells->length * cells->length;

	const auto& playerNiPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	const auto& playerPos = reinterpret_cast<float3 const&>(playerNiPos);
	const int playerQuadrantX = static_cast<int>(std::floor(playerPos.x / 2048.0f));
	const int playerQuadrantY = static_cast<int>(std::floor(playerPos.y / 2048.0f));

	for (uint32_t i = 0; i < cellCount; i++) {
		if (const auto cell = cells->cells[i]) {
			const auto runtimeData = cell->GetRuntimeData();
			quadrant.cellX = runtimeData.cellData.exterior->cellX;
			quadrant.cellY = runtimeData.cellData.exterior->cellY;

			if (const auto land = runtimeData.cellLand) {
				for (uint32_t j = 0; j < 4; j++) {
					if (const auto mesh = land->loadedData->mesh[j]) {
						if (mesh->GetFlags().all(RE::NiAVObject::Flag::kPreProcessedNode)) {
							quadrant.quadrantX = j % 2;
							quadrant.quadrantY = j / 2;

							const int32_t worldQuadrantX = quadrant.cellX * 2 + static_cast<int32_t>(quadrant.quadrantX);
							const int32_t worldQuadrantY = quadrant.cellY * 2 + static_cast<int32_t>(quadrant.quadrantY);
							const int32_t xDiff = abs(playerQuadrantX - worldQuadrantX);
							const int32_t yDiff = abs(playerQuadrantY - worldQuadrantY);

							if (xDiff <= 1 && yDiff <= 1)
								quadrantsHighLOD.push_back(quadrant);
							else if (xDiff <= 2 && yDiff <= 2)
								quadrantsMidLOD.push_back(quadrant);
							else
								quadrantsLowLOD.push_back(quadrant);
						}
					}
				}
			}
		}
	}
}

void ProceduralGrass::PostDepthRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer)
{
	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC texDesc;
	mainTex.texture->GetDesc(&texDesc);

	SetViewport(ctx, texDesc.Width, texDesc.Height);

	const auto viewProjMat = Util::GetCameraData(0).viewProjMat;
	const auto& row0 = viewProjMat.m[0];
	const auto& row1 = viewProjMat.m[1];

	
	// float dt = fpsTimer.tick();
	windTimer = windTimer + fpsTimer.tick();
	if (windTimer > 1.0f) {
		windTimer -= 1.0f;
	}

	auto grassGlobals = GrassGlobals{};
	grassGlobals.voronoiGridSize = static_cast<float>(voronoiGridSize);
	grassGlobals.inverseVoronoiGridSize = 1.0f / grassGlobals.voronoiGridSize;
	grassGlobals.cameraViewRow0Sum = abs(row0[0]) + abs(row0[1]) + abs(row0[2]);
	grassGlobals.cameraViewRow1Sum = abs(row1[0]) + abs(row1[1]) + abs(row1[2]);
	grassGlobals.dynamicResolutionInverted = float2(1.0f / texDesc.Width, 1.0f / texDesc.Height);
	grassGlobals.color = float4(color.x, color.y, color.z, 1);
	grassGlobals.windSpeed = windSpeed;
	grassGlobals.windDir = windDirection;
	grassGlobals.windTimer = windTimer;
	grassGlobalsCB->Update(grassGlobals);

	auto grassTypes = GrassTypesArray{};
	auto& type = grassTypes.grassType[1];
	type.minAO = ao;
	type.tipWeight = tipWeight;
	type.clumpDistanceFactor = clumpDistanceFactor;
	type.clumpFacingFactor = clumpFacingFactor;
	type.clumpHeightFactor = clumpHeightFactor;
	type.height = grassHeight;
	type.width = grassWidth;
	type.stiffness = stiffness;
	type.mid = mid;
	type.minMaxSubsurfaceOpacity = subsurfaceOpacity;
	type.rotationalStiffness = rotationalStiffness;
	type.specular = specular;
	type.phaseLag = phaseLag;
	type.phaseOffset = phaseOffset;
	type.spatialFreq = spatialFreq;
	grassTypesArrayCB->Update(grassTypes);

	ID3D11Buffer* buffers[2] = { *globals::game::perFrame, nullptr };
	if (REL::Module::IsVR()) {
		static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
		buffers[2] = *VRValues.get();
	}
	ctx->VSSetConstantBuffers(12, 2, buffers);
	ctx->CSSetConstantBuffers(12, 2, buffers);

	ID3D11Buffer* grassBuffers[2] = { grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ctx->CSSetConstantBuffers(8, 2, grassBuffers);
	ctx->VSSetConstantBuffers(8, 2, grassBuffers);

	const auto state = globals::state;
	auto sharedDataCB = state->sharedDataCB->CB();
	auto featureDataCB = state->featureDataCB->CB();
	ctx->VSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->CSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->VSSetConstantBuffers(6, 1, &featureDataCB);

	ID3D11ShaderResourceView* heightAndGrassMaps[2] = { heightMap.get(), grassMap.get() };
	ctx->CSSetShaderResources(0, 2, heightAndGrassMaps);

	ctx->CSSetSamplers(0, 1, &linearClampSampler);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
}

void ProceduralGrass::SetViewport(ID3D11DeviceContext* ctx, const uint32_t width, const uint32_t height)
{
	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = static_cast<float>(width);
	vp.Height = static_cast<float>(height);
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;

	ctx->RSSetViewports(1, &vp);
}

void ProceduralGrass::GenerateBlades(ID3D11DeviceContext* ctx) const
{
	globals::state->BeginPerfEvent("Procedural Grass - Blade Generation");

	grassRendererHighLOD->GenerateBlades(ctx, quadrantsHighLOD, 61, 60);
	grassRendererMidLOD->GenerateBlades(ctx, quadrantsMidLOD, 61, 60);
	grassRendererLowLOD->GenerateBlades(ctx, quadrantsLowLOD, 61, 60);

	ID3D11UnorderedAccessView* uavs[2] = { nullptr, nullptr };
	ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

	globals::state->EndPerfEvent();
}

void ProceduralGrass::RenderDepth(ID3D11DeviceContext* ctx) const
{
	globals::state->BeginPerfEvent("Procedural Grass - Depth");

	ctx->RSSetState(noCullRS);
	ctx->OMSetDepthStencilState(depthWriteDS, 0);
	ctx->OMSetBlendState(depthOnlyBlend, nullptr, 0xFFFFFFFF);

	grassRendererHighLOD->RenderDepth(ctx);
	grassRendererMidLOD->RenderDepth(ctx);
	grassRendererLowLOD->RenderDepth(ctx);

	globals::state->EndPerfEvent();
}

void ProceduralGrass::DeferredRendering() const
{
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!player)
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

	if (oldRS) {
		oldRS->Release();
		oldRS = nullptr;
	}
	if (oldDSS) {
		oldDSS->Release();
		oldDSS = nullptr;
	}
	if (oldBS) {
		oldBS->Release();
		oldBS = nullptr;
	}

	// render targets are cleared and set already, this prevents the game from setting the state incorrectly
	globals::game::stateUpdateFlags->reset(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void ProceduralGrass::DeferredRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer) const
{
	const auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	const auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	ID3D11RenderTargetView* rtvs[7] = {
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_DOWNSCALED].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kINDIRECT].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kINDIRECT_DOWNSCALED].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT].RTV,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS].RTV,
	};

	ClearRenderTargets(ctx, rtvs);

	D3D11_TEXTURE2D_DESC texDesc;
	mainTex.texture->GetDesc(&texDesc);

	SetViewport(ctx, texDesc.Width, texDesc.Height);

	ctx->OMSetRenderTargets(7, rtvs, mainDepth.views[0]);

	auto& shadowMask = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];
	ctx->PSSetShaderResources(14, 1, &shadowMask.SRV);
	ctx->PSSetSamplers(14, 1, &shadowSampler);

	static auto& precipOcclusionTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
	ctx->PSSetShaderResources(70, 1, &precipOcclusionTexture.depthSRV);

	const auto state = globals::state;
	auto sharedDataCB = state->sharedDataCB->CB();
	auto featureDataCB = state->featureDataCB->CB();
	ctx->PSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->VSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->PSSetConstantBuffers(6, 1, &featureDataCB);

	ID3D11Buffer* buffers[1] = { *globals::game::perFrame };
	ID3D11Buffer* vrBuffer = nullptr;
	if (REL::Module::IsVR()) {
		static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
		vrBuffer = *VRValues.get();
	}
	if (vrBuffer) {
		ctx->VSSetConstantBuffers(12, 1, buffers);
		ctx->VSSetConstantBuffers(13, 1, &vrBuffer);
		ctx->PSSetConstantBuffers(12, 1, buffers);
		ctx->PSSetConstantBuffers(13, 1, &vrBuffer);
	} else {
		ctx->PSSetConstantBuffers(12, 1, buffers);
		ctx->VSSetConstantBuffers(12, 1, buffers);
	}

	if (globals::features::lightLimitFix->loaded) {
		auto strictLightDataCB = globals::features::lightLimitFix->strictLightDataCB->CB();
		ctx->PSSetConstantBuffers(3, 1, &strictLightDataCB);
	}

	if (globals::features::skylighting->loaded) {
		ID3D11ShaderResourceView* srvs[2] = { globals::features::skylighting->texProbeArray->srv.get(), globals::features::skylighting->stbn_vec3_2Dx1D_128x128x64.get() };
		ctx->PSSetShaderResources(50, 2, srvs);
	}

	const auto grassTypesCBa = grassTypesArrayCB->CB();
	ctx->VSSetConstantBuffers(9, 1, &grassTypesCBa);

	ctx->PSSetShaderResources(48, 1, &grassDiffuseRoughnessArraySRV);
	ctx->PSSetShaderResources(49, 1, &grassColorLUTsArraySRV);

	ctx->PSSetSamplers(15, 1, &linearClampSampler);  // TODO restore original sampler when done, also only set once

	ctx->OMSetDepthStencilState(depthEqualDS, 0);
	ctx->RSSetState(noCullRS);

	ID3D11Buffer* grassBuffers[2] = { grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ctx->VSSetConstantBuffers(8, 2, grassBuffers);
	ctx->PSSetConstantBuffers(8, 2, grassBuffers);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
}

void ProceduralGrass::ClearRenderTargets(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtvs[7])
{
	constexpr float black[4] = { 0, 0, 0, 0 };
	for (uint i = 2; i < 7; i++) {
		ctx->ClearRenderTargetView(rtvs[i], black);
	}
}

void ProceduralGrass::RenderGrass(ID3D11DeviceContext* ctx) const
{
	globals::state->BeginPerfEvent("Procedural Grass - Deferred");

	grassRendererHighLOD->RenderGrass(ctx);
	grassRendererMidLOD->RenderGrass(ctx);
	grassRendererLowLOD->RenderGrass(ctx);

	globals::state->EndPerfEvent();
}
