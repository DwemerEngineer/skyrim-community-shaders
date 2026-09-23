#include "ProceduralGrass.h"

#include "GrassCollision.h"
#include "DynamicCubemaps.h"
#include "HiZPyramid.h"
#include "IBL.h"
#include "LightLimitFix.h"
#include "LinearLighting.h"
#include "ProceduralGrass/TopDownOcclusion.h"
#include "ShaderCache.h"
#include "Skylighting.h"
#include "State.h"
#include "TerrainBlending.h"
#include "TerrainHeightMap.h"
#include "Utils/Serialize.h"
#include "Utils/game.h"

#include <numbers>

using namespace PGrassCommon;

namespace
{
	// Preserve full density at the Low/Far handoff, then retain this fraction in distant Far regions.
	constexpr float FarPerformanceKeep = 0.55f;
	constexpr uint32_t GrassMaterialDetailDim = 64;
	constexpr uint32_t GrassMaterialDetailVariants = 4;
	constexpr float GrassMaterialDetailNormalRange = 1.25f;

	uint32_t GrassMaterialDetailHash(uint32_t x, uint32_t y)
	{
		x ^= y * 0x9E3779B9u;
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		return x ^ (x >> 16);
	}

	float GrassMaterialDetailTexel(int32_t x, int32_t y, bool grain)
	{
		const uint32_t wrappedX = static_cast<uint32_t>(x) & (GrassMaterialDetailDim - 1u);
		const uint32_t wrappedY = static_cast<uint32_t>(y) & (GrassMaterialDetailDim - 1u);
		const uint32_t hash = grain ?
			GrassMaterialDetailHash(wrappedX, wrappedY) :
			GrassMaterialDetailHash(wrappedX / 8u, wrappedY / 8u);
		return static_cast<float>(hash >> 24) * (1.0f / 255.0f);
	}

	float SampleGrassMaterialDetail(float u, float v, bool grain)
	{
		const float x = u * GrassMaterialDetailDim - 0.5f;
		const float y = v * GrassMaterialDetailDim - 0.5f;
		const int32_t x0 = static_cast<int32_t>(std::floor(x));
		const int32_t y0 = static_cast<int32_t>(std::floor(y));
		const float fx = x - x0;
		const float fy = y - y0;
		const float a = GrassMaterialDetailTexel(x0, y0, grain);
		const float b = GrassMaterialDetailTexel(x0 + 1, y0, grain);
		const float c = GrassMaterialDetailTexel(x0, y0 + 1, grain);
		const float d = GrassMaterialDetailTexel(x0 + 1, y0 + 1, grain);
		return std::lerp(std::lerp(a, b, fx), std::lerp(c, d, fx), fy);
	}

	float Smoothstep(float edge0, float edge1, float value)
	{
		const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	template <class T>
	void ReleaseAndNull(T*& resource)
	{
		if (resource) {
			resource->Release();
			resource = nullptr;
		}
	}
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

	ReleaseAndNull(densityAOVS);
	ReleaseAndNull(densityAOPS);
	ReleaseAndNull(depthClipPS);
	ReleaseAndNull(densityGatherCS);
	ReleaseAndNull(distantAmbientLUTCS);

	densityAOVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOVS.hlsl", {}, "vs_5_0"));
	densityAOPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOPS.hlsl", {}, "ps_5_0"));
	depthClipPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDepthPS.hlsl", {}, "ps_5_0"));
	densityGatherCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityGatherCS.hlsl", {}, "cs_5_0"));
	distantAmbientLUTCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassAmbientLUTCS.hlsl", {}, "cs_5_0"));
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

void ProceduralGrass::SetupResources()
{
	auto device = globals::d3d::device;

	quadrantsHighLOD.reserve(HighTierQuadrantCap);
	quadrantsMidLOD.reserve(MidTierQuadrantCap);
	quadrantsLowLOD.reserve(LowTierQuadrantCap);
	quadrantsFarLOD.reserve(FarQuadrantCount);
	quadrantsPresence.reserve(LowTierQuadrantCap);
	grassMapCache.reserve(grassMapCacheCapacity);

	globals::terrainHeightMap->Discover();
	globals::topDownOcclusion->SetupResources();
	// Snap the shared window to the density grid so terrain darkening stays stable at grass edges.
	globals::topDownOcclusion->SetSnapDim(grassDensityDim);

	grassGlobalsCB = new ConstantBuffer(ConstantBufferDesc<GrassGlobals>());
	grassTypesArrayCB = new ConstantBuffer(ConstantBufferDesc<GrassTypesArray>());
	grassGeneratorTypesCB = new ConstantBuffer(ConstantBufferDesc<GrassGeneratorTypesArray>());

	const auto makeIndexBuffer = [](const std::vector<uint16_t>& indices, const char* name) {
		D3D11_BUFFER_DESC desc{};
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
		D3D11_SUBRESOURCE_DATA init{ indices.data(), 0, 0 };
		return new Buffer(desc, &init, name);
	};

	auto vertexIndicesHigh = CreateVertexIndicesArray(15);
	vertexIndicesHighBuffer = makeIndexBuffer(vertexIndicesHigh, "PGrass::HighIndices");
	auto vertexIndicesHighOuter = CreateVertexIndicesArray(7);
	vertexIndicesHighOuterBuffer = makeIndexBuffer(vertexIndicesHighOuter, "PGrass::HighOuterIndices");

	auto vertexIndicesLow = CreateVertexIndicesArray(7);
	D3D11_BUFFER_DESC lowIbd{};
	lowIbd.Usage = D3D11_USAGE_IMMUTABLE;
	lowIbd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	lowIbd.ByteWidth = static_cast<UINT>(vertexIndicesLow.size() * sizeof(uint16_t));
	lowIbd.CPUAccessFlags = 0;
	D3D11_SUBRESOURCE_DATA lowIbdInit{ vertexIndicesLow.data(), 0, 0 };
	vertexIndicesLowBuffer = new Buffer(lowIbd, &lowIbdInit);

	// Mid uses five vertices and three triangles. Low keeps denser geometry for the closer overlap.
	auto vertexIndicesMid = CreateVertexIndicesArray(5);
	D3D11_BUFFER_DESC midIbd{};
	midIbd.Usage = D3D11_USAGE_IMMUTABLE;
	midIbd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	midIbd.ByteWidth = static_cast<UINT>(vertexIndicesMid.size() * sizeof(uint16_t));
	midIbd.CPUAccessFlags = 0;
	D3D11_SUBRESOURCE_DATA midIbdInit{ vertexIndicesMid.data(), 0, 0 };
	vertexIndicesMidBuffer = new Buffer(midIbd, &midIbdInit);

	// Far uses one tapered triangle because finer geometry is not visible at this distance.
	auto vertexIndicesFar = CreateVertexIndicesArray(3);
	D3D11_BUFFER_DESC farIbd{};
	farIbd.Usage = D3D11_USAGE_IMMUTABLE;
	farIbd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	farIbd.ByteWidth = static_cast<UINT>(vertexIndicesFar.size() * sizeof(uint16_t));
	farIbd.CPUAccessFlags = 0;
	D3D11_SUBRESOURCE_DATA farIbdInit{ vertexIndicesFar.data(), 0, 0 };
	vertexIndicesFarBuffer = new Buffer(farIbd, &farIbdInit);

	uint32_t threadGroupSize = 64;
	// Add steepness-gated slope-fill candidates per patch. Low needs the most to fill sparse steep ground.
	const bool cacheCollision = globals::features::grassCollision.loaded;
	const uint32_t highBladeStride = cacheCollision ? sizeof(PGrassCommon::BladeSkylitCollision) : sizeof(PGrassCommon::BladeSkylit);
	const uint32_t midBladeStride = cacheCollision ? sizeof(PGrassCommon::BladeSkylitMidCollision) : sizeof(PGrassCommon::BladeSkylit);
	grassRendererHighLOD = new PGrassRenderer<PGrassCommon::HighTierQuadrantCap, 4>(QualityDensities[settings.Quality], threadGroupSize, vertexIndicesHighBuffer,
		"HIGH_LOD", "HIGH_VERTEX", nullptr, 1, highBladeStride, vertexIndicesHighOuterBuffer);
	grassRendererMidLOD = new PGrassRenderer<PGrassCommon::MidTierQuadrantCap, 2>(static_cast<uint32_t>(settings.midGrassDensity), threadGroupSize, vertexIndicesMidBuffer, "MID_LOD", "MID_VERTEX", nullptr, 1, midBladeStride);
	grassRendererLowLOD = new PGrassRenderer<PGrassCommon::LowTierQuadrantCap, 1>(static_cast<uint32_t>(settings.lowGrassDensity), threadGroupSize, vertexIndicesLowBuffer, "LOW_LOD", "LOW_VERTEX", nullptr, 5, sizeof(PGrassCommon::BladeSkylit));
	grassRendererFarLOD = new PGrassRenderer<PGrassCommon::FarQuadrantCount, 1>(FarPatchDensity(), threadGroupSize, vertexIndicesFarBuffer, "LOW_LOD", "FAR_VERTEX", "FAR_LOD", 2, sizeof(PGrassCommon::BladeFar));

	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	device->CreateSamplerState(&samplerDesc, &linearClampSampler);

	D3D11_TEXTURE2D_DESC detailDesc{};
	detailDesc.Width = GrassMaterialDetailDim;
	detailDesc.Height = GrassMaterialDetailDim;
	detailDesc.MipLevels = 1;
	detailDesc.ArraySize = PGrassCommon::MaxGrassTypes * GrassMaterialDetailVariants;
	detailDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	detailDesc.SampleDesc.Count = 1;
	detailDesc.Usage = D3D11_USAGE_DEFAULT;
	detailDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	grassMaterialDetailTexture = new Texture2D(detailDesc, "PGrass::MaterialDetail");
	D3D11_SHADER_RESOURCE_VIEW_DESC detailSRVDesc{};
	detailSRVDesc.Format = detailDesc.Format;
	detailSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	detailSRVDesc.Texture2DArray.MostDetailedMip = 0;
	detailSRVDesc.Texture2DArray.MipLevels = 1;
	detailSRVDesc.Texture2DArray.FirstArraySlice = 0;
	detailSRVDesc.Texture2DArray.ArraySize = detailDesc.ArraySize;
	grassMaterialDetailTexture->CreateSRV(detailSRVDesc);

	D3D11_SAMPLER_DESC detailSamplerDesc = samplerDesc;
	detailSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	detailSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	device->CreateSamplerState(&detailSamplerDesc, &grassDetailSampler);

	D3D11_SAMPLER_DESC shadowSamplerDesc = {};
	shadowSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	shadowSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	shadowSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	shadowSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	shadowSamplerDesc.MipLODBias = 0;
	shadowSamplerDesc.MaxAnisotropy = 1;
	shadowSamplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	shadowSamplerDesc.MinLOD = -FLT_MAX;
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
		rd.ScissorEnable = TRUE;
		device->CreateRasterizerState(&rd, &noCullScissorRS);
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

	if (!terrainFadeBlend) {
		D3D11_BLEND_DESC bd = {};
		bd.IndependentBlendEnable = TRUE;
		for (uint32_t i = 0; i < 7; i++) {
			auto& target = bd.RenderTarget[i];
			target.BlendEnable = TRUE;
			target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			target.BlendOp = D3D11_BLEND_OP_ADD;
			target.SrcBlendAlpha = D3D11_BLEND_ONE;
			target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		device->CreateBlendState(&bd, &terrainFadeBlend);
	}

	if (!multiplyBlend) {
		// Multiply destination RGB by the terrain-darkening factor.
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		device->CreateBlendState(&bd, &multiplyBlend);
	}

	if (!noDepthDSS) {
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		device->CreateDepthStencilState(&dd, &noDepthDSS);
	}

	if (!grassDensityTexture) {
		D3D11_TEXTURE2D_DESC td{};
		td.Width = grassDensityDim;
		td.Height = grassDensityDim;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_UINT;
		td.SampleDesc = { 1, 0 };
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		grassDensityTexture = new Texture2D(td);
		Util::SetResourceName(grassDensityTexture->resource.get(), "PGrass::GrassDensity");

		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = td.Format;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		grassDensityTexture->CreateSRV(sd);

		D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = td.Format;
		ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		grassDensityTexture->CreateUAV(ud);
	}

	if (!distantAmbientLUT) {
		D3D11_TEXTURE2D_DESC td{};
		td.Width = distantAmbientLUTDim;
		td.Height = distantAmbientLUTDim;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		td.SampleDesc = { 1, 0 };
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		distantAmbientLUT = new Texture2D(td);
		Util::SetResourceName(distantAmbientLUT->resource.get(), "PGrass::DistantAmbientLUT");

		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = td.Format;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		distantAmbientLUT->CreateSRV(sd);

		D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = td.Format;
		ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		distantAmbientLUT->CreateUAV(ud);
	}

	if (!grassPresenceTexture) {
		D3D11_TEXTURE2D_DESC td{};
		td.Width = grassPresenceDim;
		td.Height = grassPresenceDim;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UINT;
		td.SampleDesc = { 1, 0 };
		td.Usage = D3D11_USAGE_DEFAULT;  // rewritten each frame via UpdateSubresource (window scrolls with the player)
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		grassPresenceTexture = new Texture2D(td);
		Util::SetResourceName(grassPresenceTexture->resource.get(), "PGrass::GrassPresence");

		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = td.Format;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		grassPresenceTexture->CreateSRV(sd);

		grassPresenceStaging.assign(static_cast<size_t>(grassPresenceDim) * grassPresenceDim, 0);
	}

	if (!densityGatherCS)
		densityGatherCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityGatherCS.hlsl", {}, "cs_5_0"));
	if (!distantAmbientLUTCS)
		distantAmbientLUTCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassAmbientLUTCS.hlsl", {}, "cs_5_0"));
	if (!densityAOVS)
		densityAOVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOVS.hlsl", {}, "vs_5_0"));
	if (!densityAOPS)
		densityAOPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOPS.hlsl", {}, "ps_5_0"));
	if (!depthClipPS)
		depthClipPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDepthPS.hlsl", {}, "ps_5_0"));
}

std::vector<uint16_t> ProceduralGrass::CreateVertexIndicesArray(uint16_t vertCount)
{
	assert(vertCount >= 3 && ((vertCount - 3) % 2) == 0);
	const uint16_t segments = (vertCount - 3) / 2;

	// Keep fold vertices from becoming provoking vertices so flat normals remain correct.
	const uint16_t fold0 = segments;
	const uint16_t fold1 = segments + 1;

	std::vector<uint16_t> indices;
	indices.reserve(segments * 6 + 3);

	// Rotate each triangle so a non-fold vertex is provoking while preserving winding.
	auto addTri = [&](uint16_t a, uint16_t b, uint16_t c) {
		if (a == fold0 || a == fold1) {
			if (b != fold0 && b != fold1) {
				// Use b as the provoking vertex.
				indices.push_back(b);
				indices.push_back(c);
				indices.push_back(a);
				return;
			}
			// Otherwise use c as the provoking vertex.
			indices.push_back(c);
			indices.push_back(a);
			indices.push_back(b);
			return;
		}

		indices.push_back(a);
		indices.push_back(b);
		indices.push_back(c);
	};

	for (uint16_t i = 0; i < segments; ++i) {
		uint16_t v0 = 2 * i + 0;
		uint16_t v1 = 2 * i + 1;
		uint16_t v2 = 2 * (i + 1) + 0;
		uint16_t v3 = 2 * (i + 1) + 1;

		addTri(v0, v1, v2);
		addTri(v2, v1, v3);
	}

	// Last cap triangle
	uint16_t base = segments * 2;
	addTri(base, base + 1, base + 2);

	return indices;
}

std::string ProceduralGrass::LandTextureKey(const RE::TESLandTexture* tex)
{
	if (!tex)
		return {};
	const RE::TESFile* file = tex->GetFile(0);
	if (!file)
		return {};
	return std::format("{}|0x{:06X}", file->GetFilename(), tex->GetLocalFormID());
}

void ProceduralGrass::RebuildTypeAllocation()
{
	typeAllocation.clear();
	textureSelection.clear();
	textureSelectionByTexture.clear();
	grassTypesDirty = true;
	typeAllocation.reserve(PGrassCommon::MaxGrassTypes - 2);
	textureSelection.reserve(settings.textureTypes.size());
	textureSelectionByTexture.reserve(settings.textureTypes.size());

	// Sort keys so cached type ids remain stable across frames.
	std::vector<std::string> keys;
	keys.reserve(settings.textureTypes.size());
	for (const auto& [key, defs] : settings.textureTypes)
		if (!defs.empty())
			keys.push_back(key);

	std::sort(keys.begin(), keys.end());

	// Slots 0 and 1 are reserved for bare and base grass.
	for (const auto& key : keys) {
		const auto& defs = settings.textureTypes[key];
		TextureSelection sel;
		float acc = 0.0f;

		for (uint32_t i = 0; i < defs.size(); i++) {
			if (defs[i].noGrass) {
				sel.ids.push_back(0u);
			} else {
				if (typeAllocation.size() + 2 >= PGrassCommon::MaxGrassTypes)
					break;  // Remaining variants use the base type.
				sel.ids.push_back(static_cast<uint8_t>(typeAllocation.size() + 2));
				typeAllocation.emplace_back(key, i);
			}
			acc += std::max(0.0f, defs[i].weight);
			sel.cumulative.push_back(acc);
		}

		sel.total = acc;
		if (!sel.ids.empty())
			textureSelection[key] = std::move(sel);
	}
}

PGrassCommon::GrassType ProceduralGrass::ResolveGrassType(const nlohmann::json& typeOverride) const
{
	// Present keys override the base setting. Missing keys inherit it.
	static const nlohmann::json emptyObject = nlohmann::json::object();
	const nlohmann::json& ov = typeOverride.is_object() ? typeOverride : emptyObject;
	const auto& s = settings;

	const auto packColor = [](const float3& c) { return float4(c.x, c.y, c.z, 0.0f); };

	PGrassCommon::GrassType t{};
	t.height = ov.value("Height", s.grassHeight);
	t.width = ov.value("Width", s.grassWidth);
	t.minSlope = std::cos(ov.value("MinSlope", s.grassMinSlope) * (std::numbers::pi_v<float> / 180.0f));
	t.maxSlope = std::cos(ov.value("MaxSlope", s.grassMaxSlope) * (std::numbers::pi_v<float> / 180.0f));
	t.stiffness = ov.value("Stiffness", s.stiffness);
	t.rotationalStiffness = ov.value("RotationalStiffness", s.rotationalStiffness);
	t.tipWeight = ov.value("TipWeight", s.tipWeight);
	t.mid = ov.value("Mid", s.mid);

	t.clumpDistanceFactor = ov.value("ClumpDistanceFactor", s.clumpDistanceFactor);
	t.clumpFacingFactor = ov.value("ClumpFacingFactor", s.clumpFacingFactor);
	t.clumpHeightFactor = ov.value("ClumpHeightFactor", s.clumpHeightFactor);
	t.clumpAOStrength = ov.value("ClumpAOStrength", s.clumpAOStrength);
	t.clumpColorStrength = ov.value("ClumpColorStrength", s.grassClumpColorStrength);

	t.spatialFreq = ov.value("SpatialFreq", s.spatialFreq);
	t.phaseLag = ov.value("PhaseLag", s.phaseLag);
	t.phaseOffset = ov.value("PhaseOffset", s.phaseOffset);
	t.minAO = ov.value("MinAO", s.ao);
	t.specular = ov.value("Specular", s.specular);
	t.minMaxSubsurfaceOpacity = ov.value("SubsurfaceOpacity", s.subsurfaceOpacity);
	t.grassSubsurfaceColor = packColor(ov.value("SubsurfaceTint", s.grassSubsurfaceTint));
	t.grassSurfParams = float4(
		ov.value("WaxSheenStrength", s.waxSheenStrength),
		ov.value("AmbientFlatten", s.grassAmbientFlatten),
		ov.value("Wrap", s.grassWrap),
		ov.value("WaxRoughnessMultiplier", s.waxRoughnessMultiplier));
	const float3 rough = ov.value("BaseMinTipRoughness", s.baseMinTipRoughness);
	const float roughnessStart = ov.value("TipRoughnessStart", s.tipRoughnessStart);
	t.baseMinTipRoughnessStart = float4(rough.x, rough.y, rough.z, roughnessStart);
	// Fit Mid roughness at its three vertex positions to avoid evaluating both smoothstep curves in the vertex shader.
	const auto smoothstep = [](float edge0, float edge1, float value) {
		if (edge0 == edge1)
			return value < edge0 ? 0.0f : 1.0f;
		const float x = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return x * x * (3.0f - 2.0f * x);
	};
	const float roughnessAtMidFirst = std::lerp(rough.x, rough.y, smoothstep(0.0f, roughnessStart, 0.5f));
	const float roughnessAtMid = std::lerp(roughnessAtMidFirst, rough.z, smoothstep(rough.x, 1.0f, 0.5f));
	const float baseToMid = roughnessAtMid - rough.x;
	const float baseToTip = rough.z - rough.x;
	t.midRoughnessPolynomial = float4(2.0f * baseToTip - 8.0f * baseToMid, 8.0f * baseToMid - baseToTip, rough.x, 0.0f);
	t.grassTypeLightParams = float4(
		ov.value("BounceStrength", s.grassBounceStrength),
		1.0f,  // sky translucency (fixed)
		ov.value("SpecOcclusion", s.grassSpecOcclusion),
		ov.value("AmbientDesat", s.grassAmbientDesat));

	t.baseColor = packColor(ov.value("BaseColor", s.baseColor));
	t.tipColor = packColor(ov.value("TipColor", s.tipColor));
	t.grassColorTipDry = packColor(ov.value("ColorTipDry", s.grassColorTipDry));
	t.grassColorVar = float4(
		ov.value("HueVariation", s.grassColorHueVariation),
		ov.value("ValueVariation", s.grassColorValueVariation),
		ov.value("TipDryStrength", s.grassColorTipDryStrength),
		ov.value("MottleStrength", s.grassColorMottleStrength));
	t.grassColorCool = packColor(ov.value("ColorCool", s.grassColorCool));
	t.grassColorWarm = packColor(ov.value("ColorWarm", s.grassColorWarm));
	t.grassBounceColor = packColor(ov.value("BounceColor", s.grassBounceColor));
	t.grassTextureParams = float4(
		ov.value("BlotchStrength", s.grassBlotchStrength),
		ov.value("BlotchScale", s.grassBlotchScale),
		ov.value("SpeckleStrength", s.grassSpeckleStrength),
		ov.value("SpeckleScale", s.grassSpeckleScale));

	const float3 veinTint = ov.value("VeinTint", s.grassVeinTint);
	t.grassVeinParams = float4(veinTint.x, veinTint.y, veinTint.z, ov.value("VeinAlbedoStrength", s.grassVeinAlbedoStrength));
	t.grassVeinParams2 = float4(
		ov.value("VeinNormalStrength", s.grassVeinNormalStrength),
		ov.value("VeinRippleDepth", s.grassVeinRippleDepth),
		ov.value("VeinWiggleAmount", s.grassVeinWiggleAmount),
		ov.value("CurvedNormalStrength", s.curvedNormalStrength));

	return t;
}

void ProceduralGrass::UpdateGrassMaterialDetailTexture()
{
	if (!grassMaterialDetailTexture)
		return;

	const auto encodeUNorm8 = [](float value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
	};

	std::vector<uint8_t> detailData(GrassMaterialDetailDim * GrassMaterialDetailDim * 4u);
	const uint32_t activeTypeCount = std::min<uint32_t>(static_cast<uint32_t>(typeAllocation.size()) + 2u, MaxGrassTypes);
	for (uint32_t typeIndex = 0; typeIndex < activeTypeCount; ++typeIndex) {
		const auto& type = resolvedGrassTypes.grassType[typeIndex];
		const float blotchScale = std::max(type.grassTextureParams.y, 0.0f);
		const float speckleScale = std::max(type.grassTextureParams.w, 0.0f);
		const float veinStrength = type.grassVeinParams2.x;
		const float veinRippleDepth = type.grassVeinParams2.y;
		const float veinWiggleAmount = type.grassVeinParams2.z;

		for (uint32_t variant = 0; variant < GrassMaterialDetailVariants; ++variant) {
			const uint32_t offsetHash = GrassMaterialDetailHash(typeIndex * GrassMaterialDetailVariants + variant, variant);
			const float noiseOffsetX = static_cast<float>(offsetHash & 0xFFFFu) * (1.0f / 65536.0f);
			const float noiseOffsetY = static_cast<float>(offsetHash >> 16) * (1.0f / 65536.0f);
			const float phase = static_cast<float>(GrassMaterialDetailHash(variant, typeIndex) >> 8) * (std::numbers::pi_v<float> * 2.0f / 16777216.0f);

			for (uint32_t y = 0; y < GrassMaterialDetailDim; ++y) {
				const float along = (static_cast<float>(y) + 0.5f) * (1.0f / GrassMaterialDetailDim);
				for (uint32_t x = 0; x < GrassMaterialDetailDim; ++x) {
					const float across = (static_cast<float>(x) + 0.5f) * (1.0f / GrassMaterialDetailDim);
					const float blotch = SampleGrassMaterialDetail(across * 0.125f * blotchScale + noiseOffsetX,
						along * 0.5f * blotchScale + noiseOffsetY, false);
					const float grain = SampleGrassMaterialDetail(across * 6.0f * speckleScale + noiseOffsetX * 1.7f,
						along * 26.0f * speckleScale + noiseOffsetY * 1.7f, true);

					const float centreVein = 1.0f - Smoothstep(0.0f, 0.050f, std::abs(across - 0.5f));
					const float sideVeinL = 1.0f - Smoothstep(0.0f, 0.032f, std::abs(across - 0.27f));
					const float sideVeinR = 1.0f - Smoothstep(0.0f, 0.032f, std::abs(across - 0.73f));
					float vein = std::clamp(centreVein + 0.5f * (sideVeinL + sideVeinR), 0.0f, 1.0f);
					vein *= Smoothstep(0.0f, 0.16f, along) * Smoothstep(0.0f, 0.20f, 1.0f - along);
					vein *= (1.0f - veinRippleDepth) + veinRippleDepth * std::sin(along * 26.0f + phase);

					const float normalOffset = (across - 0.5f) * 2.0f * vein * veinStrength +
						std::sin(along * 40.0f + phase) * veinWiggleAmount;
					const uint32_t index = (y * GrassMaterialDetailDim + x) * 4u;
					detailData[index] = encodeUNorm8(blotch);
					detailData[index + 1u] = encodeUNorm8(grain);
					detailData[index + 2u] = encodeUNorm8(vein);
					detailData[index + 3u] = encodeUNorm8(normalOffset * (0.5f / GrassMaterialDetailNormalRange) + 0.5f);
				}
			}

			const uint32_t slice = typeIndex * GrassMaterialDetailVariants + variant;
			globals::d3d::context->UpdateSubresource(grassMaterialDetailTexture->resource.get(), D3D11CalcSubresource(0, slice, 1), nullptr,
				detailData.data(), GrassMaterialDetailDim * 4u, 0);
		}
	}
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
	GenerateBlades(ctx);
	RenderDepth(ctx);

	CopyDepthBuffer(ctx, renderer);

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

	ReleaseAndNull(oldRS);
	ReleaseAndNull(oldDSS);
	ReleaseAndNull(oldBS);
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
	float dirLightZ = 1.0f;
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

					const auto& direction = dirLight->GetWorldDirection();
					float3 dirLightDirection(-direction.x, -direction.y, -direction.z);
					dirLightDirection.Normalize();
					dirLightZ = dirLightDirection.z;

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

	auto grassGlobals = GrassGlobals{};
	grassGlobals.voronoiGridSize = static_cast<float>(settings.voronoiGridSize);
	grassGlobals.inverseVoronoiGridSize = 1.0f / grassGlobals.voronoiGridSize;
	grassGlobals.cameraViewRow0Sum = abs(row0[0]) + abs(row0[1]) + abs(row0[2]);
	grassGlobals.cameraViewRow1Sum = abs(row1[0]) + abs(row1[1]) + abs(row1[2]);
	// Convert viewport-space SV_Position to normalized coordinates before dynamic-resolution adjustment.
	grassGlobals.dynamicResolutionInverted = float2(1.0f / renderSize.x, 1.0f / renderSize.y);

	grassGlobals.windSpeed = settings.windSpeed;
	grassGlobals.previousWindSpeed = previousWindSpeed;
	grassGlobals.windDir = windDirection;
	grassGlobals.windAngle = atan2(windDirection.y, windDirection.x);
	grassGlobals.previousWindDir = previousWindDirection;
	grassGlobals.grassPBRLightingScale = prelinearizeTypeColors ? 1.0f : vanillaPBRLightingScale;

	const auto topDown = globals::topDownOcclusion;
	topDown->SetPaddingWorld(settings.occlusionPadding);  // Pre-pad the map for one generator centre tap.
	grassGlobals.occlusionHalfExtent = topDown->GetHalfExtent();
	grassGlobals.occlusionInvExtent = 1.0f / (topDown->GetHalfExtent() * 2.0f);
	const auto window = topDown->GetWindowCentre();
	// z is underside clearance. A large negative value disables object culling.
	grassGlobals.occlusionParams = float4(window.x, window.y, settings.debugIgnoreObjectOcclusion ? -1.0e9f : settings.occlusionClearance, settings.occlusionBias);

	grassGlobals.grassAOParams = float4((float)grassDensityDim, settings.grassAOStrength, settings.grassAODensity, settings.grassHeight);
	const float canopySunExponent = settings.grassSunSelfShadow * 2.5f / std::max(dirLightZ, 0.25f);
	grassGlobals.grassLightParams = float4(settings.grassDensityAO, settings.grassCanopySkyOcclusion, canopySunExponent, settings.grassBaseAO);
	grassGlobals.grassFrameLight = float4(resolvedDirLightColor.x, resolvedDirLightColor.y, resolvedDirLightColor.z, grassLightingScale);

	const auto farGridCells = globals::game::tes ? globals::game::tes->gridCells : nullptr;
	const int32_t loadedGridLength = farGridCells ? farGridCells->length : 5;
	const int32_t loadedCellRadius = loadedGridLength / 2;
	const int32_t farExtraCells = std::clamp(settings.grassCellRadius, 0, std::max(0, PGrassCommon::FarCellRadiusCap - PGrassCommon::FarStreamGuardCells - loadedCellRadius));
	const float farStart = loadedGridLength * 2048.0f;  // Loaded-grid half extent
	const float farEnd = farStart + std::max(farExtraCells, 1) * 4096.0f;
	grassGlobals.farParams = float4(farStart, 1.0f / (farEnd - farStart), 2048.0f / static_cast<float>(FarPatchDensity()), FarPerformanceKeep);

	previousShaderTimer = shaderTimer;
	previousWindDirection = windDirection;
	previousWindSpeed = settings.windSpeed;
	grassGlobals.miscParams = float4(settings.grassMapEdgeNoise, settings.grassSlopeFacing, settings.grassViewThicken, timerDelta);
	grassGlobals.grassTerrainBlend = float4(settings.grassTerrainBlendStrength, settings.grassTerrainBlendHeight, settings.grassTerrainBlendNormal, settings.grassTerrainBlendRough);

	auto heightMap = globals::terrainHeightMap;
	heightMap->LoadForCurrentWorldspace();

	const auto heightMapScale = heightMap->GetScale();
	grassGlobals.heightMapScale = float2(heightMapScale.x, heightMapScale.y);
	grassGlobals.heightMapOffset = heightMap->GetOffset();
	grassGlobals.heightMapZRange = heightMap->GetPosRange();
	grassGlobals.debugFlags = float2(settings.debugDisableAllCulls ? 1.0f : 0.0f, 0.0f);

	// Presence-map origin, inverse sample spacing, and dimension.
	grassGlobals.grassPresenceParams = float4(grassPresenceOrigin.x, grassPresenceOrigin.y, (float)(QuadrantGrassPitch - 1) / 2048.0f, (float)grassPresenceDim);
	const auto* grassHiZ = globals::hiZPyramid;
	grassGlobals.grassHiZParams = grassHiZ->IsValid() ?
		float4((float)grassHiZ->GetWidth(), (float)grassHiZ->GetHeight(), grassHiZ->GetTexelPixels(), (float)grassHiZ->GetMipCount()) :
		float4::Zero;
	grassGlobals.grassLodOrigin = grassLodOrigin;

	grassGlobalsCB->Update(grassGlobals);

	if (grassTypesDirty) {
		// Slot 0 is bare, slot 1 is base grass, and later slots are texture variants.
		resolvedGrassTypes = {};
		resolvedGeneratorTypes = {};
		resolvedGrassTypes.grassType[1] = ResolveGrassType(nlohmann::json::object());

		for (size_t i = 0; i < typeAllocation.size(); i++) {
			const auto& [key, defIndex] = typeAllocation[i];
			resolvedGrassTypes.grassType[i + 2] = ResolveGrassType(settings.textureTypes[key][defIndex].overrides);
		}

		if (prelinearizeTypeColors) {
			const auto convertTint = [typeColorGamma](float4& tint) {
				tint.x = std::pow(std::abs(tint.x), typeColorGamma);
				tint.y = std::pow(std::abs(tint.y), typeColorGamma);
				tint.z = std::pow(std::abs(tint.z), typeColorGamma);
			};
			for (auto& type : resolvedGrassTypes.grassType) {
				convertTint(type.grassSubsurfaceColor);
				convertTint(type.grassBounceColor);
			}
		}

		float maxHeight = 0.0f;
		float maxNearWidth = 0.0f;
		float maxFarWidth = 0.0f;

		for (uint32_t i = 0; i < MaxGrassTypes; ++i) {
			const auto& source = resolvedGrassTypes.grassType[i];
			resolvedGeneratorTypes.grassType[i] = GrassGeneratorType{
				source.height, source.width, source.minSlope, source.maxSlope,
				source.stiffness, source.rotationalStiffness, source.tipWeight, 0.0f,
				source.clumpDistanceFactor, source.clumpHeightFactor, source.clumpFacingFactor, 0.0f
			};

			maxHeight = std::max(maxHeight, source.height);
			const float baseWidth = source.width * 2.5f * 1.3f;
			maxNearWidth = std::max(maxNearWidth, baseWidth * 2.0f);  // Low is the widest near tier.
			maxFarWidth = std::max(maxFarWidth, baseWidth * 32.0f * 1.6f);  // Match the maximum Far blade widening in the shader.
		}

		grassTypesArrayCB->Update(resolvedGrassTypes);
		grassGeneratorTypesCB->Update(resolvedGeneratorTypes);
		UpdateGrassMaterialDetailTexture();
		// View thickening scales with blade width.
		nearQuadrantFrustumPadding = settings.voronoiGridSize * settings.clumpDistanceFactor + maxHeight + maxNearWidth * (1.0f + settings.grassViewThicken);
		farQuadrantFrustumPadding = maxHeight + maxFarWidth;
		grassTypesDirty = false;
	}

	ID3D11Buffer* buffers[2] = { *globals::game::perFrame, nullptr };
	ctx->VSSetConstantBuffers(12, 2, buffers);
	ctx->CSSetConstantBuffers(12, 2, buffers);

	ID3D11Buffer* grassBuffers[2] = { grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ctx->CSSetConstantBuffers(8, 2, grassBuffers);
	const auto generatorTypesCB = grassGeneratorTypesCB->CB();
	ctx->CSSetConstantBuffers(10, 1, &generatorTypesCB);
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

void ProceduralGrass::GenerateBlades(ID3D11DeviceContext* ctx) const
{
	globals::profiler->BeginPass("ProceduralGrass::Blade Generation");

	const float quad = 2048.0f;
	const float invBand = 1.0f / quad;

	const float highToMid = (HighTierQuadrantRadius - 1) * quad;  // High and Mid transition here
	const float midToLow = (MidTierQuadrantRadius - 1) * quad;    // Mid and Low transition here

	const float lowToFar = (LowTierQuadrantRadius - 1) * quad;
	const float gridEdge = LowTierQuadrantRadius * quad;
	const auto farGridCells = globals::game::tes ? globals::game::tes->gridCells : nullptr;
	const int32_t loadedGridLength = farGridCells ? farGridCells->length : 5;
	const int32_t loadedCellRadius = loadedGridLength / 2;
	const int32_t farExtraCells = std::clamp(settings.grassCellRadius, 0, std::max(0, PGrassCommon::FarCellRadiusCap - PGrassCommon::FarStreamGuardCells - loadedCellRadius));
	const float farStart = loadedGridLength * 2048.0f;
	const float radiusEdge = farStart + std::max(farExtraCells, 1) * 4096.0f;
	const float4 noFadeIn = float4(0.0f, 1.0e9f, 0.0f, 0.0f);
	const float farPatchDensity = static_cast<float>(FarPatchDensity());
	// Match Low's candidate count across the Low/Far cross-fade.
	const float farSeamExtraKeep = std::clamp(
		((settings.lowGrassDensity * settings.lowGrassDensity) / (farPatchDensity * farPatchDensity) - 1.0f) * 0.5f,
		0.0f, 1.0f);

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
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->CSSetShaderResources(0, 1, &nullSRV);
	if (auto heightMapSRV = globals::terrainHeightMap->GetSRV())
		ctx->CSSetShaderResources(0, 1, &heightMapSRV);
	ID3D11ShaderResourceView* densitySRV = grassDensityTexture->srv.get();
	ctx->CSSetShaderResources(7, 1, &densitySRV);

	// Near bounds cover clumping and Low width. Far bounds cover wider billboard blades.
	grassRendererHighLOD->GenerateBlades(ctx, quadrantsHighLOD, quadrantsHighVersion, 61, 60, grassLodOrigin, noFadeIn,
		float4(highToMid, invBand, 0.0f, 0.0f), nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
	grassRendererMidLOD->GenerateBlades(ctx, quadrantsMidLOD, quadrantsMidVersion, 61, 60, grassLodOrigin, float4(highToMid, invBand, 0.0f, 0.0f),
		float4(midToLow, invBand, 0.0f, 0.0f), nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
	grassRendererLowLOD->GenerateBlades(ctx, quadrantsLowLOD, quadrantsLowVersion, 61, 60, grassLodOrigin, float4(midToLow, invBand, 0.0f, 0.0f),
		float4(gridEdge, invBand, 0.0f, 0.0f), nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
	const float farCompactStart = gridEdge + (radiusEdge - gridEdge) * 0.75f;
	grassRendererFarLOD->GenerateBlades(ctx, quadrantsFarLOD, quadrantsFarVersion, 61, 60, grassLodOrigin, float4(lowToFar, invBand, farSeamExtraKeep, 0.0f),
		float4(gridEdge, 1.0f / std::max(radiusEdge - gridEdge, 1.0f), settings.farDensityFalloff, 1.0f / PGrassCommon::FarUnloadFadeWidth),
		farQuadrantFrustumPadding, settings.debugDisableAllCulls, farCompactStart, FarPerformanceKeep);

	ID3D11UnorderedAccessView* uavs[3] = { nullptr, nullptr, nullptr };
	ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

	ID3D11ShaderResourceView* nullGeneratorSRVs[9]{};
	ctx->CSSetShaderResources(0, ARRAYSIZE(nullGeneratorSRVs), nullGeneratorSRVs);
	ID3D11ShaderResourceView* nullSkylightingSRV = nullptr;
	ctx->CSSetShaderResources(50, 1, &nullSkylightingSRV);
	ctx->CSSetShader(nullptr, nullptr, 0);

	globals::profiler->EndPass();
}

void ProceduralGrass::RenderDepth(ID3D11DeviceContext* ctx) const
{
	globals::profiler->BeginPass("ProceduralGrass::Depth");

	const auto& mainDepth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ctx->OMSetRenderTargets(0, nullptr, mainDepth.views[0]);

	ctx->RSSetState(noCullRS);
	ctx->OMSetDepthStencilState(depthWriteDS, 0);
	ctx->OMSetBlendState(depthOnlyBlend, nullptr, 0xFFFFFFFF);

	ID3D11Buffer* grassCB = grassGlobalsCB->CB();
	ctx->VSSetConstantBuffers(8, 1, &grassCB);
	ctx->PSSetConstantBuffers(8, 1, &grassCB);

	grassRendererHighLOD->RenderDepth(ctx, depthClipPS);
	// Screen-space lighting needs blade depth before the shadow and occlusion passes.
	grassRendererMidLOD->RenderDepth(ctx, nullptr);
	grassRendererLowLOD->RenderDepth(ctx, nullptr);

	ID3D11ShaderResourceView* nullBladeSRV = nullptr;
	ctx->VSSetShaderResources(0, 1, &nullBladeSRV);

	globals::profiler->EndPass();
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

	ReleaseAndNull(oldRS);
	ReleaseAndNull(oldDSS);
	ReleaseAndNull(oldBS);

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
		nullptr,
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

	ctx->OMSetRenderTargets(1, &mainTex.RTV, nullptr);
	ctx->OMSetBlendState(multiplyBlend, nullptr, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState(noDepthDSS, 0);
	ctx->RSSetState(useShadowScissor ? noCullScissorRS : noCullRS);
	if (useShadowScissor)
		ctx->RSSetScissorRects(1, &shadowRect);

	auto terrainHeightSRV = globals::terrainHeightMap->GetSRV();
	ID3D11ShaderResourceView* srvs[3] = { mainDepth.depthSRV, grassDensityTexture->srv.get(), terrainHeightSRV };
	ctx->PSSetShaderResources(0, 3, srvs);
	ctx->PSSetSamplers(0, 1, &linearClampSampler);

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
	ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
	ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
	ctx->PSSetShaderResources(0, 3, nullSRVs);

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

void ProceduralGrass::RenderGrass(ID3D11DeviceContext* ctx) const
{
	globals::profiler->BeginPass("ProceduralGrass::Deferred");

	ID3D11ShaderResourceView* densitySRV = grassDensityTexture->srv.get();
	ctx->PSSetShaderResources(71, 1, &densitySRV);
	ID3D11ShaderResourceView* detailSRV = grassMaterialDetailTexture->srv.get();
	ctx->PSSetShaderResources(75, 1, &detailSRV);
	ctx->PSSetSamplers(13, 1, &grassDetailSampler);

	// Only High uses terrain-contact blending; other append buffers render opaque.
	ctx->OMSetBlendState(terrainFadeBlend, nullptr, 0xFFFFFFFF);
	grassRendererHighLOD->RenderGrass(ctx);
	ctx->OMSetBlendState(defaultBlend, nullptr, 0xFFFFFFFF);

	grassRendererMidLOD->RenderGrass(ctx);
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
	globals::profiler->BeginPass("ProceduralGrass::Far Forward");
	ID3D11RasterizerState* oldRS = nullptr;
	ID3D11DepthStencilState* oldDSS = nullptr;
	ID3D11BlendState* oldBS = nullptr;
	UINT oldRef = 0;
	float oldBlendFactor[4]{};
	UINT oldSampleMask = 0;
	ctx->RSGetState(&oldRS);
	ctx->OMGetDepthStencilState(&oldDSS, &oldRef);
	ctx->OMGetBlendState(&oldBS, oldBlendFactor, &oldSampleMask);
	DeferredRenderPrep(ctx, renderer);

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ctx->OMSetRenderTargets(1, &main.RTV, mainDepth.views[0]);
	ctx->OMSetBlendState(defaultBlend, nullptr, 0xFFFFFFFF);
	// Opaque depth makes Far output independent of append order.
	ctx->OMSetDepthStencilState(depthWriteDS, 0);
	ID3D11ShaderResourceView* terrainHeightSRV = globals::terrainHeightMap->GetSRV();
	ctx->PSSetShaderResources(74, 1, &terrainHeightSRV);
	grassRendererFarLOD->RenderGrass(ctx);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->VSSetShaderResources(0, 1, &nullSRV);
	ID3D11ShaderResourceView* nullGrassSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
	ctx->PSSetShaderResources(71, 4, nullGrassSRVs);
	ctx->OMSetRenderTargets(0, nullptr, nullptr);
	ctx->RSSetState(oldRS);
	ctx->OMSetDepthStencilState(oldDSS, oldRef);
	ctx->OMSetBlendState(oldBS, oldBlendFactor, oldSampleMask);
	ReleaseAndNull(oldRS);
	ReleaseAndNull(oldDSS);
	ReleaseAndNull(oldBS);
	globals::profiler->EndPass();
}
