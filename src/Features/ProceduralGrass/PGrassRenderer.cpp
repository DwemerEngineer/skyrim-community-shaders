#include "PGrassRenderer.h"

#include "TopDownOcclusion.h"

#include "ShaderCache.h"

using namespace PGrassCommon;

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
PGrassRenderer<QuadrantCount, PatchBladeCount>::PGrassRenderer(const uint32_t grassDensity, const uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef)
{
	vertexIndicesBuffer = vertexIndicesBuf;
	lodDefine = lodDef;
	vertCountDefine = vertCountDef;
	extraDefine = extraDef;
	
	CreateArgsBuffer();
	SetDensity(grassDensity);
	SetThreadGroupSize(tgSize);

	GetBladeGeneratorCS();
	GetCopyBladeCountCS();
	GetDepthVS();
	GetVS();
	GetPS();
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::CreateArgsBuffer()
{
	quadrantsCB = new ConstantBuffer(ConstantBufferDesc<QuadrantDataArray<QuadrantCount>>());

	constexpr uint32_t grassSampleCount = QuadrantCount * QuadrantGrassSamples;
	quadrantGrassSB = new StructuredBuffer(StructuredBufferDesc<uint32_t>(grassSampleCount, true), grassSampleCount, "PGrass::QuadrantGrass");
	quadrantGrassSB->CreateSRV();
	quadrantGrassStaging.assign(grassSampleCount, 0u);


	quadrantHeightSB = new StructuredBuffer(StructuredBufferDesc<float>(grassSampleCount, true), grassSampleCount, "PGrass::QuadrantHeights");
	quadrantHeightSB->CreateSRV();
	quadrantHeightStaging.assign(grassSampleCount, QuadrantNoHeight);

	D3D11_BUFFER_DESC argsBufferDesc{};
	argsBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	argsBufferDesc.CPUAccessFlags = 0;
	argsBufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	argsBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
	argsBufferDesc.ByteWidth = 5 * sizeof(uint32_t);
	argsBuffer = new Buffer(argsBufferDesc);

	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.ByteWidth = 5 * sizeof(uint32_t);
	globals::d3d::device->CreateBuffer(&stagingDesc, nullptr, argsStaging.put());

	D3D11_UNORDERED_ACCESS_VIEW_DESC argsBufferUavDesc;
	argsBufferUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	argsBufferUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	argsBufferUavDesc.Buffer.FirstElement = 0;
	argsBufferUavDesc.Buffer.NumElements = 5;
	argsBufferUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	argsBuffer->CreateUAV(argsBufferUavDesc);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::SetDensity(uint32_t grassDensity)
{
	density = grassDensity;
	patchesPerQuadrant = grassDensity * grassDensity / 4;
	totalBladeCount = patchesPerQuadrant * PatchBladeCount * QuadrantCount;
	densityString = std::to_string(grassDensity);

	bladesSB = new StructuredBuffer(StructuredBufferDesc<Blade>(totalBladeCount, false), totalBladeCount);
	bladesSB->CreateUAV(true);
	bladesSB->CreateSRV();
	
	if (bladeGeneratorCS) {
		bladeGeneratorCS->Release();
		bladeGeneratorCS = nullptr;
	}
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
	if (copyBladeCountCS) {
		copyBladeCountCS->Release();
		copyBladeCountCS = nullptr;
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
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<Quadrant>& quadrants, const int32_t cellXOffset, const int32_t cellYOffset, const float4& lodFadeIn, const float4& lodFadeOut)
{
	auto quadrantDataArray = QuadrantDataArray<QuadrantCount>{};
	quadrantDataArray.lodFadeIn = lodFadeIn;
	quadrantDataArray.lodFadeOut = lodFadeOut;

	for (uint32_t i = 0; i < quadrants.size(); i++) {
		const auto& [cellX, cellY, quadrantX, quadrantY, grassIds, heights] = quadrants[i];
		const float worldX = (cellX + quadrantX / 2.0f) * 4096.0f;
		const float worldY = (cellY + quadrantY / 2.0f) * 4096.0f;

		auto& quadrantData = quadrantDataArray.data[i];
		quadrantData.quadWorldPos = float2{ worldX, worldY };
		quadrantData.quadX = (cellX + cellXOffset) * 32 + quadrantX * 16;
		quadrantData.quadY = (cellY + cellYOffset) * 32 + quadrantY * 16;

		auto* dst = quadrantGrassStaging.data() + i * QuadrantGrassSamples;
		if (grassIds)
			std::copy_n(grassIds, QuadrantGrassSamples, dst);
		else
			std::fill_n(dst, QuadrantGrassSamples, 0u);

		auto* heightDst = quadrantHeightStaging.data() + i * QuadrantGrassSamples;
		if (heights)
			std::copy_n(heights, QuadrantGrassSamples, heightDst);
		else
			std::fill_n(heightDst, QuadrantGrassSamples, QuadrantNoHeight);
	}

	quadrantsCB->Update(quadrantDataArray);
	quadrantGrassSB->Update(quadrantGrassStaging.data(), quadrantGrassStaging.size() * sizeof(uint32_t));
	quadrantHeightSB->Update(quadrantHeightStaging.data(), quadrantHeightStaging.size() * sizeof(float));
	const auto quadrantsBuffer = quadrantsCB->CB();
	ctx->CSSetConstantBuffers(7, 1, &quadrantsBuffer);

	constexpr uint32_t initialCount = 0;
	ID3D11UnorderedAccessView* uavs[] = { bladesSB->UAV(), argsBuffer->uav.get() };
	ctx->CSSetUnorderedAccessViews(0, 2, uavs, &initialCount);

	ID3D11ShaderResourceView* mapSRVs[3] = { quadrantGrassSB->SRV(), TopDownOcclusion::GetSingleton()->GetSRV(), quadrantHeightSB->SRV() };
	ctx->CSSetShaderResources(1, 3, mapSRVs);

	ctx->CSSetShader(GetBladeGeneratorCS(), nullptr, 0);
	// Pad the group count with groupSize - 1, so it doesn't get truncated. The shader discards any excess threads.
	const uint32_t gx = (patchesPerQuadrant + threadGroupSize - 1) / threadGroupSize;
	ctx->Dispatch(gx, PatchBladeCount, static_cast<uint32_t>(quadrants.size()));

	ctx->CopyStructureCount(argsBuffer->resource.get(), 4, bladesSB->UAV());
	if (argsStaging)
		ctx->CopyResource(argsStaging.get(), argsBuffer->resource.get());
	ctx->CSSetShader(GetCopyBladeCountCS(), nullptr, 0);
	ctx->Dispatch(1, 1, 1);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
uint32_t PGrassRenderer<QuadrantCount, PatchBladeCount>::ReadBladeCount() const
{
	if (!argsStaging)
		return 0;

	auto ctx = globals::d3d::context;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(ctx->Map(argsStaging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
		return 0;

	// DrawIndexedInstancedIndirect args are [IndexCount, InstanceCount, StartIndex, BaseVertex, StartInstance]
	const uint32_t instanceCount = static_cast<const uint32_t*>(mapped.pData)[1];
	ctx->Unmap(argsStaging.get(), 0);
	return instanceCount;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderDepth(ID3D11DeviceContext* ctx)
{
	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);

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

	const auto bladesSRV = bladesSB->SRV();
	ctx->VSSetShaderResources(0, 1, &bladesSRV);

	ctx->VSSetShader(GetVS(), nullptr, 0);
	ctx->PSSetShader(GetPS(), nullptr, 0);

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
		if (extraDefine)
			defines.push_back({extraDefine, nullptr});
		
		bladeGeneratorCS = CompileShader<ID3D11ComputeShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassBladeGeneratorCS.hlsl", defines, "cs_5_0");
	}
	return bladeGeneratorCS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11ComputeShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetCopyBladeCountCS()
{
	if (!copyBladeCountCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		defines.push_back({ vertCountDefine, nullptr });
		copyBladeCountCS = CompileShader<ID3D11ComputeShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassCopyBladeCountCS.hlsl", defines, "cs_5_0");
	}
	return copyBladeCountCS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetDepthVS()
{
	if (!depthVS) {
		std::vector<std::pair<const char*, const char*>> defines;
		defines.push_back({ vertCountDefine, nullptr });
		defines.push_back({ "DEPTH", nullptr });
		depthVS = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}
	return depthVS;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11VertexShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetVS()
{
	if (!vs) {
		std::vector<std::pair<const char*, const char*>> defines;
		defines.push_back({ vertCountDefine, nullptr });
		vs = CompileShader<ID3D11VertexShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassVS.hlsl", defines, "vs_5_0");
	}
	return vs;
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
ID3D11PixelShader* PGrassRenderer<QuadrantCount, PatchBladeCount>::GetPS()
{
	if (!ps) {
		std::vector<std::pair<const char*, const char*>> defines;
		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting))
				defines.push_back({ feature->GetShaderDefineName().data(), nullptr });
		}
		defines.push_back({ lodDefine, nullptr });
		defines.push_back({ vertCountDefine, nullptr });
		ps = CompileShader<ID3D11PixelShader>(L"Data\\Shaders\\ProceduralGrass\\PGrassPS.hlsl", defines, "ps_5_0");
	}
	return ps;
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

template <uint32_t QuadrantCount, uint32_t PatchPatchBladeCount>
std::string PGrassRenderer<QuadrantCount, PatchPatchBladeCount>::BuildDefineList(std::span<const std::pair<const char*, const char*>> defines)
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

template class PGrassRenderer<25, 4>;
template class PGrassRenderer<49, 2>;
template class PGrassRenderer<121, 1>;
template class PGrassRenderer<FarQuadrantCount, 1>;  // far tier for cells beyond the loaded grid