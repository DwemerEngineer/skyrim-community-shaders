#include "HiZPyramid.h"

#include "Features/TerrainBlending.h"
#include "Profiler.h"

void HiZPyramid::SetupResources()
{
	paramsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Params>(), "GrassOptimizations::HiZParamsCB");

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = sizeof(uint32_t);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	const uint32_t zero = 0;
	D3D11_SUBRESOURCE_DATA init{ &zero, 0, 0 };
	spdCounter = std::make_unique<Buffer>(bd, &init, "GrassOptimizations::SpdCounter");

	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_TYPELESS;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.FirstElement = 0;
	uav.Buffer.NumElements = 1;
	uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	spdCounter->CreateUAV(uav);
}

void HiZPyramid::ClearShaderCache()
{
	if (baseCS)
		baseCS->Release();
	baseCS = nullptr;
	if (spdCS)
		spdCS->Release();
	spdCS = nullptr;
}

ID3D11ShaderResourceView* HiZPyramid::GetSourceDepthSRV()
{
	// Grass runs before the terrain blending pass, so it takes the original prepass copy; the blended
	// texture would be a frame stale and flicker grass on fast camera movement. Both branches return
	// that same R24_UNORM_X8_TYPELESS view, so GrassHiZCS needs no TERRAIN_BLENDING variant of its
	// `unorm float` declaration the way Util::GetCurrentSceneDepthSRV's R32_FLOAT consumers do.
	auto& tb = globals::features::terrainBlending;
	if (tb.loaded && tb.settings.Enabled && tb.prepassSRVBackup)
		return tb.prepassSRVBackup;
	if (auto* renderer = globals::game::renderer)
		return renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
	return nullptr;
}

bool HiZPyramid::CreateTexture(ID3D11Device* device, uint32_t dstW, uint32_t dstH)
{
	mipUAVs.clear();
	texture.reset();
	paddedWidth = 0;
	paddedHeight = 0;
	mipCount = 1;

	uint32_t mips = 1;
	for (uint32_t d = std::max(dstW, dstH); d > 1; d >>= 1)
		++mips;
	mips = std::min(mips, kExactMips + 1u);

	D3D11_TEXTURE2D_DESC td{};
	td.Width = dstW;
	td.Height = dstH;
	td.MipLevels = mips;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	try {
		texture = std::make_unique<Texture2D>(td, "GrassOptimizations::HiZ");

		// Full-chain SRV for the cull plus single-level views for the reduction passes.
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = DXGI_FORMAT_R32_FLOAT;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MostDetailedMip = 0;
		sd.Texture2D.MipLevels = mips;
		texture->CreateSRV(sd);
	} catch (...) {
		logger::error("[GRASS OPTIMIZATIONS] HiZ texture create failed");
		texture.reset();
		return false;
	}

	for (uint32_t m = 0; m < mips; ++m) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = DXGI_FORMAT_R32_FLOAT;
		ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		ud.Texture2D.MipSlice = m;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		if (FAILED(device->CreateUnorderedAccessView(texture->resource.get(), &ud, uav.put()))) {
			logger::error("[GRASS OPTIMIZATIONS] HiZ mip UAV create failed");
			return false;
		}
		Util::SetResourceName(uav.get(), "GrassOptimizations::HiZ Mip%u UAV", m);
		mipUAVs.push_back(uav);
	}

	paddedWidth = dstW;
	paddedHeight = dstH;
	mipCount = mips;
	return true;
}

bool HiZPyramid::Build(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	valid = false;

	ID3D11ShaderResourceView* srcSRV = GetSourceDepthSRV();
	if (!srcSRV || !paramsCB || !globals::game::renderer)
		return false;

	const auto [screenW, screenH] = globals::game::renderer->GetScreenSize();

	// Dynamic resolution renders the prepass into a top-left sub-rect of a full-size target, so the
	// extent the depth actually covers is the scaled one. NDC maps onto that, not the nominal target.
	const auto& gsRuntime = globals::game::graphicsState->GetRuntimeData();
	const float drX = std::clamp(gsRuntime.dynamicResolutionWidthRatio, 0.01f, 1.0f);
	const float drY = std::clamp(gsRuntime.dynamicResolutionHeightRatio, 0.01f, 1.0f);
	const uint32_t srcW = std::max(1u, (uint32_t)std::lround(screenW * drX));
	const uint32_t srcH = std::max(1u, (uint32_t)std::lround(screenH * drY));

	const uint32_t validW = (srcW + kDownsampleFactor - 1) / kDownsampleFactor;
	const uint32_t validH = (srcH + kDownsampleFactor - 1) / kDownsampleFactor;
	if (!validW || !validH)
		return false;

	// Converts the cull's nominal-pixel projPx into texels. Its radius is one scalar for both axes, so
	// the larger ratio wins: too small a radius picks a level whose fixed 3x3 footprint misses part of
	// the instance, culling it against an incomplete max.
	texelPixels = kDownsampleFactor / std::max(drX, drY);

	// Sized from the nominal extent so a shifting dynamic-resolution ratio never reallocates, then padded to SPD's tile granularity so every allocated level halves exactly. An odd level would drop its last row, underestimate the max, and cull visible grass.
	const auto padToTile = [](uint32_t v) { return (v + tileSize - 1) & ~(tileSize - 1); };
	const uint32_t padW = padToTile(((uint32_t)screenW + kDownsampleFactor - 1) / kDownsampleFactor);
	const uint32_t padH = padToTile(((uint32_t)screenH + kDownsampleFactor - 1) / kDownsampleFactor);

	if ((padW != paddedWidth || padH != paddedHeight) && !CreateTexture(device, padW, padH))
		return false;

	width = validW;
	height = validH;

	// One variant only, since the only source is the game's R24_UNORM_X8_TYPELESS prepass copy.
	if (!baseCS) {
		baseCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassHiZCS.hlsl", {}, "cs_5_0"));
		if (!baseCS) {
			logger::error("[GRASS OPTIMIZATIONS] HiZ CS load failed — occlusion culling disabled");
			return false;
		}
	}

	if (!spdCS) {
		spdCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\SPD\\SPD.hlsl", {}, "cs_5_0"));
		if (!spdCS)
			logger::error("[GRASS OPTIMIZATIONS] SPD load failed — large instances will not be occlusion culled");
	}

	// Threads past the rendered sub-rect read beyond it, so the base pass's out-of-bounds guard writes 1.0 there and neither the padding nor the unrendered margin can cull.
	paramsCB->Update(Params{ srcW, srcH, padW, padH });

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11ShaderResourceView* nullSRV = nullptr;

	globals::profiler->BeginPass("GrassOptimizations::HiZBase");
	ID3D11Buffer* cb = paramsCB->CB();
	ID3D11UnorderedAccessView* baseUAV = mipUAVs[0].get();
	ctx->CSSetShader(baseCS, nullptr, 0);
	ctx->CSSetConstantBuffers(0, 1, &cb);
	ctx->CSSetShaderResources(0, 1, &srcSRV);
	ctx->CSSetUnorderedAccessViews(0, 1, &baseUAV, nullptr);
	ctx->Dispatch((padW + 7) / 8, (padH + 7) / 8, 1);
	ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ctx->CSSetShaderResources(0, 1, &nullSRV);
	globals::profiler->EndPass();

	// Each level is the exact max of the one above, so an instance of any on-screen size is testable against a fixed number of texels.
	// One dispatch for the whole chain, every group reducing its own tile from LDS.
	if (spdCS && spdCounter && mipCount > 1) {
		globals::profiler->BeginPass("GrassOptimizations::HiZMips");

		const uint32_t outputMips = mipCount - 1;
		const uint32_t groupsX = padW / tileSize;
		const uint32_t groupsY = padH / tileSize;

		paramsCB->Update(Params{ padW, padH, outputMips, groupsX * groupsY });

		ID3D11UnorderedAccessView* spdUAVs[14]{};
		for (uint32_t i = 0; i < outputMips; ++i)
			spdUAVs[i] = mipUAVs[i + 1].get();
		spdUAVs[12] = spdCounter->uav.get();
		spdUAVs[13] = mipUAVs[0].get();

		ctx->CSSetShader(spdCS, nullptr, 0);
		ctx->CSSetUnorderedAccessViews(0, 14, spdUAVs, nullptr);
		ctx->Dispatch(groupsX, groupsY, 1);

		ID3D11UnorderedAccessView* spdNulls[14]{};
		ctx->CSSetUnorderedAccessViews(0, 14, spdNulls, nullptr);
		globals::profiler->EndPass();
	}

	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::info("[GRASS OPTIMIZATIONS] HiZ occlusion cull active: {}x{} tiles (1/{} res) in a {}x{} texture, {} mips, source=POST_ZPREPASS_COPY (R24_UNORM)",
			validW, validH, kDownsampleFactor, padW, padH, GetMipCount());
	}

	valid = true;
	return true;
}
