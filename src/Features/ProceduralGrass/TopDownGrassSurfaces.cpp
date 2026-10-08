#include "TopDownOcclusion.h"

#include "Features/ProceduralGrass.h"
#include "Globals.h"
#include "Utils/D3D.h"

namespace
{
	// Dedicated extra data leaves BSX and material flags available to their existing consumers.
	const RE::BSFixedString SurfaceMarker = "CommunityShaders.ProceduralGrass.Surface";
	const RE::BSFixedString SurfaceType = "CommunityShaders.ProceduralGrass.Type";

	struct SurfaceGridBounds
	{
		int32_t minX, minY, maxX, maxY;
	};

	SurfaceGridBounds GetSurfaceGridBounds(const RE::NiBound& bound, float2 centre, float extent, float spacing, float padding = 0.0f)
	{
		return {
			static_cast<int32_t>(std::floor(std::max(bound.center.x - bound.radius - padding, centre.x - extent) / spacing)),
			static_cast<int32_t>(std::floor(std::max(bound.center.y - bound.radius - padding, centre.y - extent) / spacing)),
			static_cast<int32_t>(std::floor(std::min(bound.center.x + bound.radius + padding, centre.x + extent) / spacing)),
			static_cast<int32_t>(std::floor(std::min(bound.center.y + bound.radius + padding, centre.y + extent) / spacing))
		};
	}
}

void TopDownOcclusion::ResolveGrassSurface(CapturedGeometry& entry, bool allowGrass) const
{
	const auto& grass = globals::features::proceduralGrass;
	if (!grass.settings.objectGrassEnabled)
		return;

	const auto& runtime = entry.geometry->GetGeometryRuntimeData();
	auto* property = runtime.shaderProperty.get();
	if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
		return;
	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	if (property->flags.any(kSkinned, kDecal, kDynamicDecal))
		return;
	if (runtime.alphaProperty && runtime.alphaProperty->GetRTTI() == globals::rtti::NiAlphaPropertyRTTI.get()) {
		const auto* alpha = static_cast<const RE::NiAlphaProperty*>(runtime.alphaProperty.get());
		if (alpha->GetAlphaBlending() && !alpha->GetAlphaTesting())
			return;
		if (alpha->GetAlphaTesting()) {
			auto* texture = property->GetBaseTexture();
			auto* rendererTexture = texture ? texture->rendererTexture : nullptr;
			auto* material = property->GetBaseMaterial();
			if (!rendererTexture || !rendererTexture->resourceView || !material || material->GetType() != RE::BSShaderMaterial::Type::kLighting)
				return;
			const auto& desc = entry.vertexDesc;
			if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_UV))
				return;

			const auto* lightingMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
			const bool vertexAlpha = property->flags.all(kVertexAlpha, kVertexColors) && !property->flags.any(kTreeAnim) && desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS);
			entry.grassAlphaTexture.copy_from(rendererTexture->resourceView);
			entry.grassAlphaParams = { alpha->alphaThreshold / 255.0f, lightingMaterial->materialAlpha, vertexAlpha ? 1.0f : 0.0f, 0.0f };
			entry.grassUVTransform = { material->texCoordScale[0].x, material->texCoordScale[0].y, material->texCoordOffset[0].x, material->texCoordOffset[0].y };
			entry.grassAlphaSampler = static_cast<uint8_t>(std::clamp(lightingMaterial->textureClampMode, 0, 3));
			// Cutout foliage contributes blockers only, even if its texture or parent is marked for grass.
			entry.grassSurfaceCaptured = true;
			return;
		}
	}

	entry.grassSurfaceCaptured = true;
	// Keep animated and vertex-alpha materials as blockers.
	if (!allowGrass || property->flags.any(kTreeAnim, kVertexAlpha))
		return;

	ProceduralGrass::Settings::ObjectGrassRule rule;
	bool enabled = false;
	if (auto* texture = property->GetBaseTexture()) {
		if (const auto* match = grass.GetObjectGrassRule(texture->name.c_str())) {
			rule = *match;
			enabled = true;
		}
	}

	bool foundMarker = false, foundType = false;
	static const REL::Relocation<const RE::NiRTTI*> stringRTTI{ RE::NiStringExtraData::Ni_RTTI };
	for (const RE::NiAVObject* node = entry.geometry.get(); node; node = node->parent) {
		if (!foundMarker) {
			if (const auto* extra = node->GetExtraData(SurfaceMarker); extra && extra->GetRTTI() == globals::rtti::NiIntegerExtraDataRTTI.get()) {
				enabled = static_cast<const RE::NiIntegerExtraData*>(extra)->value == 1;
				foundMarker = true;
			}
		}
		if (!foundType) {
			if (const auto* extra = node->GetExtraData(SurfaceType); extra && extra->GetRTTI() == stringRTTI.get()) {
				const auto* value = static_cast<const RE::NiStringExtraData*>(extra)->value;
				rule.LandTexture = value ? value : "";
				rule.Variant = 0;
				foundType = true;
			}
		}
		if (foundMarker && foundType)
			break;
	}
	if (enabled && std::isfinite(rule.Density)) {
		entry.grassType = grass.GetObjectGrassType(rule);
		entry.grassDensity = std::clamp(rule.Density, 0.0f, 1.0f);
	}
}

void TopDownOcclusion::AddGrassSurfaceBounds(const CapturedGeometry& entry)
{
	if (!entry.grassType || entry.grassDensity <= 0.0f)
		return;
	const auto& bound = entry.geometry->worldBound;
	// Cover candidate jitter and window movement without expanding the sampled surface itself.
	const auto bounds = GetSurfaceGridBounds(bound, windowCentre, halfExtent * 1.25f, 128.0f, 128.0f);
	for (int32_t y = bounds.minY; y <= bounds.maxY; ++y) {
		for (int32_t x = bounds.minX; x <= bounds.maxX; ++x) {
			const int32_t qx = static_cast<int32_t>(std::floor(x / 16.0));
			const int32_t qy = static_cast<int32_t>(std::floor(y / 16.0));
			auto& quadrant = grassSurfaceQuadrants[PGrassCommon::GrassQuadrantKey(qx, qy)];
			quadrant.x = qx;
			quadrant.y = qy;
			quadrant.rows[static_cast<uint32_t>(y) & 15u] |= static_cast<uint16_t>(1u << (static_cast<uint32_t>(x) & 15u));
			quadrant.minHeight = std::min(quadrant.minHeight, bound.center.z - bound.radius);
			quadrant.maxHeight = std::max(quadrant.maxHeight, bound.center.z + bound.radius);
		}
	}
}

void TopDownOcclusion::FilterGrassSurfaceBlockers()
{
	// Blockers outside growable quadrants still contribute to LAND occlusion.
	if (grassSurfaceQuadrants.empty())
		return;

	for (auto& entry : captured) {
		if (!entry.grassSurfaceCaptured || entry.grassType != 0u)
			continue;
		const auto bounds = GetSurfaceGridBounds(entry.geometry->worldBound, windowCentre, halfExtent * 1.25f, 2048.0f);
		bool overlaps = false;
		for (int32_t y = bounds.minY; y <= bounds.maxY && !overlaps; ++y)
			for (int32_t x = bounds.minX; x <= bounds.maxX && !overlaps; ++x)
				overlaps = grassSurfaceQuadrants.contains(PGrassCommon::GrassQuadrantKey(x, y));
		entry.grassSurfaceCaptured = overlaps;
	}
}

ID3D11ShaderResourceView* TopDownOcclusion::GetGrassSurfaceSRV() const
{
	return grassSurfaceValid && grassSurfaceMap ? grassSurfaceMap->srv.get() : nullptr;
}

const TopDownOcclusion::GrassSurfaceQuadrant* TopDownOcclusion::GetGrassSurfaceQuadrant(int32_t x, int32_t y) const
{
	if (!GetGrassSurfaceSRV())
		return nullptr;
	const auto found = grassSurfaceQuadrants.find(PGrassCommon::GrassQuadrantKey(x, y));
	return found != grassSurfaceQuadrants.end() ? &found->second : nullptr;
}

ID3D11SamplerState* TopDownOcclusion::GetGrassAlphaSampler(uint8_t mode)
{
	auto& sampler = grassAlphaSamplers[mode];
	if (!sampler) {
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = mode & 2u ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressV = mode & 1u ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxAnisotropy = 1;
		desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, sampler.put()));
	}
	return sampler.get();
}

void TopDownOcclusion::CreateGrassSurfaceResources()
{
	if (grassSurfaceMap)
		return;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = desc.Height = mapDim;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	grassSurfaceMap = new Texture2D(desc, "PGrass::ObjectSurfaces");

	D3D11_RENDER_TARGET_VIEW_DESC rtv{};
	rtv.Format = desc.Format;
	rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	grassSurfaceMap->CreateRTV(rtv);
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	grassSurfaceMap->CreateSRV(srv);

	desc.Format = DXGI_FORMAT_D32_FLOAT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	grassSurfaceDepth = new Texture2D(desc, "PGrass::ObjectSurfaceDepth");
	D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
	dsv.Format = desc.Format;
	dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	grassSurfaceDepth->CreateDSV(dsv);

	D3D11_DEPTH_STENCIL_DESC depthState{};
	depthState.DepthEnable = TRUE;
	depthState.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthState.DepthFunc = D3D11_COMPARISON_LESS;
	DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthState, grassSurfaceDSS.put()));
}

void TopDownOcclusion::RenderGrassSurfaces(ID3D11DeviceContext* context)
{
	grassSurfaceValid = false;
	if (grassSurfaceQuadrants.empty())
		return;
	if (!grassSurfacePS)
		grassSurfacePS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\TopDownOcclusion\\ObjectCapture.hlsl", { { "GRASS_SURFACE", "1" } }, "ps_5_0"));
	if (!grassSurfacePS)
		return;

	const bool hasAlpha = std::ranges::any_of(captured, [](const CapturedGeometry& entry) {
		return entry.grassSurfaceCaptured && entry.grassAlphaTexture;
	});
	if (hasAlpha && !grassSurfaceAlphaPS)
		grassSurfaceAlphaPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\TopDownOcclusion\\ObjectCapture.hlsl", { { "GRASS_SURFACE", "1" }, { "GRASS_SURFACE_ALPHA_TEST", "1" } }, "ps_5_0")));
	if (hasAlpha && !grassSurfaceAlphaPS)
		return;

	CreateGrassSurfaceResources();

	float minZ = FLT_MAX, maxZ = -FLT_MAX;
	for (const auto& entry : captured) {
		const auto& bound = entry.geometry->worldBound;
		minZ = std::min(minZ, bound.center.z - bound.radius);
		maxZ = std::max(maxZ, bound.center.z + bound.radius);
	}
	const float inverseHeightRange = 1.0f / std::max(maxZ - minZ + 2.0f, 1.0f);

	const float empty[4]{};
	context->ClearRenderTargetView(grassSurfaceMap->rtv.get(), empty);
	context->ClearDepthStencilView(grassSurfaceDepth->dsv.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
	auto* rtv = grassSurfaceMap->rtv.get();
	context->OMSetRenderTargets(1, &rtv, grassSurfaceDepth->dsv.get());
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(grassSurfaceDSS.get(), 0);
	context->VSSetShader(heightVS, nullptr, 0);
	context->PSSetShader(grassSurfacePS, nullptr, 0);
	auto* constants = heightCB->CB();
	context->PSSetConstantBuffers(0, 1, &constants);

	winrt::com_ptr<ID3D11ShaderResourceView> previousAlphaTexture;
	winrt::com_ptr<ID3D11SamplerState> previousAlphaSampler;
	if (hasAlpha) {
		context->PSGetShaderResources(0, 1, previousAlphaTexture.put());
		context->PSGetSamplers(0, 1, previousAlphaSampler.put());
	}

	bool currentAlpha = false;
	for (const auto& entry : captured) {
		if (!entry.grassSurfaceCaptured)
			continue;
		const bool alpha = entry.grassAlphaTexture != nullptr;
		auto* layout = alpha ? GetGrassAlphaInputLayout(entry.vertexDesc) : entry.inputLayout;
		if (!layout)
			continue;

		if (alpha != currentAlpha) {
			context->VSSetShader(alpha ? grassSurfaceAlphaVS.get() : heightVS, nullptr, 0);
			context->PSSetShader(alpha ? grassSurfaceAlphaPS.get() : grassSurfacePS, nullptr, 0);
			currentAlpha = alpha;
		}
		if (alpha) {
			auto* texture = entry.grassAlphaTexture.get();
			auto* alphaSampler = GetGrassAlphaSampler(entry.grassAlphaSampler);
			context->PSSetShaderResources(0, 1, &texture);
			context->PSSetSamplers(0, 1, &alphaSampler);
		}

		const UINT offset = 0, stride = entry.stride;
		auto* vertices = entry.vertexBuffer.get();
		context->IASetInputLayout(layout);
		context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
		context->IASetIndexBuffer(entry.indexBuffer.get(), DXGI_FORMAT_R16_UINT, 0);
		auto data = MakeHeightConstants(entry);
		data.grassSurfaceParams = { static_cast<float>(entry.grassType), entry.grassDensity, minZ - 1.0f, inverseHeightRange };
		data.grassAlphaParams = entry.grassAlphaParams;
		data.grassUVTransform = entry.grassUVTransform;
		heightCB->Update(data);
		context->DrawIndexed(entry.indexCount, 0, 0);
	}

	if (hasAlpha) {
		auto* texture = previousAlphaTexture.get();
		auto* sampler = previousAlphaSampler.get();
		context->PSSetShaderResources(0, 1, &texture);
		context->PSSetSamplers(0, 1, &sampler);
	}
	context->OMSetRenderTargets(0, nullptr, nullptr);
	grassSurfaceValid = true;
}
