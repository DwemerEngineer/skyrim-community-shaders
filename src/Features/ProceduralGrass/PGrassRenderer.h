#pragma once

#include "PGrassCommon.h"

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
class PGrassRenderer
{
public:
	PGrassRenderer(uint32_t grassDensity, uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef);

	void SetDensity(uint32_t grassDensity);
	void SetThreadGroupSize(uint32_t tgSize);
	
	void ClearShaderCache();
	
	void GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<PGrassCommon::Quadrant>& quadrants, int32_t cellXOffset, int32_t cellYOffset);
	void RenderDepth(ID3D11DeviceContext* ctx);
	void RenderGrass(ID3D11DeviceContext* ctx);

private:
	const char* lodDefine;
	const char* vertCountDefine;
	uint32_t density;
	std::string densityString;
	uint32_t patchesPerQuadrant;
	uint32_t totalBladeCount;
	uint32_t threadGroupSize;
	std::string threadGroupSizeString;

	ID3D11ComputeShader* bladeGeneratorCS = nullptr;
	ID3D11ComputeShader* copyBladeCountCS = nullptr;
	ID3D11VertexShader* depthVS = nullptr;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;

	StructuredBuffer* bladesSB = nullptr;
	ConstantBuffer* quadrantsCB = nullptr;
	Buffer* argsBuffer = nullptr;
	Buffer* vertexIndicesBuffer = nullptr;

	void CreateArgsBuffer();
	
	ID3D11ComputeShader* GetCopyBladeCountCS();
	ID3D11ComputeShader* GetBladeGeneratorCS();
	ID3D11VertexShader* GetDepthVS();
	ID3D11VertexShader* GetVS();
	ID3D11PixelShader* GetPS();
	
	static std::string BuildDefineList(std::span<const std::pair<const char*, const char*>> defines);

	template <class ShaderT>
	static ShaderT* CompileShader(const wchar_t* path, std::vector<std::pair<const char*, const char*>>& defines, const char* programType);
};
