#pragma once

#include "PGrassCommon.h"

namespace PGrassRendererQuads
{
	// Packed visible-work layout consumed by PGrassBladeGeneratorCS.
	inline constexpr uint32_t WorkQuadrantMask = 0xFFFu;  // Far's fixed cbuffer capacity is 4,000.
	inline constexpr uint32_t WorkLaneShift = 12;
	inline constexpr uint32_t WorkHasLand = 1u << 16;
	inline constexpr uint32_t WorkInsideFrustum = 1u << 17;
	inline constexpr uint32_t WorkAllowSlopeExtras = 1u << 18;

	/** Stable quadrant identity hash, generated once on the CPU for all patches in that quadrant. */
	uint32_t QuadrantHash(uint32_t x, uint32_t y);

	enum class QuadrantFrustumState : uint8_t
	{
		Outside,
		Intersecting,
		Inside,
	};

	/** Conservative XY clip test for a padded quadrant LAND AABB. Near/far clipping intentionally matches the generator and stays disabled. */
	QuadrantFrustumState ClassifyQuadrantFrustum(const PGrassCommon::Quadrant& quadrant, const float4x4& viewProj, const float4& cameraPosAdjust, float xyPadding, bool& hasLand);
}

template <uint32_t QuadrantCount, uint32_t PatchBladeCount>
class PGrassRenderer
{
public:
	/**
	 * @brief Creates the renderer and sizes its blade buffers for the requested LOD and optional slope-fill capacity.
	 *
	 * @param slopeExtraBlades Extra candidate blade slots per patch for filling sloped ground. Keep this small because it enlarges the
	 *						   blade buffer and the base thread's candidate loop.
	 * @param slopeExtraQuadrants Number of quadrants for which the buffer reserves slope extras; 0 means every
	 *                            quadrant. Far uses only its near-seam ring, so it reserves fewer than all 4,000.
	 */
	PGrassRenderer(uint32_t grassDensity, uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef = nullptr, 
		uint32_t slopeExtraBlades = 0, uint32_t slopeExtraQuadrants = 0, uint32_t bladeStrideBytes = sizeof(PGrassCommon::Blade), uint32_t bladeQuadrantCapacity = QuadrantCount);

	void SetDensity(uint32_t grassDensity);
	/** @brief Adjusts the output-buffer budget without changing the cbuffer's fixed quadrant capacity. Far tracks its radius this way. */
	void SetBladeQuadrantCapacity(uint32_t quadrantCapacity);
	void SetThreadGroupSize(uint32_t tgSize);
	
	void ClearShaderCache();
	
	void GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<PGrassCommon::Quadrant>& quadrants, int32_t cellXOffset, int32_t cellYOffset, const float4& lodFadeIn, 
		const float4& lodFadeOut, uint64_t contentGeneration, float frustumPadding, bool disableGeneratorCulls);
	void RenderDepth(ID3D11DeviceContext* ctx);
	void RenderGrass(ID3D11DeviceContext* ctx);

	/** @brief Reads back the instance count the generator appended last frame. Debug only; stalls. */
	uint32_t ReadBladeCount() const;

private:
	const char* lodDefine;
	const char* vertCountDefine;
	const char* extraDefine;
	uint32_t density;
	uint32_t bladeStrideBytes = sizeof(PGrassCommon::Blade);  // High stores two extra f16x2 skylight words; Mid/Low retain two wind poses and Far is 20 bytes
	uint32_t bladeQuadrantCapacity = QuadrantCount;
	std::string densityString;
	uint32_t slopeExtraBlades = 0;
	uint32_t slopeExtraQuadrants = QuadrantCount;
	std::string patchBladeCountString = std::to_string(PatchBladeCount);
	std::string slopeExtraBladesString = "0";
	uint32_t patchesPerQuadrant;
	uint32_t totalBladeCount;
	uint32_t threadGroupSize;
	std::string threadGroupSizeString;
	std::string quadrantCountString = std::to_string(QuadrantCount);

	ID3D11ComputeShader* bladeGeneratorCS = nullptr;
	ID3D11VertexShader* depthVS = nullptr;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	ID3D11PixelShader* noWetnessPS = nullptr;
	ID3D11PixelShader* noLocalLightsPS = nullptr;
	ID3D11PixelShader* noWetnessNoLocalLightsPS = nullptr;

	StructuredBuffer* bladesSB = nullptr;
	StructuredBuffer* quadrantGrassSB = nullptr;
	std::vector<uint8_t> quadrantGrassStaging;  // one grass id per byte; the GPU buffer packs 4 per uint
	StructuredBuffer* quadrantGrassCellsSB = nullptr;
	std::vector<uint32_t> quadrantGrassCellsStaging;  // one packed 2x2 LAND-id cell per 16x16 quadrant cell
	StructuredBuffer* quadrantHeightSB = nullptr;
	std::vector<float> quadrantHeightStaging;
	StructuredBuffer* visibleWorkSB = nullptr;
	std::vector<uint32_t> visibleWorkStaging;
	ConstantBuffer* quadrantsCB = nullptr;

	// Skip the staging rebuild + uploads on frames where the quadrant data is unchanged
	size_t lastUploadHash = 0;
	bool hasUploadedQuadrants = false;
	Buffer* argsBuffer = nullptr;
	winrt::com_ptr<ID3D11Buffer> argsStaging;
	Buffer* vertexIndicesBuffer = nullptr;

	void CreateArgsBuffer();
	bool UsesGrassCollision(bool grassCollisionLoaded) const
	{
		const auto lod = std::string_view(lodDefine);
		return grassCollisionLoaded && (lod == "HIGH_LOD" || lod == "MID_LOD");
	}
	
	ID3D11ComputeShader* GetBladeGeneratorCS();
	ID3D11VertexShader* GetDepthVS();
	ID3D11VertexShader* GetVS();
	ID3D11PixelShader* GetPS(bool noWetness = false, bool noLocalLights = false);
	
	static std::string BuildDefineList(std::span<const std::pair<const char*, const char*>> defines);

	template <class ShaderT>
	static ShaderT* CompileShader(const wchar_t* path, std::vector<std::pair<const char*, const char*>>& defines, const char* programType);
};
