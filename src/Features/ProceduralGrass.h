#pragma once

#include "ProceduralGrass/PGrassCommon.h"
#include "ProceduralGrass/PGrassRenderer.h"

using namespace PGrassCommon;

struct ProceduralGrass : Feature
{
public:
	static ProceduralGrass* GetSingleton()
	{
		static ProceduralGrass singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Procedural Grass"; }
	virtual inline std::string GetShortName() override { return "ProceduralGrass"; }

	struct Settings
	{
		bool Enabled = true;
		int32_t Quality = 1;
		int32_t ThreadGroupSize = 6;
	};

	Settings settings;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; }

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	void DeferredRendering() const;

	struct Main_RenderShadowmasks_UpdateCamera
	{
		static void thunk(RE::BSGraphics::State* state, RE::NiCamera* camera, bool flag);
		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	class Timer {
	public:
		using clock = std::chrono::steady_clock;  // or high_resolution_clock
		Timer() : last(clock::now()) {}

		// Call once per frame:
		float tick() {
			auto now   = clock::now();
			std::chrono::duration<double> diff = now - last;
			last = now;
			return static_cast<float>(diff.count());  // seconds as float
		}

	private:
		clock::time_point last;
	};

	Timer fpsTimer;
	
	enum class Quality : uint8_t
	{
		Low = 0,
		Medium = 1,
		High = 2,
		Ultra = 3,
		Count = 4
	};

	const char* QualityNames[static_cast<uint8_t>(Quality::Count)] = { "160", "192", "256", "320" };
	uint32_t QualityDensities[static_cast<uint8_t>(Quality::Count)] = { 160, 192, 256, 320 };

	PGrassRenderer<9, 4>* grassRendererHighLOD = nullptr;
	PGrassRenderer<16, 2>* grassRendererMidLOD = nullptr;
	PGrassRenderer<75, 1>* grassRendererLowLOD = nullptr;

	std::vector<Quadrant> quadrantsHighLOD;
	std::vector<Quadrant> quadrantsMidLOD;
	std::vector<Quadrant> quadrantsLowLOD;

	ID3D11RasterizerState* noCullRS = nullptr;
	ID3D11DepthStencilState* depthOnDSS = nullptr;
	ID3D11DepthStencilState* depthWriteDS = nullptr;
	ID3D11DepthStencilState* depthEqualDS = nullptr;
	ID3D11BlendState* depthOnlyBlend = nullptr;
	ID3D11BlendState* defaultBlend = nullptr;

	ID3D11SamplerState* linearClampSampler = nullptr;
	ID3D11SamplerState* shadowSampler = nullptr;

	ConstantBuffer* grassGlobalsCB = nullptr;
	ConstantBuffer* grassTypesArrayCB = nullptr;
	Buffer* vertexIndicesHighBuffer = nullptr;
	Buffer* vertexIndicesLowBuffer = nullptr;

	ID3D11Texture1D* grassDiffuseRoughnessArray = nullptr;
	ID3D11ShaderResourceView* grassDiffuseRoughnessArraySRV = nullptr;

	ID3D11Texture1D* grassColorLUTsArray = nullptr;
	ID3D11ShaderResourceView* grassColorLUTsArraySRV = nullptr;

	winrt::com_ptr<ID3D11ShaderResourceView> heightMap;
	winrt::com_ptr<ID3D11ShaderResourceView> grassMap;

	static bool ConsoleFunc_ToggleGrass();

	static std::vector<uint16_t> CreateVertexIndicesArray(uint16_t vertCount);

	static void CopyDepthBuffer(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer);
	static void SetViewport(ID3D11DeviceContext* ctx, uint32_t width, uint32_t height);
	static void ClearRenderTargets(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtvs[7]);

	void PostDepthRendering();
	void GetVisibleQuadrants();
	void PostDepthRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer);
	void GenerateBlades(ID3D11DeviceContext* ctx) const;
	void RenderDepth(ID3D11DeviceContext* ctx) const;

	void DeferredRenderPrep(ID3D11DeviceContext* ctx, RE::BSGraphics::Renderer* renderer) const;
	void RenderGrass(ID3D11DeviceContext* ctx) const;
};
