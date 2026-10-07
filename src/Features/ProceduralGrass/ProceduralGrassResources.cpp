#include "Features/ProceduralGrass.h"

#include "Features/GrassCollision.h"
#include "Features/ProceduralGrass/TopDownOcclusion.h"
#include "TerrainHeightMap.h"
#include "Utils/D3D.h"

using namespace PGrassCommon;

void ProceduralGrass::CreateIndexBuffers()
{
	const auto makeIndexBuffer = [](const std::vector<uint16_t>& indices, const char* name) {
		D3D11_BUFFER_DESC desc{};
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
		D3D11_SUBRESOURCE_DATA init{ indices.data(), 0, 0 };
		return new Buffer(desc, &init, name);
	};

	const auto makeBatchedIndexBuffer = [&](std::span<const uint16_t> indices, uint32_t verticesPerBlade, uint32_t bladeCount, const char* name) {
		std::vector<uint16_t> batchedIndices;
		batchedIndices.reserve(indices.size() * bladeCount);
		for (uint32_t blade = 0; blade < bladeCount; ++blade) {
			for (const auto index : indices)
				batchedIndices.push_back(static_cast<uint16_t>(blade * verticesPerBlade + index));
		}
		return makeIndexBuffer(batchedIndices, name);
	};

	auto vertexIndicesHigh = CreateVertexIndicesArray(15);
	vertexIndicesHighBuffer = makeIndexBuffer(vertexIndicesHigh, "PGrass::HighIndices");
	// Outer High is drawn twice, in depth and colour, and both passes are vertex-bound.
	auto vertexIndicesHighOuter = CreateVertexIndicesArray(5);
	vertexIndicesHighOuterBuffer = makeIndexBuffer(vertexIndicesHighOuter, "PGrass::HighOuterIndices");

	const std::array<uint16_t, 6> lowBladeIndices = { 0, 1, 2, 2, 1, 3 };
	vertexIndicesLowBuffer = makeBatchedIndexBuffer(lowBladeIndices, 4u, LowBladeBatchSize, "PGrass::LowIndices");
	const std::array<uint16_t, 3> outerBladeIndices = { 0, 1, 2 };
	vertexIndicesLowOuterBuffer = makeBatchedIndexBuffer(outerBladeIndices, 3u, LowBladeBatchSize, "PGrass::LowOuterIndices");

	// Mid keeps one curve midpoint; Low uses a distant two-triangle ribbon.
	const auto midBladeIndices = CreateVertexIndicesArray(5);
	vertexIndicesMidBuffer = makeBatchedIndexBuffer(midBladeIndices, 5u, MidBladeBatchSize, "PGrass::MidIndices");

	// Fully straightened Mid blades need only one triangle in both depth and colour passes.
	vertexIndicesMidOuterBuffer = makeBatchedIndexBuffer(outerBladeIndices, 3u, MidBladeBatchSize, "PGrass::MidOuterIndices");

	// Far uses one tapered triangle because finer geometry is not visible at this distance.
	const auto farBladeIndices = CreateVertexIndicesArray(3);
	vertexIndicesFarBuffer = makeBatchedIndexBuffer(farBladeIndices, 3u, FarBladeBatchSize, "PGrass::FarIndices");

	// Far's handoff fill draws double blades, two crossed triangles per record, from a second list.
	const std::array<uint16_t, 6> farDoubleBladeIndices = { 0, 1, 2, 3, 4, 5 };
	vertexIndicesFarDoubleBuffer = makeBatchedIndexBuffer(farDoubleBladeIndices, 6u, FarBladeBatchSize, "PGrass::FarDoubleIndices");
}

void ProceduralGrass::CreatePipelineStates()
{
	auto device = globals::d3d::device;

	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	device->CreateSamplerState(&samplerDesc, &linearClampSampler);

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
		bd.IndependentBlendEnable = TRUE;
		for (auto& target : bd.RenderTarget)
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		// Grass clears terrain classification while inheriting the ground's vertex AO.
		bd.RenderTarget[7].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_GREEN;
		device->CreateBlendState(&bd, &defaultBlend);
	}

	if (!terrainFadeBlend) {
		D3D11_BLEND_DESC bd = {};
		bd.IndependentBlendEnable = TRUE;
		for (uint32_t i = 0; i < 8; i++) {
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
		bd.RenderTarget[7].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_GREEN;
		device->CreateBlendState(&bd, &terrainFadeBlend);
	}

	if (!noDepthDSS) {
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		device->CreateDepthStencilState(&dd, &noDepthDSS);
	}
}

void ProceduralGrass::CreateGrassTextures()
{
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

	if (!grassDensityTexture)
		grassDensityTexture = Util::CreateSquareTexture(grassDensityDim, DXGI_FORMAT_R32_UINT, true, "PGrass::GrassDensity");
	if (!distantAmbientLUT)
		distantAmbientLUT = Util::CreateSquareTexture(distantAmbientLUTDim, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "PGrass::DistantAmbientLUT");
	if (!terrainLiftTexture) {
		// R32_FLOAT is the float format D3D11 guarantees for typed UAV reads.
		terrainLiftMeasuredTexture = Util::CreateSquareTexture(PGrassCommon::TerrainLiftDim, DXGI_FORMAT_R32_FLOAT, true, "PGrass::TerrainLiftMeasured");
		terrainLiftTexture = Util::CreateSquareTexture(PGrassCommon::TerrainLiftDim, DXGI_FORMAT_R32_FLOAT, true, "PGrass::TerrainLift");
		terrainLiftSurfaceTexture = Util::CreateSquareTexture(PGrassCommon::TerrainLiftDim, DXGI_FORMAT_R32_FLOAT, true, "PGrass::TerrainLiftSurface");
		terrainLiftOriginValid = false;
	}
	if (!grassPresenceTexture) {
		// Rewritten via UpdateSubresource as the window scrolls with the player.
		grassPresenceTexture = Util::CreateSquareTexture(grassPresenceDim, DXGI_FORMAT_R8_UINT, false, "PGrass::GrassPresence");
		grassPresenceStaging.assign(static_cast<size_t>(grassPresenceDim) * grassPresenceDim, 0);
	}
}

void ProceduralGrass::SetupResources()
{
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

	grassGlobalsCB = new ConstantBuffer(ConstantBufferDesc<GrassGlobals>(), "PGrass::GlobalsCB");
	grassTypesArrayCB = new ConstantBuffer(ConstantBufferDesc<GrassTypesArray>(), "PGrass::TypesCB");
	grassGeneratorTypesCB = new ConstantBuffer(ConstantBufferDesc<GrassGeneratorTypesArray>(), "PGrass::GeneratorTypesCB");

	CreateIndexBuffers();

	constexpr uint32_t threadGroupSize = 64;
	// Add steepness-gated slope-fill candidates per patch. Low needs the most to fill sparse steep ground.
	const bool cacheCollision = globals::features::grassCollision.loaded;
	const uint32_t highBladeStride = cacheCollision ? sizeof(PGrassCommon::BladeSkylitCollision) : sizeof(PGrassCommon::BladeSkylit);
	const uint32_t midBladeStride = cacheCollision ? sizeof(PGrassCommon::BladeMidCollision) : sizeof(PGrassCommon::BladeMid);
	grassRendererHighLOD = new PGrassRenderer<PGrassCommon::HighTierQuadrantCap, 4>(QualityDensities[settings.Quality], threadGroupSize, vertexIndicesHighBuffer,
		"HIGH_LOD", "HIGH_VERTEX", nullptr, 1, highBladeStride, vertexIndicesHighOuterBuffer);
	grassRendererMidLOD = new PGrassRenderer<PGrassCommon::MidTierQuadrantCap, PGrassCommon::MidPatchBladeCount>(static_cast<uint32_t>(settings.midGrassDensity), threadGroupSize, vertexIndicesMidBuffer, "MID_LOD", "MID_VERTEX", nullptr, 1, midBladeStride, vertexIndicesMidOuterBuffer);
	grassRendererLowLOD = new PGrassRenderer<PGrassCommon::LowTierQuadrantCap, 1>(static_cast<uint32_t>(settings.lowGrassDensity), threadGroupSize, vertexIndicesLowBuffer, "LOW_LOD", "LOW_VERTEX", nullptr, 1, sizeof(PGrassCommon::Blade), vertexIndicesLowOuterBuffer);
	grassRendererFarLOD = new PGrassRenderer<PGrassCommon::FarQuadrantCount, 1>(FarPatchDensity(), threadGroupSize, vertexIndicesFarBuffer, "LOW_LOD", "FAR_VERTEX", "FAR_LOD", 1, sizeof(PGrassCommon::BladeFar), vertexIndicesFarDoubleBuffer);

	CreatePipelineStates();
	CreateGrassTextures();
	CreateTerrainCanopyResources();

	CompileSupportShaders();
}

void ProceduralGrass::CompileSupportShaders()
{
	if (!densityGatherCS)
		densityGatherCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityGatherCS.hlsl", {}, "cs_5_0"));
	if (!distantAmbientLUTCS)
		distantAmbientLUTCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassAmbientLUTCS.hlsl", {}, "cs_5_0"));
	if (!terrainLiftCS)
		terrainLiftCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassTerrainLiftCS.hlsl", {}, "cs_5_0"));
	if (!terrainCanopyCS && terrainCanopyTypedLoadSupported) {
		std::vector<std::pair<const char*, const char*>> defines;
		for (auto* feature : Feature::GetFeatureList()) {
			const auto name = feature->GetShaderDefineName();
			if (feature->loaded && (name == "LINEAR_LIGHTING" || name == "TERRAIN_SHADOWS" || name == "CLOUD_SHADOWS" || name == "SCREEN_SPACE_SHADOWS"))
				defines.push_back({ name.data(), nullptr });
		}
		terrainCanopyCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassTerrainCanopyCS.hlsl", defines, "cs_5_0"));
	}
	if (!densityAOVS)
		densityAOVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOVS.hlsl", {}, "vs_5_0"));
	if (!densityAOPS) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (!terrainCanopyTypedLoadSupported)
			defines.push_back({ "PGRASS_DARKENING_COPY", nullptr });
		densityAOPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\ProceduralGrass\\PGrassDensityAOPS.hlsl", defines, "ps_5_0"));
	}
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
