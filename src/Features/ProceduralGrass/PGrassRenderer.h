#pragma once

#include "PGrassCommon.h"

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
class PGrassRenderer
{
public:
	PGrassRenderer(uint32_t grassDensity, uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef = nullptr);

	void SetDensity(uint32_t grassDensity);
	void SetThreadGroupSize(uint32_t tgSize);
	
	void ClearShaderCache();
	
	void GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<PGrassCommon::Quadrant>& quadrants, int32_t cellXOffset, int32_t cellYOffset, const float4& lodFadeIn, const float4& lodFadeOut);
	void RenderDepth(ID3D11DeviceContext* ctx);
	void RenderGrass(ID3D11DeviceContext* ctx);

	/** @brief Reads back the instance count the generator appended last frame. Debug only; stalls. */
	uint32_t ReadBladeCount() const;

private:
	const char* lodDefine;
	const char* vertCountDefine;
	const char* extraDefine;
	uint32_t density;
	std::string densityString;
	uint32_t patchesPerQuadrant;
	uint32_t totalBladeCount;
	uint32_t threadGroupSize;
	std::string threadGroupSizeString;
	std::string quadrantCountString = std::to_string(QuadrantCount);

	ID3D11ComputeShader* bladeGeneratorCS = nullptr;
	ID3D11ComputeShader* copyBladeCountCS = nullptr;
	ID3D11VertexShader* depthVS = nullptr;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;

	StructuredBuffer* bladesSB = nullptr;
	StructuredBuffer* quadrantGrassSB = nullptr;
	std::vector<uint32_t> quadrantGrassStaging;
	StructuredBuffer* quadrantHeightSB = nullptr;
	std::vector<float> quadrantHeightStaging;
	ConstantBuffer* quadrantsCB = nullptr;
	Buffer* argsBuffer = nullptr;
	winrt::com_ptr<ID3D11Buffer> argsStaging;
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
