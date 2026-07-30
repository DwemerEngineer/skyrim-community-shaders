#include "HiZPyramid.h"

#include "Features/TerrainBlending.h"

void HiZPyramid::SetupResources()
{
	paramsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Params>(), "GrassOptimizations::HiZParamsCB");
}

void HiZPyramid::ClearShaderCache()
{
	if (baseCS)
		baseCS->Release();
	baseCS = nullptr;
	if (mipCS)
		mipCS->Release();
	mipCS = nullptr;
}

ID3D11ShaderResourceView* HiZPyramid::GetSourceDepthSRV()
{
	// Since grass runs before the terrain blending pass, it must use the original prepass copy, not the blended depth texture. Otherwise, the culling would run against last frame's depth and flicker grass during fast camera movement.
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
	mipSRVs.clear();
	texture.reset();
	width = 0;
	height = 0;

	uint32_t mips = 1;
	for (uint32_t d = std::max(dstW, dstH); d > 1; d >>= 1)
		++mips;

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

		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = DXGI_FORMAT_R32_FLOAT;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MostDetailedMip = m;
		sd.Texture2D.MipLevels = 1;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		if (FAILED(device->CreateShaderResourceView(texture->resource.get(), &sd, srv.put()))) {
			logger::error("[GRASS OPTIMIZATIONS] HiZ mip SRV create failed");
			return false;
		}
		Util::SetResourceName(srv.get(), "GrassOptimizations::HiZ Mip%u SRV", m);
		mipSRVs.push_back(srv);
	}

	width = dstW;
	height = dstH;
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
	const uint32_t dstW = ((uint32_t)screenW + downsampleFactor - 1) / downsampleFactor;
	const uint32_t dstH = ((uint32_t)screenH + downsampleFactor - 1) / downsampleFactor;
	if (!dstW || !dstH)
		return false;

	if ((dstW != width || dstH != height) && !CreateTexture(device, dstW, dstH))
		return false;

	// One variant only, since the only source is the game's R24_UNORM_X8_TYPELESS prepass copy.
	if (!baseCS) {
		baseCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassHiZCS.hlsl", {}, "cs_5_0"));
		if (!baseCS) {
			logger::error("[GRASS OPTIMIZATIONS] HiZ CS load failed — occlusion culling disabled");
			return false;
		}
	}

	if (!mipCS) {
		mipCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassHiZMipCS.hlsl", {}, "cs_5_0"));
		if (!mipCS)
			logger::error("[GRASS OPTIMIZATIONS] HiZ mip CS load failed — large clumps will not be occlusion culled");
	}

	paramsCB->Update(Params{ (uint32_t)screenW, (uint32_t)screenH, dstW, dstH });

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11ShaderResourceView* nullSRV = nullptr;

	ID3D11Buffer* cb = paramsCB->CB();
	ID3D11UnorderedAccessView* baseUAV = mipUAVs[0].get();
	ctx->CSSetShader(baseCS, nullptr, 0);
	ctx->CSSetConstantBuffers(0, 1, &cb);
	ctx->CSSetShaderResources(0, 1, &srcSRV);
	ctx->CSSetUnorderedAccessViews(0, 1, &baseUAV, nullptr);
	ctx->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
	ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ctx->CSSetShaderResources(0, 1, &nullSRV);

	// Each level is the exact max of the one above, so an instance of any on-screen size is testable against a fixed number of texels.
	if (mipCS && mipCount > 1) {
		ctx->CSSetShader(mipCS, nullptr, 0);
		uint32_t srcW = dstW, srcH = dstH;
		for (uint32_t m = 1; m < mipCount; ++m) {
			const uint32_t mw = std::max(1u, srcW >> 1);
			const uint32_t mh = std::max(1u, srcH >> 1);

			paramsCB->Update(Params{ srcW, srcH, mw, mh });

			ID3D11ShaderResourceView* mipSRV = mipSRVs[m - 1].get();
			ID3D11UnorderedAccessView* mipUAV = mipUAVs[m].get();
			ctx->CSSetShaderResources(0, 1, &mipSRV);
			ctx->CSSetUnorderedAccessViews(0, 1, &mipUAV, nullptr);
			ctx->Dispatch((mw + 7) / 8, (mh + 7) / 8, 1);
			ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			ctx->CSSetShaderResources(0, 1, &nullSRV);

			srcW = mw;
			srcH = mh;
		}
	}

	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::info("[GRASS OPTIMIZATIONS] HiZ occlusion cull active: {}x{} tiles (1/{} res), {} mips, source=POST_ZPREPASS_COPY (R24_UNORM)",
			dstW, dstH, downsampleFactor, GetMipCount());
	}

	valid = true;
	return true;
}
