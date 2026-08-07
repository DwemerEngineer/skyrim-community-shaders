#include "PGrassRenderer.h"

#include "ShaderCache.h"

using namespace PGrassCommon;

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
PGrassRenderer<QuadrantCount, PatchBladeCount>::PGrassRenderer(const uint32_t grassDensity, const uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef)
{
	vertexIndicesBuffer = vertexIndicesBuf;
	lodDefine = lodDef;
	vertCountDefine = vertCountDef;
	
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

	D3D11_BUFFER_DESC argsBufferDesc{};
	argsBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	argsBufferDesc.CPUAccessFlags = 0;
	argsBufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	argsBufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
	argsBufferDesc.ByteWidth = 5 * sizeof(uint32_t);
	argsBuffer = new Buffer(argsBufferDesc);

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
	totalBladeCount = grassDensity * grassDensity * QuadrantCount;
	patchesPerQuadrant = grassDensity * grassDensity / 4;
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
void PGrassRenderer<QuadrantCount, PatchBladeCount>::GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<Quadrant>& quadrants, const int32_t cellXOffset, const int32_t cellYOffset)
{
	auto quadrantDataArray = QuadrantDataArray<QuadrantCount>{};

	for (uint32_t i = 0; i < quadrants.size(); i++) {
		const auto& [cellX, cellY, quadrantX, quadrantY] = quadrants[i];
		const float worldX = (cellX + quadrantX / 2.0f) * 4096.0f;
		const float worldY = (cellY + quadrantY / 2.0f) * 4096.0f;

		auto& quadrantData = quadrantDataArray.data[i];
		quadrantData.quadWorldPos = float2{ worldX, worldY };
		quadrantData.quadX = (cellX + cellXOffset) * 32 + quadrantX * 16;
		quadrantData.quadY = (cellY + cellYOffset) * 32 + quadrantY * 16;
	}

	quadrantsCB->Update(quadrantDataArray);
	const auto quadrantsBuffer = quadrantsCB->CB();
	ctx->CSSetConstantBuffers(7, 1, &quadrantsBuffer);

	constexpr uint32_t initialCount = 0;
	ID3D11UnorderedAccessView* uavs[] = { bladesSB->UAV(), argsBuffer->uav.get() };
	ctx->CSSetUnorderedAccessViews(0, 2, uavs, &initialCount);

	ctx->CSSetShader(GetBladeGeneratorCS(), nullptr, 0);
	const uint32_t gx = patchesPerQuadrant / threadGroupSize;
	ctx->Dispatch(gx, PatchBladeCount, static_cast<uint32_t>(quadrants.size()));

	ctx->CopyStructureCount(argsBuffer->resource.get(), 4, bladesSB->UAV());
	ctx->CSSetShader(GetCopyBladeCountCS(), nullptr, 0);
	ctx->Dispatch(1, 1, 1);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
void PGrassRenderer<QuadrantCount, PatchBladeCount>::RenderDepth(ID3D11DeviceContext* ctx)
{
	ctx->IASetIndexBuffer(vertexIndicesBuffer->resource.get(), DXGI_FORMAT_R16_UINT, 0);

	const auto bladesSRV = bladesSB->SRV();
	ctx->VSSetShaderResources(0, 1, &bladesSRV);

	ctx->VSSetShader(GetDepthVS(), nullptr, 0);
	ctx->PSSetShader(nullptr, nullptr, 0);

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

template class PGrassRenderer<9, 4>;
template class PGrassRenderer<16, 2>;
template class PGrassRenderer<75, 1>;