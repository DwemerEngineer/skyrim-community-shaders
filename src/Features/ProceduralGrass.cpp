#include "ProceduralGrass.h"

#include "GrassCollision.h"
#include "LightLimitFix.h"
#include "ProceduralGrass/TopDownOcclusion.h"
#include "ShaderCache.h"
#include "Skylighting.h"
#include "State.h"
#include "TerrainBlending.h"
#include "TerrainHeightMap.h"
#include "Utils/Serialize.h"
#include "Utils/game.h"

#include <optional>
#include <numbers>

using namespace PGrassCommon;

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
	ConsoleFunc_ToggleGrass();
}

void ProceduralGrass::ClearShaderCache()
{
	TopDownOcclusion::GetSingleton()->ClearShaderCache();
	grassRendererHighLOD->ClearShaderCache();
	grassRendererMidLOD->ClearShaderCache();
	grassRendererLowLOD->ClearShaderCache();
	grassRendererFarLOD->ClearShaderCache();

	if (densityAOVS) {
		densityAOVS->Release();
		densityAOVS = nullptr;
	}

	if (densityAOPS) {
		densityAOPS->Release();
		densityAOPS = nullptr;
	}

	if (depthClipPS) {
		depthClipPS->Release();
		depthClipPS = nullptr;
	}

	if (densityGatherCS) {
		densityGatherCS->Release();
		densityGatherCS = nullptr;
	}

	densityAOVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOVS.hlsl", {}, "vs_5_0"));
	densityAOPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOPS.hlsl", {}, "ps_5_0"));
	depthClipPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDepthPS.hlsl", {}, "ps_5_0"));
	densityGatherCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityGatherCS.hlsl", {}, "cs_5_0"));
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
	grassMapCache.reserve(LowTierQuadrantCap);

	TerrainHeightMap::GetSingleton()->Discover();
	TopDownOcclusion::GetSingleton()->SetupResources();
	// Snap the shared window to the density grid so terrain darkening stays stable at grass edges.
	TopDownOcclusion::GetSingleton()->SetSnapDim(grassDensityDim);

	grassGlobalsCB = new ConstantBuffer(ConstantBufferDesc<GrassGlobals>());
	grassTypesArrayCB = new ConstantBuffer(ConstantBufferDesc<GrassTypesArray>());
	grassGeneratorTypesCB = new ConstantBuffer(ConstantBufferDesc<GrassGeneratorTypesArray>());

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
	grassRendererHighLOD = new PGrassRenderer<PGrassCommon::HighTierQuadrantCap, 4>(QualityDensities[settings.Quality], threadGroupSize, vertexIndicesHighBuffer, "HIGH_LOD", "HIGH_VERTEX", nullptr, 1, 0, sizeof(PGrassCommon::BladeSkylit));
	grassRendererMidLOD = new PGrassRenderer<PGrassCommon::MidTierQuadrantCap, 2>(static_cast<uint32_t>(settings.midGrassDensity), threadGroupSize, vertexIndicesMidBuffer, "MID_LOD", "MID_VERTEX", nullptr, 1);
	// Low's md=3..5 overlap contains 96 quadrants.
	grassRendererLowLOD = new PGrassRenderer<PGrassCommon::LowTierQuadrantCap, 1>(static_cast<uint32_t>(settings.lowGrassDensity), threadGroupSize, vertexIndicesLowBuffer, "LOW_LOD", "LOW_VERTEX", nullptr, 5, 0, sizeof(PGrassCommon::Blade), 96);
	// Far starts at Low's seam density and thins outward. Reserve its slope extras only for the near seam.
	grassRendererFarLOD = new PGrassRenderer<PGrassCommon::FarQuadrantCount, 1>(FarPatchDensity(), threadGroupSize, vertexIndicesFarBuffer, "LOW_LOD", "FAR_VERTEX", "FAR_LOD", 2, 512, sizeof(PGrassCommon::BladeFar), FarBladeQuadrantCapacity());

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
		ov.value("MicroDetail", s.grassMicroDetail),
		ov.value("AmbientFlatten", s.grassAmbientFlatten),
		ov.value("Wrap", s.grassWrap),
		ov.value("Aniso", s.grassAniso));
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
		0.0f);

	return t;
}

void ProceduralGrass::PostDepthRendering()
{
	const auto ctx = globals::d3d::context;
	const auto renderer = globals::game::renderer;

	const auto player = RE::PlayerCharacter::GetSingleton();

	if (!settings.Enabled || !player || globals::state->isMapMenuOpen) {
		CopyDepthBuffer(ctx, renderer);
		return;
	}

	globals::profiler->BeginPass("ProceduralGrass::Post Depth");

	GetVisibleQuadrants();
	TopDownOcclusion::GetSingleton()->Render();

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

	// Merge grass depth after terrain blending so grass does not appear transparent over terrain.
	if (globals::features::terrainBlending.loaded)
		globals::features::terrainBlending.MergeSceneDepthIntoBlend();

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

	globals::profiler->EndPass();
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

	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC texDesc;
	mainTex.texture->GetDesc(&texDesc);

	const float2 renderSize = Util::ConvertToDynamic(float2((float)texDesc.Width, (float)texDesc.Height));
	SetViewport(ctx, renderSize);

	const auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
	const auto& row0 = viewProjMat.m[0];
	const auto& row1 = viewProjMat.m[1];

	auto grassGlobals = GrassGlobals{};
	grassGlobals.voronoiGridSize = static_cast<float>(settings.voronoiGridSize);
	grassGlobals.inverseVoronoiGridSize = 1.0f / grassGlobals.voronoiGridSize;
	grassGlobals.cameraViewRow0Sum = abs(row0[0]) + abs(row0[1]) + abs(row0[2]);
	grassGlobals.cameraViewRow1Sum = abs(row1[0]) + abs(row1[1]) + abs(row1[2]);
	// Convert viewport-space SV_Position to normalized coordinates before dynamic-resolution adjustment.
	grassGlobals.dynamicResolutionInverted = float2(1.0f / renderSize.x, 1.0f / renderSize.y);

	grassGlobals.windSpeed = settings.windSpeed;
	grassGlobals.windTimer = std::fmod(globals::state->timer, 1.0f);
	grassGlobals.windDir = windDirection;
	grassGlobals.windAngle = atan2(windDirection.y, windDirection.x);

	const auto topDown = TopDownOcclusion::GetSingleton();
	topDown->SetPaddingWorld(settings.occlusionPadding);  // Pre-pad the map for one generator centre tap.
	grassGlobals.occlusionHalfExtent = topDown->GetHalfExtent();
	grassGlobals.occlusionInvExtent = 1.0f / (topDown->GetHalfExtent() * 2.0f);
	const auto window = topDown->GetWindowCentre();
	// z is underside clearance. A large negative value disables object culling.
	grassGlobals.occlusionParams = float4(window.x, window.y, settings.debugIgnoreObjectOcclusion ? -1.0e9f : settings.occlusionClearance, settings.occlusionBias);

	grassGlobals.grassAOParams = float4((float)grassDensityDim, settings.grassAOStrength, settings.grassAODensity, settings.grassHeight);
	grassGlobals.grassLightParams = float4(settings.grassDensityAO, settings.grassCanopySkyOcclusion, settings.grassSunSelfShadow, settings.grassBaseAO);

	const auto farGridCells = globals::game::tes ? globals::game::tes->gridCells : nullptr;
	const float farStart = (farGridCells ? farGridCells->length : 5) * 2048.0f;  // Loaded-grid half extent
	const float farEnd = std::max(farStart + 4096.0f, settings.grassCellRadius * 4096.0f);
	grassGlobals.farParams = float4(farStart, 1.0f / (farEnd - farStart), settings.farDensityFalloff, 0.0f);

	const float shaderTimer = globals::state->timer;
	const float timerDelta = std::max(0.0f, shaderTimer - previousShaderTimer);
	previousShaderTimer = shaderTimer;
	grassGlobals.miscParams = float4(settings.grassMapEdgeNoise, settings.grassSlopeFacing, settings.grassViewThicken, timerDelta);
	grassGlobals.grassTerrainBlend = float4(settings.grassTerrainBlendStrength, settings.grassTerrainBlendHeight, settings.grassTerrainBlendNormal, settings.grassTerrainBlendRough);

	auto heightMap = TerrainHeightMap::GetSingleton();
	heightMap->LoadForCurrentWorldspace();

	const auto heightMapScale = heightMap->GetScale();
	grassGlobals.heightMapScale = float2(heightMapScale.x, heightMapScale.y);
	grassGlobals.heightMapOffset = heightMap->GetOffset();
	grassGlobals.heightMapZRange = heightMap->GetPosRange();
	grassGlobals.debugFlags = float2(settings.debugDisableAllCulls ? 1.0f : 0.0f, 0.0f);

	// Presence-map origin, inverse sample spacing, and dimension.
	grassGlobals.grassPresenceParams = float4(grassPresenceOrigin.x, grassPresenceOrigin.y, (float)(QuadrantGrassPitch - 1) / 2048.0f, (float)grassPresenceDim);

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
			maxFarWidth = std::max(maxFarWidth, baseWidth * 32.0f);
		}

		grassTypesArrayCB->Update(resolvedGrassTypes);
		grassGeneratorTypesCB->Update(resolvedGeneratorTypes);
		// View thickening scales with blade width.
		nearQuadrantFrustumPadding = settings.voronoiGridSize * settings.clumpDistanceFactor + maxHeight + maxNearWidth * (1.0f + settings.grassViewThicken);
		farQuadrantFrustumPadding = maxHeight + maxFarWidth;
		grassTypesDirty = false;
	}

	ID3D11Buffer* buffers[2] = { *globals::game::perFrame, nullptr };
	ctx->VSSetConstantBuffers(12, 2, buffers);
	ctx->CSSetConstantBuffers(12, 2, buffers);

	ID3D11Buffer* grassBuffers[2] = { grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ctx->CSSetConstantBuffers(8, 1, grassBuffers);
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
	if (grassCollision.loaded)
		grassCollision.BindGrassShaderResources(ctx);

	ctx->IASetInputLayout(nullptr);
	ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
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

	const float gridEdge = LowTierQuadrantRadius * quad;  // Low's outer edge in world units
	const float radiusEdge = std::max(gridEdge + quad, settings.grassCellRadius * 4096.0f);
	const float4 noFadeIn = float4(0.0f, 1.0e9f, 0.0f, 0.0f);

	// Low thins to Far's seam density so the tiers meet without a density step.
	const float lowToFar = settings.lowGrassDensity > 0 ? std::clamp((float)settings.farGrassDensity / (float)settings.lowGrassDensity, 0.0f, 1.0f) : 0.0f;

	// Near bounds cover clumping and Low width. Far bounds cover wider billboard blades.
	grassRendererHighLOD->GenerateBlades(ctx, quadrantsHighLOD, 61, 60, noFadeIn, float4(highToMid, invBand, 0.0f, 0.0f), grassContentGeneration, nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
	grassRendererMidLOD->GenerateBlades(ctx, quadrantsMidLOD, 61, 60, float4(highToMid, invBand, 0.0f, 0.0f), float4(midToLow, invBand, 0.0f, 0.0f), grassContentGeneration, nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
	// Thin Low continuously from Mid's transition to Far's seam density.
	grassRendererLowLOD->GenerateBlades(ctx, quadrantsLowLOD, 61, 60, float4(midToLow, invBand, 0.0f, 0.0f), float4(midToLow, 1.0f / (gridEdge - midToLow), lowToFar, 0.0f), grassContentGeneration, nearQuadrantFrustumPadding, settings.debugDisableAllCulls);
	grassRendererFarLOD->GenerateBlades(ctx, quadrantsFarLOD, 61, 60, noFadeIn, float4(gridEdge, 1.0f / std::max(radiusEdge - gridEdge, 1.0f), settings.farDensityFalloff, 0.0f), grassContentGeneration, farQuadrantFrustumPadding, settings.debugDisableAllCulls);

	ID3D11UnorderedAccessView* uavs[3] = { nullptr, nullptr, nullptr };
	ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

	// Gather density after generation so the heightmap SRV remains bound for blade generation.
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

	// Match base dithering in depth and colour so dissolved grass leaves no depth behind.
	ID3D11Buffer* grassCB = grassGlobalsCB->CB();
	ctx->VSSetConstantBuffers(8, 1, &grassCB);
	ctx->PSSetConstantBuffers(8, 1, &grassCB);
	ctx->PSSetShader(depthClipPS, nullptr, 0);

	grassRendererHighLOD->RenderDepth(ctx);
	grassRendererMidLOD->RenderDepth(ctx);
	// Low and Far skip the depth prepass and write depth in their early-Z colour pass.

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

	auto& terrainBlending = globals::features::terrainBlending;
	if (terrainBlending.loaded && terrainBlending.settings.Enabled) {
		// Low and Far write depth in their colour pass, after the first terrain-depth merge.
		terrainBlending.MergeSceneDepthIntoBlend();

		ID3D11RenderTargetView* rtvs[7] = {
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV,
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV,
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_DOWNSCALED].RTV,
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kINDIRECT].RTV,
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kINDIRECT_DOWNSCALED].RTV,
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT].RTV,
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS].RTV,
		};
		const auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		ctx->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, mainDepth.views[0]);
	}

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

	// Prevent the game from replacing the render targets already bound here.
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

	SetViewport(ctx, Util::ConvertToDynamic(float2((float)texDesc.Width, (float)texDesc.Height)));

	ctx->OMSetRenderTargets(7, rtvs, mainDepth.views[0]);

	auto& shadowMask = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];
	ctx->PSSetShaderResources(14, 1, &shadowMask.SRV);
	ctx->PSSetSamplers(14, 1, &shadowSampler);

	static auto& precipOcclusionTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
	ctx->PSSetShaderResources(70, 1, &precipOcclusionTexture.depthSRV);

	ID3D11ShaderResourceView* densitySRV = grassDensityTexture->srv.get();
	ctx->PSSetShaderResources(71, 1, &densitySRV);

	const auto state = globals::state;
	auto sharedDataCB = state->sharedDataCB->CB();
	auto featureDataCB = state->featureDataCB->CB();
	ctx->PSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->VSSetConstantBuffers(5, 1, &sharedDataCB);
	ctx->PSSetConstantBuffers(6, 1, &featureDataCB);

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
	// Force opaque writes after terrain passes change the blend state.
	ctx->OMSetBlendState(defaultBlend, nullptr, 0xFFFFFFFF);

	ID3D11Buffer* grassBuffers[2] = { grassGlobalsCB->CB(), grassTypesArrayCB->CB() };
	ctx->VSSetConstantBuffers(8, 2, grassBuffers);
	ctx->PSSetConstantBuffers(8, 2, grassBuffers);
	if (globals::features::grassCollision.loaded)
		globals::features::grassCollision.BindGrassShaderResources(ctx);

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
	SetViewport(ctx, Util::ConvertToDynamic(float2((float)texDesc.Width, (float)texDesc.Height)));

	ctx->OMSetRenderTargets(1, &mainTex.RTV, nullptr);
	ctx->OMSetBlendState(multiplyBlend, nullptr, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState(noDepthDSS, 0);
	ctx->RSSetState(noCullRS);

	auto terrainHeightSRV = TerrainHeightMap::GetSingleton()->GetSRV();
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

	ID3D11RenderTargetView* nullRTV = nullptr;
	ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
	ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
	ctx->PSSetShaderResources(0, 3, nullSRVs);

	globals::profiler->EndPass();
}

void ProceduralGrass::RenderGrass(ID3D11DeviceContext* ctx) const
{
	globals::profiler->BeginPass("ProceduralGrass::Deferred");

	grassRendererHighLOD->RenderGrass(ctx);
	grassRendererMidLOD->RenderGrass(ctx);

	// Low and Far write depth here because they skip the depth prepass. Their colour pass remains early-Z.
	ctx->OMSetDepthStencilState(depthWriteDS, 0);
	grassRendererLowLOD->RenderGrass(ctx);
	grassRendererFarLOD->RenderGrass(ctx);

	// Unbind the density map so generation can use it as a UAV next frame.
	ID3D11ShaderResourceView* nullDensitySRV = nullptr;
	ctx->PSSetShaderResources(71, 1, &nullDensitySRV);

	globals::profiler->EndPass();
}
