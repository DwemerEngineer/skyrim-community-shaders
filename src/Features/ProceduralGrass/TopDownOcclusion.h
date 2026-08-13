#pragma once

#include "Buffer.h"

/**
 * @brief Top-down world-height map of nearby geometry, for coverage/occlusion queries.
 *
 * Owns its render instead of the engine's precipitation occlusion pass, which can't be steered and holds
 * only view-dependent geometry. Texels hold world Z of the topmost surface via a max blend.
 */
class TopDownOcclusion
{
public:
	static TopDownOcclusion* GetSingleton()
	{
		static TopDownOcclusion singleton;
		return &singleton;
	}

	void SetupResources();
	void ClearShaderCache();

	/** @brief Re-renders the map. Call once per frame from a render-time entry point. */
	void Render();

	bool IsReady() const { return heightMap != nullptr; }
	ID3D11ShaderResourceView* GetSRV() const;

	/** @brief World-XY centre of the covered square, snapped to the texel grid. */
	float2 GetWindowCentre() const { return windowCentre; }

	float GetHalfExtent() const { return halfExtent; }
	void SetHalfExtent(float a_halfExtent) { halfExtent = a_halfExtent; }

	/** @brief Value a texel holds where nothing was drawn. */
	static constexpr float EmptyHeight = -1.0e30f;

	uint32_t GetMapDim() const { return mapDim; }
	uint32_t GetDrawCount() const { return lastDrawCount; }

	/** @brief Grid the window snaps to. Set to the coarsest consumer (density map) so both stay world-stable. */
	void SetSnapDim(uint32_t a_snapDim) { snapDim = a_snapDim; }

	float GetMinOccluderRadius() const { return minOccluderRadius; }
	void SetMinOccluderRadius(float a_radius) { minOccluderRadius = a_radius; }

private:
	struct CapturedGeometry
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
	};

	Texture2D* heightMap = nullptr;
	uint32_t mapDim = 1024;
	uint32_t snapDim = 1024;
	float halfExtent = 4096.0f;
	float2 windowCentre = { 0.0f, 0.0f };
	float minOccluderRadius = 8.0f;

	ID3D11VertexShader* heightVS = nullptr;
	ID3D11PixelShader* heightPS = nullptr;
	winrt::com_ptr<ID3DBlob> heightVSBlob;
	winrt::com_ptr<ID3D11BlendState> maxBlend;
	winrt::com_ptr<ID3D11RasterizerState> noCull;
	ConstantBuffer* heightCB = nullptr;

	/** @brief One layout per distinct vertex descriptor. A null entry means that descriptor is skipped. */
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> inputLayouts;

	std::vector<CapturedGeometry> captured;
	float2 capturedCentre = { 1.0e30f, 1.0e30f };
	uint32_t lastDrawCount = 0;

	void CompileShaders();

	/** @brief Rebuilds the geometry list from loaded references, independent of what is in view. */
	void GatherGeometry();

	/** @brief Walks a reference's 3D and records every renderable triangle shape. */
	void CollectFrom(RE::NiAVObject* a_object);

	ID3D11InputLayout* GetInputLayout(const RE::BSGraphics::VertexDesc& a_desc);
};
