#pragma once

#include "Buffer.h"

/**
 * @brief Top-down world-height map of nearby geometry, for coverage/occlusion queries.
 *
 * Separate from the engine's precipitation occlusion pass so the projection and resolution can be configured independently.
 * and to cover more than view-dependent geometry.
 */
class TopDownOcclusion
{
public:
	void SetupResources();
	void ClearShaderCache();
	/** @brief Drops captured geometry and rendered maps after a load or scene transition. */
	void Invalidate();

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
			Invalidate();
		}
	}

	static constexpr float EmptyHigh = -1.0e30f;
	static constexpr float EmptyLow = 1.0e30f;

	uint32_t GetMapDim() const { return mapDim; }
	uint32_t GetDrawCount() const { return lastDrawCount; }

	/** @brief Grid the window snaps to. Set to the coarsest consumer (density map) so both stay world-stable. */
	void SetSnapDim(uint32_t a_snapDim)
	{
		if (snapDim != a_snapDim) {
			snapDim = a_snapDim;
			renderCacheValid = false;
		}
	}

	float GetMinOccluderRadius() const { return minOccluderRadius; }
	void SetMinOccluderRadius(float a_radius)
	{
		if (minOccluderRadius != a_radius) {
			minOccluderRadius = a_radius;
			Invalidate();
		}
	}

	void SetPaddingWorld(float a_padding)
	{
		if (paddingWorld != a_padding) {
			paddingWorld = a_padding;
			renderCacheValid = false;
		}
	}

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
	Texture2D* heightMapTmp = nullptr;  // scratch for padding
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

	bool PadMaps(ID3D11DeviceContext* a_context);

	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> inputLayouts;
	std::vector<CapturedGeometry> captured;
	float2 capturedCentre = { 0.0f, 0.0f };
	const RE::TESWorldSpace* capturedWorldspace = nullptr;
	bool capturedGeometryValid = false;
	uint64_t capturedGeometryRevision = 0;
	uint32_t lastDrawCount = 0;

	struct RenderCacheState
	{
		float2 windowCentre = { 0.0f, 0.0f };
		float halfExtent = 0.0f;
		float paddingWorld = 0.0f;
		uint32_t mapDim = 0;
		uint32_t snapDim = 0;
		uint64_t capturedGeometryRevision = 0;

		bool operator==(const RenderCacheState& other) const
		{
			return windowCentre.x == other.windowCentre.x &&
			       windowCentre.y == other.windowCentre.y &&
			       halfExtent == other.halfExtent &&
			       paddingWorld == other.paddingWorld &&
			       mapDim == other.mapDim &&
			       snapDim == other.snapDim &&
			       capturedGeometryRevision == other.capturedGeometryRevision;
		}
	};

	RenderCacheState renderCacheState{};
	bool renderCacheValid = false;

	void CompileShaders();
	void GatherGeometry();
	void CollectFrom(RE::NiAVObject* a_object);
	RenderCacheState GetRenderCacheState() const;
	bool CanReuseRenderedMaps() const;
	void CommitRenderedMaps();

	ID3D11InputLayout* GetInputLayout(const RE::BSGraphics::VertexDesc& a_desc);
};
