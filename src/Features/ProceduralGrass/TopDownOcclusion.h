#pragma once

#include "Buffer.h"

/**
 * @brief Top-down world-height map of nearby geometry, for coverage/occlusion queries.
 *
 * Seperate render pass than engine's precipitation occlusion pass, to avoid the fixed angle and for high configuration,
 * and to cover more than view-dependent geometry.
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

	void Render();

	bool IsReady() const { return heightMapHigh != nullptr && heightMapLow != nullptr; }

	ID3D11ShaderResourceView* GetHighSRV() const;
	ID3D11ShaderResourceView* GetLowSRV() const;

	/** @brief World-XY centre of the covered square, snapped to the texel grid. */
	float2 GetWindowCentre() const { return windowCentre; }

	float GetHalfExtent() const { return halfExtent; }
	void SetHalfExtent(float a_halfExtent)
	{
		if (halfExtent != a_halfExtent) {
			halfExtent = a_halfExtent;
			capturedCentre = { 1.0e30f, 1.0e30f };
		}
	}

	static constexpr float EmptyHigh = -1.0e30f;
	static constexpr float EmptyLow = 1.0e30f;

	uint32_t GetMapDim() const { return mapDim; }
	uint32_t GetDrawCount() const { return lastDrawCount; }

	/** @brief Grid the window snaps to. Set to the coarsest consumer (density map) so both stay world-stable. */
	void SetSnapDim(uint32_t a_snapDim) { snapDim = a_snapDim; }

	float GetMinOccluderRadius() const { return minOccluderRadius; }
	void SetMinOccluderRadius(float a_radius)
	{
		if (minOccluderRadius != a_radius) {
			minOccluderRadius = a_radius;
			capturedCentre = { 1.0e30f, 1.0e30f };
		}
	}

	void SetPaddingWorld(float a_padding) { paddingWorld = a_padding; }

private:
	struct CapturedGeometry
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
		winrt::com_ptr<ID3D11Buffer> vertexBuffer;
		winrt::com_ptr<ID3D11Buffer> indexBuffer;
		ID3D11InputLayout* inputLayout = nullptr;
		uint32_t indexCount = 0;
		uint32_t stride = 0;
	};

	Texture2D* heightMapHigh = nullptr; 
	Texture2D* heightMapLow = nullptr;
	Texture2D* heightMapTmp = nullptr;     // scratch for padding
	Texture2D* heightMapLowTmp = nullptr;
	uint32_t mapDim = 1024;
	uint32_t snapDim = 1024;
	float halfExtent = 4096.0f;
	float2 windowCentre = { 0.0f, 0.0f };
	float minOccluderRadius = 8.0f;
	float paddingWorld = 8.0f;

	ID3D11VertexShader* heightVS = nullptr;
	ID3D11PixelShader* heightPS = nullptr;
	ID3D11ComputeShader* padCS = nullptr;
	winrt::com_ptr<ID3DBlob> heightVSBlob;
	winrt::com_ptr<ID3D11BlendState> maxBlend;
	winrt::com_ptr<ID3D11RasterizerState> noCull;
	ConstantBuffer* heightCB = nullptr;
	ConstantBuffer* padCB = nullptr;

	void PadMaps(ID3D11DeviceContext* a_context);

	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> inputLayouts;
	std::vector<CapturedGeometry> captured;
	float2 capturedCentre = { 1.0e30f, 1.0e30f };
	uint32_t lastDrawCount = 0;

	void CompileShaders();
	void GatherGeometry();
	void CollectFrom(RE::NiAVObject* a_object);

	ID3D11InputLayout* GetInputLayout(const RE::BSGraphics::VertexDesc& a_desc);
};
