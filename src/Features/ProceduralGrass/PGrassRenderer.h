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
	inline constexpr uint32_t WorkNearCovered = 1u << 19;
	inline constexpr uint32_t WorkCompactFar = 1u << 20;

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
	 * @brief Creates a tier renderer with a lazily sized append buffer.
	 *
	 * @param slopeExtraBlades Extra candidate blade slots per patch for filling sloped ground. Keep this small because it enlarges the
	 *						   blade buffer and the base thread's candidate loop.
	 */
	PGrassRenderer(uint32_t grassDensity, uint32_t tgSize, Buffer* vertexIndicesBuf, const char* lodDef, const char* vertCountDef, const char* extraDef = nullptr,
		uint32_t slopeExtraBlades = 0, uint32_t bladeStrideBytes = sizeof(PGrassCommon::Blade));

	void SetDensity(uint32_t grassDensity);
	/** Discards the retained append-buffer capacity. */
	void ResetBladeCapacity();
	void SetThreadGroupSize(uint32_t tgSize);

	void ClearShaderCache();

	void GenerateBlades(ID3D11DeviceContext* ctx, const std::vector<PGrassCommon::Quadrant>& quadrants, int32_t cellXOffset, int32_t cellYOffset, const float2& lodOrigin, const float4& lodFadeIn,
		const float4& lodFadeOut, float frustumPadding, bool disableGeneratorCulls, float compactStartDistance = -1.0f, float compactKeep = 1.0f);
	void RenderDepth(ID3D11DeviceContext* ctx, ID3D11PixelShader* depthClipPS);
	void RenderGrass(ID3D11DeviceContext* ctx);

	/** @brief Reads back the instance count the generator appended last frame. Debug only; stalls. */
	uint32_t ReadBladeCount() const;

private:
	using ShaderDefines = std::vector<std::pair<const char*, const char*>>;

	const char* lodDefine;
	const char* vertCountDefine;
	const char* extraDefine;
	uint32_t density;
	uint32_t bladeStrideBytes = sizeof(PGrassCommon::Blade);  // High may include skylighting/collision, Mid may include collision, and Far is 16 bytes.
	std::string densityString;
	uint32_t slopeExtraBlades = 0;
	std::string patchBladeCountString = std::to_string(PatchBladeCount);
	std::string slopeExtraBladesString = "0";
	uint32_t patchesPerQuadrant;
	uint32_t bladeBufferCapacity = 0;
	uint32_t threadGroupSize;
	std::string threadGroupSizeString;
	std::string quadrantCountString = std::to_string(QuadrantCount);

	ID3D11ComputeShader* bladeGeneratorCS = nullptr;
	ID3D11VertexShader* depthVS = nullptr;
	ID3D11VertexShader* vs = nullptr;
	// Feature variants for wetness and local-light availability.
	std::array<ID3D11PixelShader*, 4> pixelShaders{};

	StructuredBuffer* bladesSB = nullptr;
	StructuredBuffer* quadrantGrassSB = nullptr;
	std::vector<uint8_t> quadrantGrassStaging;  // one grass id per byte; the GPU buffer packs 4 per uint
	StructuredBuffer* quadrantGrassCellsSB = nullptr;
	std::vector<uint32_t> quadrantGrassCellsStaging;  // one packed 2x2 LAND-id cell per 16x16 quadrant cell
	StructuredBuffer* quadrantHeightSB = nullptr;
	std::vector<float> quadrantHeightStaging;
	StructuredBuffer* visibleWorkSB = nullptr;
	StructuredBuffer* visibleCompactWorkSB = nullptr;
	std::vector<uint32_t> visibleWorkStaging;
	std::vector<uint32_t> visibleCompactWorkStaging;
	ConstantBuffer* quadrantsCB = nullptr;

	// Skip the staging rebuild + uploads on frames where the quadrant data is unchanged
	size_t lastUploadHash = 0;
	bool hasUploadedQuadrants = false;
	Buffer* argsBuffer = nullptr;
	winrt::com_ptr<ID3D11Buffer> argsStaging;
	Buffer* vertexIndicesBuffer = nullptr;

	void CreateArgsBuffer();
	/** Grow the append buffer for this frame's visible candidate work. */
	void EnsureBladeCapacity(uint64_t requiredBladeCount);
	bool UsesGrassCollision(bool grassCollisionLoaded) const
	{
		const auto lod = std::string_view(lodDefine);
		return grassCollisionLoaded && (lod == "HIGH_LOD" || lod == "MID_LOD");
	}
	bool UsesSimpleLighting() const;
	void AppendVertexShaderDefines(ShaderDefines& defines) const;

	ID3D11ComputeShader* GetBladeGeneratorCS();
	ID3D11VertexShader* GetDepthVS();
	ID3D11VertexShader* GetVS();
	ID3D11PixelShader* GetPS(bool noWetness = false, bool noLocalLights = false);

	static std::string BuildDefineList(std::span<const std::pair<const char*, const char*>> defines);

	template <class ShaderT>
	static ShaderT* CompileShader(const wchar_t* path, std::vector<std::pair<const char*, const char*>>& defines, const char* programType);
};
