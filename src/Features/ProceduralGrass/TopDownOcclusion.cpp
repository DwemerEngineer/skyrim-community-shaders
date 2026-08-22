#include "TopDownOcclusion.h"

#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/game.h"

#include <d3dcompiler.h>

namespace
{
	struct alignas(16) HeightCB
	{
		float4 worldRow0;
		float4 worldRow1;
		float4 worldRow2;
		float2 windowCentre;
		float halfExtent;
		float padding;
	};

	struct alignas(16) PadCB
	{
		int32_t axisX, axisY, radius, pad0;
		uint32_t dimX, dimY, pad1, pad2;
	};

	// Keeps the bytecode, which Util::CompileShader discards but input-layout validation needs.
	ID3DBlob* CompileWithBlob(const wchar_t* a_path, const char* a_target)
	{
		winrt::com_ptr<ID3DBlob> code;
		winrt::com_ptr<ID3DBlob> errors;
		const auto hr = D3DCompileFromFile(a_path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", a_target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
		if (FAILED(hr)) {
			if (errors)
				logger::error("[Top Down Occlusion] {} failed: {}", a_target, static_cast<const char*>(errors->GetBufferPointer()));
			return nullptr;
		}
		return code.detach();
	}
}

void TopDownOcclusion::SetupResources()
{
	if (heightMap)
		return;

	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = mapDim;
	texDesc.Height = mapDim;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	texDesc.SampleDesc = { 1, 0 };
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

	heightMap = new Texture2D(texDesc, "TopDownOcclusion::HeightMap");
	heightMapLow = new Texture2D(texDesc, "TopDownOcclusion::HeightMapLow");

	D3D11_TEXTURE2D_DESC tmpDesc = texDesc;
	tmpDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	heightMapTmp = new Texture2D(tmpDesc, "TopDownOcclusion::HeightMapTmp");
	heightMapLowTmp = new Texture2D(tmpDesc, "TopDownOcclusion::HeightMapLowTmp");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	heightMap->CreateSRV(srvDesc);
	heightMapLow->CreateSRV(srvDesc);
	heightMapTmp->CreateSRV(srvDesc);
	heightMapLowTmp->CreateSRV(srvDesc);

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	heightMap->CreateUAV(uavDesc);
	heightMapLow->CreateUAV(uavDesc);
	heightMapTmp->CreateUAV(uavDesc);
	heightMapLowTmp->CreateUAV(uavDesc);

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = texDesc.Format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	heightMap->CreateRTV(rtvDesc);
	heightMapLow->CreateRTV(rtvDesc);

	// Seperate render targets for low and high, so that geometry with overhangs such as tree branches does not occlude the low map.
	blendDesc.IndependentBlendEnable = TRUE;
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
	blendDesc.RenderTarget[1] = blendDesc.RenderTarget[0];
	blendDesc.RenderTarget[1].BlendOp = D3D11_BLEND_OP_MIN;
	blendDesc.RenderTarget[1].BlendOpAlpha = D3D11_BLEND_OP_MIN;
	device->CreateBlendState(&blendDesc, maxBlend.put());

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = FALSE;
	device->CreateRasterizerState(&rasterDesc, noCull.put());

	heightCB = new ConstantBuffer(ConstantBufferDesc<HeightCB>());
	padCB = new ConstantBuffer(ConstantBufferDesc<PadCB>());

	CompileShaders();
}

void TopDownOcclusion::CompileShaders()
{
	if (heightVS && heightPS && padCS)
		return;

	auto device = globals::d3d::device;

	if (!heightVS) {
		heightVSBlob.attach(CompileWithBlob(L"Data\\Shaders\\TopDownOcclusion\\HeightVS.hlsl", "vs_5_0"));
		if (heightVSBlob)
			device->CreateVertexShader(heightVSBlob->GetBufferPointer(), heightVSBlob->GetBufferSize(), nullptr, &heightVS);
	}

	if (!heightPS) {
		winrt::com_ptr<ID3DBlob> ps;
		ps.attach(CompileWithBlob(L"Data\\Shaders\\TopDownOcclusion\\HeightPS.hlsl", "ps_5_0"));
		if (ps)
			device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &heightPS);
	}

	if (!padCS) {
		winrt::com_ptr<ID3DBlob> cs;
		cs.attach(CompileWithBlob(L"Data\\Shaders\\TopDownOcclusion\\OcclusionPadCS.hlsl", "cs_5_0"));
		if (cs)
			device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &padCS);
	}
}

void TopDownOcclusion::ClearShaderCache()
{
	if (heightVS) {
		heightVS->Release();
		heightVS = nullptr;
	}
	if (heightPS) {
		heightPS->Release();
		heightPS = nullptr;
	}
	if (padCS) {
		padCS->Release();
		padCS = nullptr;
	}
	heightVSBlob = nullptr;

	captured.clear();
	capturedCentre = { 1.0e30f, 1.0e30f };
	inputLayouts.clear();
	CompileShaders();
}

ID3D11ShaderResourceView* TopDownOcclusion::GetHighSRV() const
{
	return heightMapHigh ? heightMapHigh->srv.get() : nullptr;
}

ID3D11ShaderResourceView* TopDownOcclusion::GetLowSRV() const
{
	return heightMapLow ? heightMapLow->srv.get() : nullptr;
}

ID3D11InputLayout* TopDownOcclusion::GetInputLayout(const RE::BSGraphics::VertexDesc& a_desc)
{
	uint64_t descKey;
	std::memcpy(&descKey, &a_desc, sizeof(descKey));

	auto entry = inputLayouts.find(descKey);
	if (entry != inputLayouts.end())
		return entry->second.get();

	auto& layout = inputLayouts[descKey];

	const uint32_t strideBytes = uint32_t(descKey & 0xF) * 4;
	uint32_t positionBytes = strideBytes;
	static constexpr std::pair<RE::BSGraphics::Vertex::Flags, RE::BSGraphics::Vertex::Attribute> attributes[] = {
		{ RE::BSGraphics::Vertex::VF_UV, RE::BSGraphics::Vertex::VA_TEXCOORD0 },
		{ RE::BSGraphics::Vertex::VF_UV_2, RE::BSGraphics::Vertex::VA_TEXCOORD1 },
		{ RE::BSGraphics::Vertex::VF_NORMAL, RE::BSGraphics::Vertex::VA_NORMAL },
		{ RE::BSGraphics::Vertex::VF_TANGENT, RE::BSGraphics::Vertex::VA_BINORMAL },
		{ RE::BSGraphics::Vertex::VF_COLORS, RE::BSGraphics::Vertex::VA_COLOR },
		{ RE::BSGraphics::Vertex::VF_SKINNED, RE::BSGraphics::Vertex::VA_SKINNING },
		{ RE::BSGraphics::Vertex::VF_LANDDATA, RE::BSGraphics::Vertex::VA_LANDDATA },
		{ RE::BSGraphics::Vertex::VF_EYEDATA, RE::BSGraphics::Vertex::VA_EYEDATA },
	};
	for (auto [flag, attribute] : attributes) {
		if (a_desc.HasFlag(flag)) {
			const uint32_t attributeOffset = a_desc.GetAttributeOffset(attribute);
			if (attributeOffset > 0 && attributeOffset < positionBytes)
				positionBytes = attributeOffset;
		}
	}

	const D3D11_INPUT_ELEMENT_DESC element{
		"POSITION", 0,
		positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT,
		0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
	};

	if (heightVSBlob)
		globals::d3d::device->CreateInputLayout(&element, 1, heightVSBlob->GetBufferPointer(), heightVSBlob->GetBufferSize(), layout.put());

	// A null entry stays cached, so an unsupported descriptor is only attempted once.
	return layout.get();
}

void TopDownOcclusion::CollectFrom(RE::NiAVObject* a_object)
{
	if (!a_object || a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
		return;

	if (auto* geometry = a_object->AsGeometry()) {
		auto* triShape = geometry->AsTriShape();
		if (!triShape || geometry->worldBound.radius <= minOccluderRadius)
			return;

		RE::BSFadeNode* fadeNode = nullptr;
		RE::NiNode* parent = geometry->parent;
		while (parent && !fadeNode) {
			fadeNode = parent->AsFadeNode();
			parent = parent->parent;
		}

		if (fadeNode) {
			if (auto extraData = fadeNode->GetExtraData("BSX")) {
				auto bsxFlags = (RE::BSXFlags*)extraData;
				auto value = static_cast<int32_t>(bsxFlags->value);

				if (value & (static_cast<int32_t>(RE::BSXFlags::Flag::kRagdoll) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kEditorMarker) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kDynamic) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kAddon) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kNeedsTransformUpdate) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kMagicShaderParticles) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kLights) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kBreakable) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kSearchedBreakable))) {
					return;
				}
			}
		}

		using enum RE::BSShaderProperty::EShaderPropertyFlag;
		if (auto* shaderProperty = geometry->GetGeometryRuntimeData().shaderProperty.get()) {
			if (shaderProperty->flags.any(kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape))
				return;
		}

		auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
			return;

		const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
		const auto& desc = rendererData->vertexDesc;
		if (indexCount == 0 || !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
			return;

		uint64_t descKey;
		std::memcpy(&descKey, &desc, sizeof(descKey));
		const uint32_t stride = uint32_t(descKey & 0xF) * 4;
		auto* layout = GetInputLayout(desc);
		if (stride == 0 || !layout)
			return;

		CapturedGeometry capturedGeometry{};
		capturedGeometry.geometry = RE::NiPointer<RE::BSGeometry>(geometry);
		capturedGeometry.world = geometry->world;
		capturedGeometry.vertexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer));
		capturedGeometry.indexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer));
		capturedGeometry.inputLayout = layout;
		capturedGeometry.indexCount = indexCount;
		capturedGeometry.stride = stride;
		captured.push_back(std::move(capturedGeometry));
		return;
	}

	if (auto* node = a_object->AsNode()) {
		for (auto& child : node->GetChildren())
			CollectFrom(child.get());
	}
}

void TopDownOcclusion::GatherGeometry()
{
	captured.clear();

	const auto player = RE::PlayerCharacter::GetSingleton();
	const auto tes = RE::TES::GetSingleton();
	if (!player || !tes)
		return;

	// Use the loaded reference list, not a render queue, so geometry behind the camera is still in the map.
	const float radius = halfExtent * 1.5f;
	tes->ForEachReferenceInRange(player, radius, [&](RE::TESObjectREFR* a_ref) {
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded())
			return RE::BSContainer::ForEachResult::kContinue;

		if (a_ref->As<RE::Actor>())
			return RE::BSContainer::ForEachResult::kContinue;

		CollectFrom(a_ref->Get3D());
		return RE::BSContainer::ForEachResult::kContinue;
	});

	// The height blend is order-independent. Group layouts so Render avoids rebinding them for every geometry.
	std::ranges::sort(captured, [](const CapturedGeometry& lhs, const CapturedGeometry& rhs) {
		return std::less<>{}(lhs.inputLayout, rhs.inputLayout);
	});
}

void TopDownOcclusion::Render()
{
	if (!heightMapHigh || !heightMapLow || !heightVS || !heightPS)
		return;

	if (Util::IsInterior())
		return;

	auto context = globals::d3d::context;

	// Snap the window to the texel grid so it jumps in whole texels instead of sliding and re-quantising silhouette edges each frame.
	// Snapping to the coarsest consumer (snapDim) keeps both maps stable.
	const auto& eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float texel = halfExtent * 2.0f / snapDim;
	windowCentre = {
		std::floor(eye.x / texel) * texel,
		std::floor(eye.y / texel) * texel
	};

	const float captureRefreshDistance = std::max(texel, halfExtent * 0.25f);
	if (std::abs(windowCentre.x - capturedCentre.x) >= captureRefreshDistance || std::abs(windowCentre.y - capturedCentre.y) >= captureRefreshDistance) {
		GatherGeometry();
		capturedCentre = windowCentre;
	}

	ID3D11RenderTargetView* previousRTVs[8]{};
	ID3D11DepthStencilView* previousDSV = nullptr;
	context->OMGetRenderTargets(8, previousRTVs, &previousDSV);

	uint32_t previousViewportCount = 1;
	D3D11_VIEWPORT previousViewport{};
	context->RSGetViewports(&previousViewportCount, &previousViewport);

	const float clearHigh[4] = { EmptyHigh, EmptyHigh, EmptyHigh, EmptyHigh };
	const float clearLow[4] = { EmptyLow, EmptyLow, EmptyLow, EmptyLow };
	context->ClearRenderTargetView(heightMapHigh->rtv.get(), clearHigh);
	context->ClearRenderTargetView(heightMapLow->rtv.get(), clearLow);

	ID3D11RenderTargetView* rtvs[2] = { heightMapHigh->rtv.get(), heightMapLow->rtv.get() };
	context->OMSetRenderTargets(2, rtvs, nullptr);
	context->OMSetBlendState(maxBlend.get(), nullptr, 0xFFFFFFFF);
	context->RSSetState(noCull.get());

	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(mapDim), float(mapDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(heightVS, nullptr, 0);
	context->PSSetShader(heightPS, nullptr, 0);
	auto* constants = heightCB->CB();
	context->VSSetConstantBuffers(0, 1, &constants);

	lastDrawCount = 0;
	ID3D11InputLayout* currentLayout = nullptr;
	for (const auto& entry : captured) {
		auto* geometry = entry.geometry.get();
		if (!geometry)
			continue;

		if (entry.inputLayout != currentLayout) {
			context->IASetInputLayout(entry.inputLayout);
			currentLayout = entry.inputLayout;
		}

		const UINT offset = 0;
		const UINT stride = entry.stride;
		auto* vertexBuffer = entry.vertexBuffer.get();
		context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
		context->IASetIndexBuffer(entry.indexBuffer.get(), DXGI_FORMAT_R16_UINT, 0);

		const auto& transform = geometry->world;
		const auto& rotate = transform.rotate;
		const float scale = transform.scale;

		HeightCB data{};
		data.worldRow0 = { rotate.entry[0][0] * scale, rotate.entry[0][1] * scale, rotate.entry[0][2] * scale, transform.translate.x };
		data.worldRow1 = { rotate.entry[1][0] * scale, rotate.entry[1][1] * scale, rotate.entry[1][2] * scale, transform.translate.y };
		data.worldRow2 = { rotate.entry[2][0] * scale, rotate.entry[2][1] * scale, rotate.entry[2][2] * scale, transform.translate.z };
		data.windowCentre = windowCentre;
		data.halfExtent = halfExtent;
		heightCB->Update(data);

		context->DrawIndexed(entry.indexCount, 0, 0);
		lastDrawCount++;
	}

	ID3D11RenderTargetView* nullRTVs[2] = { nullptr, nullptr };
	context->OMSetRenderTargets(2, nullRTVs, nullptr);

	PadMaps(context);

	context->OMSetRenderTargets(8, previousRTVs, previousDSV);
	for (auto* view : previousRTVs) {
		if (view)
			view->Release();
	}
	if (previousDSV)
		previousDSV->Release();

	if (previousViewportCount)
		context->RSSetViewports(previousViewportCount, &previousViewport);
}

void TopDownOcclusion::PadMaps(ID3D11DeviceContext* context)
{
	if (!padCS || !heightMapTmp || !heightMapLowTmp || !padCB)
		return;

	// Convert world-space padding to texels. Skip padding if less than one texel.
	const float texel = halfExtent * 2.0f / mapDim;
	const int radius = static_cast<int>(std::lround(paddingWorld / texel));
	if (radius <= 0)
		return;

	context->CSSetShader(padCS, nullptr, 0);
	auto* cbuf = padCB->CB();
	context->CSSetConstantBuffers(0, 1, &cbuf);
	const UINT groups = (mapDim + 7) / 8;

	ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };

	// Horizontal pass: heightMap/Low (SRV) -> Tmp (UAV).
	PadCB h{ 1, 0, radius, 0, mapDim, mapDim, 0, 0 };
	padCB->Update(h);
	ID3D11ShaderResourceView* srvH[2] = { heightMap->srv.get(), heightMapLow->srv.get() };
	ID3D11UnorderedAccessView* uavH[2] = { heightMapTmp->uav.get(), heightMapLowTmp->uav.get() };
	context->CSSetShaderResources(0, 2, srvH);
	context->CSSetUnorderedAccessViews(0, 2, uavH, nullptr);
	context->Dispatch(groups, groups, 1);
	context->CSSetShaderResources(0, 2, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

	// Vertical pass: Tmp (SRV) -> heightMap/Low (UAV), padded in place.
	PadCB v{ 0, 1, radius, 0, mapDim, mapDim, 0, 0 };
	padCB->Update(v);
	ID3D11ShaderResourceView* srvV[2] = { heightMapTmp->srv.get(), heightMapLowTmp->srv.get() };
	ID3D11UnorderedAccessView* uavV[2] = { heightMap->uav.get(), heightMapLow->uav.get() };
	context->CSSetShaderResources(0, 2, srvV);
	context->CSSetUnorderedAccessViews(0, 2, uavV, nullptr);
	context->Dispatch(groups, groups, 1);
	context->CSSetShaderResources(0, 2, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);
}
