#include "TopDownOcclusion.h"

#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <d3dcompiler.h>

namespace
{
	struct alignas(16) PadCB
	{
		int32_t axisX, axisY, radius, pad0;
		uint32_t dimX, dimY, pad1, pad2;
	};

	// Keeps the bytecode, which Util::CompileShader discards but input-layout validation needs.
	// Adds the same stage define as Util::CompileShader so one source file can hold several stages.
	ID3DBlob* CompileWithBlob(const wchar_t* a_path, const char* a_target, const D3D_SHADER_MACRO* defines = nullptr)
	{
		std::vector<D3D_SHADER_MACRO> macros;
		for (auto define = defines; define && define->Name; ++define)
			macros.push_back(*define);
		if (!strncmp(a_target, "vs_", 3))
			macros.push_back({ "VSHADER", "" });
		else if (!strncmp(a_target, "ps_", 3))
			macros.push_back({ "PSHADER", "" });
		else if (!strncmp(a_target, "cs_", 3))
			macros.push_back({ "COMPUTESHADER", "" });
		macros.push_back({ nullptr, nullptr });

		winrt::com_ptr<ID3DBlob> code;
		winrt::com_ptr<ID3DBlob> errors;
		const auto hr = D3DCompileFromFile(a_path, macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", a_target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
		if (FAILED(hr)) {
			if (errors)
				logger::error("[Top Down Occlusion] {} failed: {}", a_target, static_cast<const char*>(errors->GetBufferPointer()));
			return nullptr;
		}
		return code.detach();
	}

	DXGI_FORMAT GetVertexPositionFormat(const RE::BSGraphics::VertexDesc& desc)
	{
		uint64_t descKey;
		std::memcpy(&descKey, &desc, sizeof(descKey));
		uint32_t positionBytes = uint32_t(descKey & 0xF) * 4;
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
			if (desc.HasFlag(flag)) {
				const uint32_t offset = desc.GetAttributeOffset(attribute);
				if (offset > 0 && offset < positionBytes)
					positionBytes = offset;
			}
		}
		return positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
}

void TopDownOcclusion::SetupResources()
{
	if (heightMapHigh && heightMapLow)
		return;

	renderCacheValid = false;

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

	heightMapHigh = new Texture2D(texDesc, "TopDownOcclusion::HeightMap");
	heightMapLow = new Texture2D(texDesc, "TopDownOcclusion::HeightMapLow");

	D3D11_TEXTURE2D_DESC tmpDesc = texDesc;
	tmpDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	heightMapTmp = new Texture2D(tmpDesc, "TopDownOcclusion::HeightMapTmp");
	heightMapLowTmp = new Texture2D(tmpDesc, "TopDownOcclusion::HeightMapLowTmp");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	heightMapHigh->CreateSRV(srvDesc);
	heightMapLow->CreateSRV(srvDesc);
	heightMapTmp->CreateSRV(srvDesc);
	heightMapLowTmp->CreateSRV(srvDesc);

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	heightMapHigh->CreateUAV(uavDesc);
	heightMapLow->CreateUAV(uavDesc);
	heightMapTmp->CreateUAV(uavDesc);
	heightMapLowTmp->CreateUAV(uavDesc);

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = texDesc.Format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	heightMapHigh->CreateRTV(rtvDesc);
	heightMapLow->CreateRTV(rtvDesc);

	// Separate maps keep overhead geometry out of the low occlusion layer.
	D3D11_BLEND_DESC blendDesc{};
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
	if (FAILED(device->CreateBlendState(&blendDesc, maxBlend.put())))
		logger::error("[Top Down Occlusion] Failed to create the height-range blend state");

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = FALSE;
	device->CreateRasterizerState(&rasterDesc, noCull.put());

	heightCB = new ConstantBuffer(ConstantBufferDesc<HeightCB>(), "TopDownOcclusion::HeightCB");
	padCB = new ConstantBuffer(ConstantBufferDesc<PadCB>(), "TopDownOcclusion::PadCB");

	CompileShaders();
}

void TopDownOcclusion::CompileShaders()
{
	if (heightVS && heightPS && padCS)
		return;

	auto device = globals::d3d::device;

	if (!heightVS) {
		heightVSBlob.attach(CompileWithBlob(L"Data\\Shaders\\TopDownOcclusion\\ObjectCapture.hlsl", "vs_5_0"));
		if (heightVSBlob)
			device->CreateVertexShader(heightVSBlob->GetBufferPointer(), heightVSBlob->GetBufferSize(), nullptr, &heightVS);
	}

	if (!heightPS) {
		winrt::com_ptr<ID3DBlob> ps;
		ps.attach(CompileWithBlob(L"Data\\Shaders\\TopDownOcclusion\\ObjectCapture.hlsl", "ps_5_0"));
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
	if (grassSurfacePS) {
		grassSurfacePS->Release();
		grassSurfacePS = nullptr;
	}
	heightVSBlob = nullptr;
	grassSurfaceAlphaVS = nullptr;
	grassSurfaceAlphaPS = nullptr;
	grassSurfaceAlphaVSBlob = nullptr;

	Invalidate();
	inputLayouts.clear();
	grassAlphaInputLayouts.clear();
	CompileShaders();
}

void TopDownOcclusion::Invalidate()
{
	captured.clear();
	grassSurfaceQuadrants.clear();
	grassSurfaceValid = false;
	++capturedGeometryRevision;
	capturedWorldspace = nullptr;
	capturedGeometryValid = false;
	renderCacheValid = false;
}

ID3D11ShaderResourceView* TopDownOcclusion::GetHighSRV() const
{
	return heightMapHigh ? heightMapHigh->srv.get() : nullptr;
}

ID3D11ShaderResourceView* TopDownOcclusion::GetLowSRV() const
{
	return heightMapLow ? heightMapLow->srv.get() : nullptr;
}

TopDownOcclusion::HeightCB TopDownOcclusion::MakeHeightConstants(const CapturedGeometry& entry) const
{
	const auto& transform = entry.world;
	const auto& rotate = transform.rotate;
	const float scale = transform.scale;
	HeightCB data{};
	data.worldRow0 = { rotate.entry[0][0] * scale, rotate.entry[0][1] * scale, rotate.entry[0][2] * scale, transform.translate.x };
	data.worldRow1 = { rotate.entry[1][0] * scale, rotate.entry[1][1] * scale, rotate.entry[1][2] * scale, transform.translate.y };
	data.worldRow2 = { rotate.entry[2][0] * scale, rotate.entry[2][1] * scale, rotate.entry[2][2] * scale, transform.translate.z };
	data.windowCentre = windowCentre;
	data.halfExtent = halfExtent;
	return data;
}

ID3D11InputLayout* TopDownOcclusion::GetInputLayout(const RE::BSGraphics::VertexDesc& a_desc)
{
	uint64_t descKey;
	std::memcpy(&descKey, &a_desc, sizeof(descKey));

	auto entry = inputLayouts.find(descKey);
	if (entry != inputLayouts.end())
		return entry->second.get();

	auto& layout = inputLayouts[descKey];

	const D3D11_INPUT_ELEMENT_DESC element{
		"POSITION", 0,
		GetVertexPositionFormat(a_desc),
		0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
	};

	if (heightVSBlob)
		globals::d3d::device->CreateInputLayout(&element, 1, heightVSBlob->GetBufferPointer(), heightVSBlob->GetBufferSize(), layout.put());

	// A null entry stays cached, so an unsupported descriptor is only attempted once.
	return layout.get();
}

ID3D11InputLayout* TopDownOcclusion::GetGrassAlphaInputLayout(const RE::BSGraphics::VertexDesc& desc)
{
	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_UV))
		return nullptr;
	if (!grassSurfaceAlphaVS) {
		const D3D_SHADER_MACRO defines[] = { { "GRASS_SURFACE_ALPHA_TEST", "1" }, { nullptr, nullptr } };
		grassSurfaceAlphaVSBlob.attach(CompileWithBlob(L"Data\\Shaders\\TopDownOcclusion\\ObjectCapture.hlsl", "vs_5_0", defines));
		if (!grassSurfaceAlphaVSBlob)
			return nullptr;
		DX::ThrowIfFailed(globals::d3d::device->CreateVertexShader(grassSurfaceAlphaVSBlob->GetBufferPointer(), grassSurfaceAlphaVSBlob->GetBufferSize(), nullptr, grassSurfaceAlphaVS.put()));
	}

	uint64_t key;
	std::memcpy(&key, &desc, sizeof(key));
	if (auto found = grassAlphaInputLayouts.find(key); found != grassAlphaInputLayouts.end())
		return found->second.get();

	// Geometry without vertex colour uses an in-bounds placeholder; its alpha multiplier is disabled.
	const UINT colorOffset = desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS) ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR) : 0u;
	const D3D11_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, GetVertexPositionFormat(desc), 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, colorOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	auto& layout = grassAlphaInputLayouts[key];
	DX::ThrowIfFailed(globals::d3d::device->CreateInputLayout(elements, 3, grassSurfaceAlphaVSBlob->GetBufferPointer(), grassSurfaceAlphaVSBlob->GetBufferSize(), layout.put()));
	return layout.get();
}

void TopDownOcclusion::CollectFrom(RE::NiAVObject* a_object, bool allowGrass)
{
	if (!a_object || a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
		return;

	if (auto* geometry = a_object->AsGeometry()) {
		auto* triShape = geometry->AsTriShape();
		if (!triShape || geometry->worldBound.radius <= MinOccluderRadius)
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

				if (value & (static_cast<int32_t>(RE::BSXFlags::Flag::kAnimated) |
								static_cast<int32_t>(RE::BSXFlags::Flag::kRagdoll) |
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
		capturedGeometry.vertexDesc = desc;
		ResolveGrassSurface(capturedGeometry, allowGrass);
		AddGrassSurfaceBounds(capturedGeometry);
		captured.push_back(std::move(capturedGeometry));
		return;
	}

	if (auto* node = a_object->AsNode()) {
		for (auto& child : node->GetChildren())
			CollectFrom(child.get(), allowGrass);
	}
}

void TopDownOcclusion::GatherGeometry()
{
	captured.clear();
	grassSurfaceQuadrants.clear();
	grassSurfaceValid = false;
	renderCacheValid = false;
	++capturedGeometryRevision;

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

		CollectFrom(a_ref->Get3D(), a_ref->GetBaseObject() && a_ref->GetBaseObject()->GetFormType() == RE::FormType::Static);
		return RE::BSContainer::ForEachResult::kContinue;
	});

	FilterGrassSurfaceBlockers();

	// The height blend is order-independent. Group layouts so Render avoids rebinding them for every geometry.
	std::ranges::sort(captured, [](const CapturedGeometry& lhs, const CapturedGeometry& rhs) {
		return std::less<>{}(lhs.inputLayout, rhs.inputLayout);
	});
}

bool TopDownOcclusion::CanReuseRenderedMaps() const
{
	return renderCacheValid && renderCacheState == GetRenderCacheState();
}

TopDownOcclusion::RenderCacheState TopDownOcclusion::GetRenderCacheState() const
{
	return { windowCentre, halfExtent, paddingWorld, mapDim, snapDim, capturedGeometryRevision };
}

void TopDownOcclusion::CommitRenderedMaps()
{
	renderCacheState = GetRenderCacheState();
	renderCacheValid = true;
}

void TopDownOcclusion::Render()
{
	if (!heightMapHigh || !heightMapLow || !heightVS || !heightPS)
		return;

	if (Util::IsInterior()) {
		// Exterior references may change while an interior is loaded.
		Invalidate();
		return;
	}

	auto context = globals::d3d::context;
	const auto tes = globals::game::tes;
	const auto worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	if (worldspace != capturedWorldspace) {
		// Worldspaces can share coordinates but not captured geometry.
		Invalidate();
		capturedWorldspace = worldspace;
	}

	// Snap the window to the texel grid so it jumps in whole texels instead of sliding and re-quantising silhouette edges each frame.
	// Snapping to the coarsest consumer (snapDim) keeps both maps stable.
	const auto& eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float texel = halfExtent * 2.0f / snapDim;
	windowCentre = {
		std::floor(eye.x / texel) * texel,
		std::floor(eye.y / texel) * texel
	};

	// Cell attachment changes invalidate the capture even while the player stands still.
	uint64_t cellStamp = PGrassCommon::GrassHashOffsetBasis;
	const auto* cells = tes ? tes->gridCells : nullptr;
	if (cells) {
		for (uint32_t i = 0; i < cells->length * cells->length; ++i) {
			const auto* cell = cells->cells[i];
			PGrassCommon::GrassHashValue(cellStamp, reinterpret_cast<uintptr_t>(cell));
			if (cell) {
				PGrassCommon::GrassHashValue(cellStamp, reinterpret_cast<uintptr_t>(cell->GetRuntimeData().loadedData));
				PGrassCommon::GrassHashValue(cellStamp, cell->IsAttached());
			}
		}
	}
	const float captureRefreshDistance = std::max(texel, halfExtent * 0.25f);
	if (!capturedGeometryValid || loadedCellStamp != cellStamp || std::abs(windowCentre.x - capturedCentre.x) >= captureRefreshDistance || std::abs(windowCentre.y - capturedCentre.y) >= captureRefreshDistance) {
		GatherGeometry();
		loadedCellStamp = cellStamp;
		capturedCentre = windowCentre;
		capturedGeometryValid = true;
	}

	// Reuse maps while their projection and captured geometry are unchanged.
	if (CanReuseRenderedMaps())
		return;

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

		const auto data = MakeHeightConstants(entry);
		heightCB->Update(data);

		context->DrawIndexed(entry.indexCount, 0, 0);
		lastDrawCount++;
	}

	ID3D11RenderTargetView* nullRTVs[2] = { nullptr, nullptr };
	context->OMSetRenderTargets(2, nullRTVs, nullptr);

	RenderGrassSurfaces(context);
	const bool paddingComplete = PadMaps(context);

	context->OMSetRenderTargets(8, previousRTVs, previousDSV);
	for (auto* view : previousRTVs) {
		if (view)
			view->Release();
	}
	if (previousDSV)
		previousDSV->Release();

	if (previousViewportCount)
		context->RSSetViewports(previousViewportCount, &previousViewport);

	if (paddingComplete)
		CommitRenderedMaps();
}

bool TopDownOcclusion::PadMaps(ID3D11DeviceContext* context)
{
	// Convert world-space padding to texels. Skip padding if less than one texel.
	const float texel = halfExtent * 2.0f / mapDim;
	const int radius = static_cast<int>(std::lround(paddingWorld / texel));
	if (radius <= 0)
		return true;

	if (!padCS || !heightMapTmp || !heightMapLowTmp || !padCB)
		return false;

	context->CSSetShader(padCS, nullptr, 0);
	auto* cbuf = padCB->CB();
	context->CSSetConstantBuffers(0, 1, &cbuf);
	const UINT groups = (mapDim + 7) / 8;

	ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };

	// Horizontal pass: heightMap/Low (SRV) -> Tmp (UAV).
	PadCB h{ 1, 0, radius, 0, mapDim, mapDim, 0, 0 };
	padCB->Update(h);
	ID3D11ShaderResourceView* srvH[2] = { heightMapHigh->srv.get(), heightMapLow->srv.get() };
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
	ID3D11UnorderedAccessView* uavV[2] = { heightMapHigh->uav.get(), heightMapLow->uav.get() };
	context->CSSetShaderResources(0, 2, srvV);
	context->CSSetUnorderedAccessViews(0, 2, uavV, nullptr);
	context->Dispatch(groups, groups, 1);
	context->CSSetShaderResources(0, 2, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);
	return true;
}
