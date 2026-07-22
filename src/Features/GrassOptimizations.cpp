#include "GrassOptimizations.h"
#include "GrassLighting.h"
#include "TerrainBlending.h"  // loaded state selects the scene depth SRV's format

#define I18N_KEY_PREFIX "feature.grass_optimizations."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassOptimizations::Settings,
	MinPixelSize,
	FullDetailPixelSize,
	MinDensity,
	MeshCostBias,
	InvisibleFadeCull,
	MaxDistanceOverride,
	EnableOcclusionCulling,
	EnableMeshLOD,
	MeshLODPixelSize,
	MeshLODBandPixels)

void GrassOptimizations::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassOptimizations::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassOptimizations::RestoreDefaultSettings()
{
	settings = {};
}

void GrassOptimizations::DrawSettings()
{
	ImGui::SeparatorText(T(TKEY("culling"), "Culling & LOD"));

	ImGui::SliderFloat(T(TKEY("full_detail_pixel_size"), "Full-Detail Pixel Size"), &settings.FullDetailPixelSize, 4.0f, 128.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("full_detail_pixel_size_tooltip"),
							  "Projected-size LOD: clumps whose on-screen radius is above this render at full density. Below it, density is thinned by screen size down to Minimum Density, then culled at Min Pixel Size. Lower = grass thins closer to the camera (less overdraw, more visible thinning)."));
	}

	ImGui::SliderFloat(T(TKEY("min_pixel_size"), "Min Pixel Size"), &settings.MinPixelSize, 1.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_pixel_size_tooltip"),
							  "Projected-size LOD, cull level: clumps whose on-screen radius is below this many pixels are dropped entirely. Higher culls more distant grass and reduces overdraw."));
	}

	ImGui::SliderFloat(T(TKEY("min_density"), "Minimum Density"), &settings.MinDensity, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_density_tooltip"),
							  "Fraction of grass kept at the smallest (Min Pixel Size) LOD level before culling."));
	}

	ImGui::SliderFloat(T(TKEY("mesh_cost_bias"), "Mesh Cost Bias"), &settings.MeshCostBias, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_cost_bias_tooltip"),
							  "Blends in a per-mesh cost weighting (sqrt(triangles/6)) that culls heavier grass meshes sooner. 0 = the pixel-size and distance settings above are LITERAL and identical for every grass type. 1 = full weighting, which can make a '4 px' setting behave like 9-23 px and shorten Max Distance by 2-6x depending on the mesh."));
	}

	ImGui::SliderFloat(T(TKEY("max_distance_override"), "Max Distance Override"), &settings.MaxDistanceOverride, 0.0f, 100000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("max_distance_override_tooltip"),
							  "Max grass render distance in units. 0 = use the game's INI cap (fGrassStartFadeDistance + fGrassFadeRange). Raise to render grass further than the vanilla cap allows."));
	}

	ImGui::SliderFloat(T(TKEY("invisible_fade_cull"), "Invisible Fade Cull"), &settings.InvisibleFadeCull, 0.0f, 0.5f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("invisible_fade_cull_tooltip"),
							  "Skip drawing grass whose fade is below this. Such grass is discarded by the alpha test anyway, so values up to the game's alpha-test threshold (~0.2-0.3) are visually lossless and remove invisible overdraw at the far fade edge and during cell fade-in. 0 = only exactly-invisible grass."));
	}

	ImGui::Checkbox(T(TKEY("occlusion_culling"), "Occlusion Culling"), &settings.EnableOcclusionCulling);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("occlusion_culling_tooltip"),
							  "Skips grass hidden behind rocks, buildings and actors, tested against a coarse max-depth reduction of the scene depth. The GPU already rejects these pixels, but only after running the vertex shader for every blade; this removes that work too. Conservative — it can only miss hidden grass, never cull visible grass. Terrain is not an occluder when Terrain Blending is enabled."));
	}

	ImGui::SeparatorText(T(TKEY("mesh_lod"), "Mesh LOD (experimental)"));

	ImGui::Checkbox(T(TKEY("enable_mesh_lod"), "Enable Mesh LOD"), &settings.EnableMeshLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_mesh_lod_tooltip"),
							  "Swap distant grass clumps to a lower-poly LOD mesh to cut overdraw. Requires a LOD .nif per grass type at meshes\\LOD\\Grass\\<source-mesh-name>.nif, authored in the same local space and vertex format as the source grass. Grass with no LOD mesh keeps its full mesh."));
	}

	ImGui::SliderFloat(T(TKEY("mesh_lod_pixel_size"), "Mesh LOD Pixel Size"), &settings.MeshLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_lod_pixel_size_tooltip"),
							  "Clumps whose on-screen radius is below this (but above Min Pixel Size) use the LOD mesh. Higher = swap to LOD nearer the camera."));
	}

	ImGui::SliderFloat(T(TKEY("mesh_lod_band"), "Mesh LOD Transition Band"), &settings.MeshLODBandPixels, 0.0f, 16.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_lod_band_tooltip"),
							  "Width of the dithered swap band centred on Mesh LOD Pixel Size. Instances inside it are randomly assigned to the full or LOD mesh, so a clump converts gradually instead of every blade popping at once. 0 = hard swap."));
	}
}

void GrassOptimizations::PostPostLoad()
{
	Hooks::Install();
}

bool GrassOptimizations::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Grass:
		return true;
	default:
		return false;
	}
}

void GrassOptimizations::ComputeFrustumPlanes(RE::NiFrustumPlanes& out, const RE::NiFrustum& viewFrustum, const RE::NiTransform& transform)
{
	const __m128 fwd = _mm_set_ps(0.0f, transform.rotate.entry[2][0], transform.rotate.entry[1][0], transform.rotate.entry[0][0]);
	const __m128 col1 = _mm_set_ps(0.0f, transform.rotate.entry[2][1], transform.rotate.entry[1][1], transform.rotate.entry[0][1]);
	const __m128 col2 = _mm_set_ps(0.0f, transform.rotate.entry[2][2], transform.rotate.entry[1][2], transform.rotate.entry[0][2]);
	const __m128 trans = _mm_set_ps(0.0f, transform.translate.z, transform.translate.y, transform.translate.x);

	const __m128 nearPt = _mm_add_ps(trans, _mm_mul_ps(fwd, _mm_set1_ps(viewFrustum.fNear)));
	const __m128 farPt = _mm_add_ps(trans, _mm_mul_ps(fwd, _mm_set1_ps(viewFrustum.fFar)));

	auto MakePlane = [&](int idx, __m128 normal, __m128 point) {
		alignas(16) float n[4];
		_mm_store_ps(n, normal);
		out.cullingPlanes[idx].normal = { n[0], n[1], n[2] };
		out.cullingPlanes[idx].constant = _mm_cvtss_f32(_mm_dp_ps(normal, point, 0x71));
	};

	MakePlane(0, fwd, nearPt);
	const __m128 negFwd = _mm_xor_ps(fwd, _mm_set1_ps(-0.0f));
	MakePlane(1, negFwd, farPt);

	if (viewFrustum.bOrtho) {
		__m128 leftVec = col2;
		MakePlane(2, leftVec, _mm_add_ps(trans, _mm_mul_ps(leftVec, _mm_set1_ps(viewFrustum.fLeft))));
		__m128 rightVec = _mm_xor_ps(col2, _mm_set1_ps(-0.0f));
		MakePlane(3, rightVec, _mm_add_ps(trans, _mm_mul_ps(rightVec, _mm_set1_ps(viewFrustum.fRight))));
		__m128 upVec = col1;
		MakePlane(4, upVec, _mm_add_ps(trans, _mm_mul_ps(upVec, _mm_set1_ps(viewFrustum.fTop))));
		__m128 botVec = _mm_xor_ps(col1, _mm_set1_ps(-0.0f));
		MakePlane(5, botVec, _mm_add_ps(trans, _mm_mul_ps(botVec, _mm_set1_ps(viewFrustum.fBottom))));
	} else {
		// Set: s = 1/sqrt(slope²+1); n = fwd*(±slope*s) + axis*(±s)
		auto sidePlane = [&](int idx, __m128 axis, float slope, float fwdSign, float axisSign) {
			const float s = 1.0f / std::sqrt(slope * slope + 1.0f);
			__m128 n = _mm_add_ps(
				_mm_mul_ps(fwd, _mm_set1_ps(fwdSign * slope * s)),
				_mm_mul_ps(axis, _mm_set1_ps(axisSign * s)));
			MakePlane(idx, n, trans);
		};

		sidePlane(2, col2, viewFrustum.fLeft, -1.0f, +1.0f);
		sidePlane(3, col2, viewFrustum.fRight, +1.0f, -1.0f);
		sidePlane(4, col1, viewFrustum.fTop, +1.0f, -1.0f);
		sidePlane(5, col1, viewFrustum.fBottom, -1.0f, +1.0f);
	}

	out.activePlanes = RE::NiFrustumPlanes::ActivePlane(0x3F);

	constexpr float kGuard = 128.0f;
	out.cullingPlanes[2].constant -= kGuard;
	out.cullingPlanes[3].constant -= kGuard;
	out.cullingPlanes[4].constant -= kGuard;
	out.cullingPlanes[5].constant -= kGuard;
}

void GrassOptimizations::UpdateGrass()
{
	std::vector<PendingCapture> caps;
	std::vector<RE::BSMultiStreamInstanceTriShape*> rems;
	{
		std::scoped_lock lk(pendingMutex);
		caps.swap(pendingCaptures);
		rems.swap(pendingRemoves);
	}

	std::scoped_lock blk(bucketMutex);
	auto* device = globals::d3d::device;
	auto* ctx = globals::d3d::context;

	if (!cullInit) {
		cullInit = true;
		InitCullResources();
	}
	if (!cullCS || !ctx1)
		return;

	timeAccum += globals::game::smState->timerValues[1];
	prevTimeBase = timeBase;
	timeBase = globals::game::smState->timerValues[4] * 0.0016666667f * 6.2831802f;

	if (fadeInTimeRcp == 0.0f) {
		const float t = RE::GetINISetting("fGrassFadeInTime:Grass")->GetFloat();
		fadeInTimeRcp = t > 0.0f ? 1.0f / t : 1e6f;
	}
	if (vanillaMaxDistance == 0.0f) {
		grassStartFadeDistance = RE::GetINISetting("fGrassStartFadeDistance:Grass")->GetFloat();
		vanillaMaxDistance = grassStartFadeDistance + RE::GetINISetting("fGrassFadeRange:Grass")->GetFloat();
	}

	// Effective max grass render distance: explicit override if set, else the vanilla INI cap.
	maxGrassDistance = settings.MaxDistanceOverride > 0.0f ? settings.MaxDistanceOverride : vanillaMaxDistance;
	maxDistSq = maxGrassDistance * maxGrassDistance;

	{
		const float threshold = globals::features::grassLighting.settings.ComplexGrassThreshold;
		if (threshold != cachedComplexThreshold) {
			cachedComplexThreshold = threshold;
			complexCache.clear();
			for (auto& [key, b] : buckets) {
				b.isComplex = DetectComplexGrass(key.tex, device, ctx);
				b.dirty = true;
			}
		}
	}

	ApplyRemovals(rems);
	ApplyCaptures(caps);
	UploadDirtyBuckets(device, ctx);

	RE::NiCamera* cam = RE::Main::WorldRootCamera();
	if (!cam)
		return;

	RE::NiFrustumPlanes frustum{};
	ComputeFrustumPlanes(frustum, cam->GetRuntimeData2().viewFrustum, cam->world);
	const RE::NiPoint3 camPos = cam->world.translate;

	const auto [screenW, screenH] = globals::game::renderer->GetScreenSize();

	// Rebuilt before any bucket is culled; the depth copy already holds this frame's statics.
	BuildHiZ(device, ctx);

	{
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(cullParamsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			auto* cp = static_cast<CullParamsCB*>(m.pData);
			for (int i = 0; i < 6; ++i) {
				cp->frustumPlanes[i][0] = frustum.cullingPlanes[i].normal.x;
				cp->frustumPlanes[i][1] = frustum.cullingPlanes[i].normal.y;
				cp->frustumPlanes[i][2] = frustum.cullingPlanes[i].normal.z;
				cp->frustumPlanes[i][3] = frustum.cullingPlanes[i].constant;
			}

			cp->alphaParam1 = grassStartFadeDistance;
			cp->alphaParam2 = maxGrassDistance;
			cp->fadeNow = timeAccum;
			cp->fadeInTimeRcp = fadeInTimeRcp;
			cp->invisibleFadeCull = settings.InvisibleFadeCull;
			cp->meshLODPixelSize = settings.MeshLODPixelSize;
			cp->meshLODBandPx = std::max(0.0f, settings.MeshLODBandPixels);
			cp->hiZEnabled = hiZValid ? 1.0f : 0.0f;
			cp->hiZSizeX = (float)hiZWidth;
			cp->hiZSizeY = (float)hiZHeight;
			cp->hiZTexelPixels = (float)kHiZTileSize;
			// Only level 0 is trustworthy if the reduction chain failed to build.
			cp->hiZMipCount = (float)(hiZMipCS ? hiZMipCount : 1u);

			cp->maxDistSq = maxDistSq;
			// projected-size LOD — tunable in the feature settings
			cp->fullDetailPixelSize = settings.FullDetailPixelSize;
			cp->meshCostBias = settings.MeshCostBias;
			cp->lodMinKeep = settings.MinDensity;
			cp->lodFadeBand = 0.15f;
			const auto& vf = cam->GetRuntimeData2().viewFrustum;
			cp->projScale = screenH / (2.0f * std::abs(vf.fTop));
			cp->minPixelSize = settings.MinPixelSize;
			cp->edgeFadeStart = 0.85f;
			cp->collisionDistSq = 2048.0f * 2048.0f;
			ctx->Unmap(cullParamsCB, 0);
		}
	}

	// coarse per-bucket cull
	uint32_t visibleBuckets = 0;
	for (auto& [key, b] : buckets) {
		b.cullSlot = UINT32_MAX;
		if (!b.totalInstances || !b.instanceSRV) {
			b.cullVisible = false;
			continue;
		}
		if (!b.coarseValid)
			UpdateCoarseBounds(b);

		// Cheap first test: the union box over every slice. If that misses, nothing in the bucket
		// can be visible.
		b.cullVisible = AabbVisible(frustum, b.coarseMin, b.coarseMax);

		// The union spans every loaded cell of this mesh, so with the camera inside it it is
		// almost always "visible" — which made this early-out useless and left us issuing a draw
		// per bucket every frame even with nothing on screen. Refine: the bucket is visible only
		// if at least one of its per-cell slices is.
		if (b.cullVisible && b.slices.size() > 1) {
			const float pad = b.clumpRadius + 64.0f;
			bool anySliceVisible = false;
			for (const auto& s : b.slices) {
				const RE::NiPoint3 mn{
					s.origin.x + s.localMin.x - pad,
					s.origin.y + s.localMin.y - pad,
					s.origin.z + s.localMin.z - pad
				};
				const RE::NiPoint3 mx{
					s.origin.x + s.localMax.x + pad,
					s.origin.y + s.localMax.y + pad,
					s.origin.z + s.localMax.z + pad
				};
				if (AabbVisible(frustum, mn, mx)) {
					anySliceVisible = true;
					break;
				}
			}
			b.cullVisible = anySliceVisible;
		}

		if (b.cullVisible) {
			// Allocated lazily and only for visible buckets that actually have an LOD mesh, so
			// toggling the setting takes effect without a reload and grass types with no authored
			// LOD .nif never pay for the second bin.
			b.lodActive = EnsureLODBin(b, device);
			++visibleBuckets;
		} else {
			b.lodActive = false;
		}
	}

	// one map fills every visible bucket's slot — replaces a Map/Unmap per bucket
	if (visibleBuckets && EnsureCullBucketCapacity(visibleBuckets, device)) {
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(cullBucketCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			auto* bytes = static_cast<uint8_t*>(m.pData);
			uint32_t slot = 0;
			for (auto& [key, b] : buckets) {
				if (!b.cullVisible)
					continue;
				b.cullSlot = slot;
				auto* cb = reinterpret_cast<CullBucketCB*>(bytes + (size_t)slot * kSlotBytes);
				cb->instanceCount = b.totalInstances;
				cb->wavePeriod = b.wavePeriod;
				cb->timeBase = timeBase;
				cb->prevTimeBase = prevTimeBase;
				cb->boundCenter[0] = b.boundCenter.x;
				cb->boundCenter[1] = b.boundCenter.y;
				cb->boundCenter[2] = b.boundCenter.z;
				cb->clumpRadius = b.clumpRadius;
				cb->distScale = b.distScale;
				// was never written — with WRITE_DISCARD the CS read whatever was in the slot, which
				// only stayed harmless because MeshCostBias defaults to 0 and weights it out
				cb->minPixelScale = b.minPixelScale;
				cb->isComplex = b.isComplex ? 1.0f : 0.0f;
				cb->lodEnabled = b.lodActive ? 1.0f : 0.0f;
				++slot;
			}
			ctx->Unmap(cullBucketCB, 0);
		}
	}

	ctx->CSSetShader(cullCS, nullptr, 0);
	ctx->CSSetConstantBuffers(0, 1, &cullParamsCB);
	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	ctx->CSSetConstantBuffers(12, 1, frameBuffers);

	for (auto& [key, b] : buckets)
		if (b.cullVisible)
			CullBucket(b, ctx);

	ID3D11UnorderedAccessView* nullUAVs[3] = {};
	ctx->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[2] = {};
	ctx->CSSetShaderResources(0, 2, nullSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);
}

void GrassOptimizations::ApplyRemovals(const std::vector<RE::BSMultiStreamInstanceTriShape*>& removes)
{
	if (removes.empty())
		return;

	std::unordered_set<RE::BSMultiStreamInstanceTriShape*> dead(removes.begin(), removes.end());

	// Both caches are keyed by shape pointer, so they must not outlive the shape: the allocator
	// will hand the same address to a future grass shape, which would then silently inherit this
	// one's mesh id — and with it the wrong LOD mesh. Lock order is always bucketMutex ->
	// meshPathMutex (the LoadGrassType hook only ever takes the latter).
	for (auto* s : removes)
		shapeMeshId.erase(s);
	{
		std::scoped_lock lk(meshPathMutex);
		for (auto* s : removes)
			shapeModelStem.erase(s);
	}

	for (auto it = buckets.begin(); it != buckets.end();) {
		auto& b = it->second;
		const size_t before = b.slices.size();

		std::erase_if(b.slices, [&](const BucketSlice& s) { return dead.count(s.shape) != 0; });

		if (b.slices.empty()) {
			RE::NiSourceTexture* tex = b.diffuseTex;
			b.Release();
			it = buckets.erase(it);

			bool texStillUsed = false;
			for (const auto& [k, other] : buckets) {
				if (other.diffuseTex == tex) {
					texStillUsed = true;
					break;
				}
			}
			if (!texStillUsed) {
				std::unique_lock lk(bucketKeysMutex);
				bucketKeys.erase(tex);
			}
			continue;
		}
		if (b.slices.size() != before) {
			b.dirty = true;
			b.coarseValid = false;
		}
		++it;
	}
}

void GrassOptimizations::ApplyCaptures(std::vector<PendingCapture>& captures)
{
	for (auto& pc : captures) {
		// One bucket per source mesh; unresolved meshes fall back to the old texture identity.
		const uint32_t meshId = ResolveMeshId(pc.shape);
		const BucketKey bk{ meshId, meshId ? nullptr : pc.diffuseTexture, pc.descVal };
		auto& b = buckets[bk];
		b.diffuseTex = pc.diffuseTexture;
		b.meshId = meshId;

		if (b.slices.empty() && b.totalInstances == 0)
			b.isComplex = DetectComplexGrass(pc.diffuseTexture, globals::d3d::device, globals::d3d::context);

		if (b.firstNewSlice == UINT32_MAX)
			b.firstNewSlice = (uint32_t)b.slices.size();

		{
			std::unique_lock lk(bucketKeysMutex);
			bucketKeys.insert(pc.diffuseTexture);
		}
		b.descVal = pc.descVal;
		if (!b.typeParamsValid) {
			CacheBucketTypeParams(b, pc.shape);
			b.isComplex = DetectComplexGrass(pc.diffuseTexture, globals::d3d::device, globals::d3d::context);
		}
		EnsureLODMesh(meshId);

		BucketSlice s;
		s.shape = pc.shape;
		s.count = pc.count;
		s.fadeStart = timeAccum;
		s.origin = pc.origin;
		s.localMin = pc.localMin;
		s.localMax = pc.localMax;
		s.data = std::move(pc.bytes);
		b.slices.push_back(std::move(s));
		b.coarseValid = false;
	}
}

void GrassOptimizations::UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	for (auto& [key, b] : buckets) {
		uint32_t total = 0;
		for (auto& s : b.slices)
			total += s.count;
		b.totalInstances = total;

		if (b.dirty) {
			RebuildBucket(b, device, ctx);
		} else if (b.firstNewSlice != UINT32_MAX) {
			if (b.totalInstances > b.capacityInstances)
				RebuildBucket(b, device, ctx);
			else
				AppendNewSlices(b, ctx);
		}
	}
}

bool GrassOptimizations::StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src,
	uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex)
{
	if (!shape || !src || !tex || !count || stride != 32) {
		logger::warn("[GRASS OPTIMIZATIONS] capture rejected: count={} stride={} desc={:016X} shape={:p}",
			count, stride, descVal, (void*)shape);
		return false;
	}

	PendingCapture pc;
	pc.shape = shape;
	pc.descVal = descVal;
	pc.diffuseTexture = tex;
	pc.count = count;
	pc.origin = shape->world.translate;
	pc.bytes.resize((size_t)count * 32);
	std::memcpy(pc.bytes.data(), src, pc.bytes.size());

	{
		using DirectX::PackedVector::XMConvertHalfToFloat;
		RE::NiPoint3 lmn{ FLT_MAX, FLT_MAX, FLT_MAX };
		RE::NiPoint3 lmx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
		const uint8_t* d = pc.bytes.data();
		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t* r = d + (size_t)i * 32;
			const float lx = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(r + 0));
			const float ly = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(r + 2));
			const float lz = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(r + 4));
			lmn.x = std::min(lmn.x, lx);
			lmn.y = std::min(lmn.y, ly);
			lmn.z = std::min(lmn.z, lz);
			lmx.x = std::max(lmx.x, lx);
			lmx.y = std::max(lmx.y, ly);
			lmx.z = std::max(lmx.z, lz);
		}
		pc.localMin = lmn;
		pc.localMax = lmx;
	}

	std::scoped_lock lk(pendingMutex);
	pendingCaptures.push_back(std::move(pc));
	return true;
}

void GrassOptimizations::CacheBucketTypeParams(GrassBucket& b, RE::BSMultiStreamInstanceTriShape* shape)
{
	if (b.typeParamsValid || !shape)
		return;

	if (auto* prop = static_cast<RE::BSGrassShaderProperty*>(
			shape->GetGeometryRuntimeData().shaderProperty.get()))
		b.wavePeriod = prop->wavePeriod;

	const auto& bound = shape->GetModelData().modelBound;
	b.boundCenter = bound.center;
	b.clumpRadius = bound.radius;

	const float tris = (float)shape->GetTrishapeRuntimeData().triangleCount;
	const float cost = std::max(1.0f, tris / 6.0f);
	const float w = std::sqrt(cost);
	b.distScale = 1.0f / w;
	b.minPixelScale = w;
	b.typeParamsValid = true;
}

void GrassOptimizations::RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	if (!EnsureBucketCapacity(bucket, bucket.totalInstances, device))
		return;
	if (!bucket.totalInstances)
		return;

	static thread_local std::vector<uint8_t> records;
	static thread_local std::vector<float> origins;
	records.clear();
	origins.clear();
	records.reserve((size_t)bucket.totalInstances * 32);
	origins.reserve((size_t)bucket.totalInstances * 4);

	uint32_t off = 0;
	for (auto& s : bucket.slices) {
		s.bufferOffset = off;
		records.insert(records.end(), s.data.begin(), s.data.end());
		for (uint32_t i = 0; i < s.count; ++i) {
			origins.push_back(s.origin.x);
			origins.push_back(s.origin.y);
			origins.push_back(s.origin.z);
			origins.push_back(s.fadeStart);
		}
		off += s.count;
	}

	const D3D11_BOX ibox{ 0, 0, 0, (UINT)records.size(), 1, 1 };
	ctx->UpdateSubresource(bucket.instanceBuf, 0, &ibox, records.data(), 0, 0);

	const D3D11_BOX obox{ 0, 0, 0, (UINT)(origins.size() * sizeof(float)), 1, 1 };
	ctx->UpdateSubresource(bucket.originBuf, 0, &obox, origins.data(), 0, 0);

	bucket.dirty = false;
	bucket.firstNewSlice = UINT32_MAX;
}

void GrassOptimizations::AppendNewSlices(GrassBucket& bucket, ID3D11DeviceContext* ctx)
{
	if (bucket.totalInstances > bucket.capacityInstances) {
		bucket.dirty = true;
		RebuildBucket(bucket, globals::d3d::device, ctx);
		return;
	}

	uint32_t prefix = 0;
	for (uint32_t i = 0; i < bucket.firstNewSlice; ++i) {
		const auto& s = bucket.slices[i];
		if (s.bufferOffset == UINT32_MAX || s.bufferOffset != prefix) {
			logger::warn("[GRASS OPTIMIZATIONS] append prefix mismatch slice={} stored={} expected={} — rebuilding",
				i, s.bufferOffset, prefix);
			bucket.dirty = true;
			RebuildBucket(bucket, globals::d3d::device, ctx);
			return;
		}
		prefix += s.count;
	}

	static thread_local std::vector<uint8_t> recTail;
	static thread_local std::vector<float> originTail;
	recTail.clear();
	originTail.clear();

	uint32_t tailOff = 0;
	for (uint32_t i = bucket.firstNewSlice; i < (uint32_t)bucket.slices.size(); ++i) {
		auto& s = bucket.slices[i];
		s.bufferOffset = prefix + tailOff;
		recTail.insert(recTail.end(), s.data.begin(), s.data.end());
		for (uint32_t j = 0; j < s.count; ++j) {
			originTail.push_back(s.origin.x);
			originTail.push_back(s.origin.y);
			originTail.push_back(s.origin.z);
			originTail.push_back(s.fadeStart);
		}
		tailOff += s.count;
	}

	if (!recTail.empty()) {
		const D3D11_BOX ibox{ prefix * 32, 0, 0, (prefix + tailOff) * 32, 1, 1 };
		ctx->UpdateSubresource(bucket.instanceBuf, 0, &ibox, recTail.data(), 0, 0);

		const D3D11_BOX obox{ prefix * 4 * (UINT)sizeof(float), 0, 0,
			(prefix + tailOff) * 4 * (UINT)sizeof(float), 1, 1 };
		ctx->UpdateSubresource(bucket.originBuf, 0, &obox, originTail.data(), 0, 0);
	}
	bucket.firstNewSlice = UINT32_MAX;
}

bool GrassOptimizations::EnsureBucketCapacity(GrassBucket& b, uint32_t needed, ID3D11Device* device)
{
	if (b.capacityInstances >= needed && b.instanceBuf)
		return true;

	uint32_t cap = b.capacityInstances ? b.capacityInstances : 4096;
	while (cap < needed)
		cap *= 2;

	b.ReleaseResources();

	// --- source instance data: raw, half-packed, local positions ---
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 32;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.instanceBuf)) || !b.instanceBuf) {
			logger::error("[GRASS OPTIMIZATIONS] instance buffer create failed bytes={}", bd.ByteWidth);
			b.capacityInstances = 0;
			return false;
		}
		Util::SetResourceName(b.instanceBuf, "GrassOptimizations::InstanceBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_R32_TYPELESS;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		sv.BufferEx.FirstElement = 0;
		sv.BufferEx.NumElements = cap * 8;
		sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		if (FAILED(device->CreateShaderResourceView(b.instanceBuf, &sv, &b.instanceSRV)) || !b.instanceSRV) {
			logger::error("[GRASS OPTIMIZATIONS] instance raw SRV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.instanceSRV, "GrassOptimizations::InstanceBuf SRV");
	}

	// --- origin + spawn time ---
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.originBuf)) || !b.originBuf) {
			logger::error("[GRASS OPTIMIZATIONS] origin buffer create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.originBuf, "GrassOptimizations::OriginBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap;
		if (FAILED(device->CreateShaderResourceView(b.originBuf, &sv, &b.originSRV)) || !b.originSRV) {
			logger::error("[GRASS OPTIMIZATIONS] origin SRV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.originSRV, "GrassOptimizations::OriginBuf SRV");
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 32;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.compactedBuf)) || !b.compactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] compacted buffer create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.compactedBuf, "GrassOptimizations::CompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.compactedBuf, &uav, &b.compactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] compacted UAV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.compactedUAV, "GrassOptimizations::CompactedBuf UAV");
	}

	// extras: [i*2+0] = {origin.xyz, isComplex}, [i*2+1] = {windCur, windPrev, fade, 0}
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 2 * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.extrasBuf)) || !b.extrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] extras buffer create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.extrasBuf, "GrassOptimizations::ExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 2;
		uav.Buffer.Flags = 0;
		if (FAILED(device->CreateUnorderedAccessView(b.extrasBuf, &uav, &b.extrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras UAV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.extrasUAV, "GrassOptimizations::ExtrasBuf UAV");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap * 2;
		if (FAILED(device->CreateShaderResourceView(b.extrasBuf, &sv, &b.extrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras SRV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.extrasSRV, "GrassOptimizations::ExtrasBuf SRV");
	}

	// counter — CopyStructureCount does not work on a raw UAV, so the survivor count
	// is an InterlockedAdd target copied into the args with CopySubresourceRegion
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 4;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		const uint32_t zero = 0;
		D3D11_SUBRESOURCE_DATA init{ &zero, 0, 0 };
		if (FAILED(device->CreateBuffer(&bd, &init, &b.counterBuf)) || !b.counterBuf) {
			logger::error("[GRASS OPTIMIZATIONS] counter buffer create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.counterBuf, "GrassOptimizations::CounterBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = 1;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.counterBuf, &uav, &b.counterUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] counter UAV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.counterUAV, "GrassOptimizations::CounterBuf UAV");
	}

	// args — preferably UAV-writable, so the cull CS drops its survivor count straight in and we
	// avoid the per-bucket counter reset + CopySubresourceRegion hop (that dependency chain is what
	// stalls the indirect draws).
	{
		// 8 uints: 3 of leading pad, then the 5-uint args block at kArgsByteOffset.
		const uint32_t initArgs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 8 * sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

		if (SUCCEEDED(device->CreateBuffer(&bd, &init, &b.argsBuf)) && b.argsBuf) {
			// Window the view onto instanceCount alone: the shader's address 0 then maps to it — so
			// the CS needs no change — and ClearUnorderedAccessViewUint resets the count without
			// disturbing indexCount, which is written once at first draw. FirstElement 4 == byte 16,
			// the alignment the earlier attempt violated.
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R32_TYPELESS;
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.FirstElement = kArgsInstanceCountOffset / sizeof(uint32_t);
			uav.Buffer.NumElements = 1;
			uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			if (FAILED(device->CreateUnorderedAccessView(b.argsBuf, &uav, &b.argsUAV)))
				b.argsUAV = nullptr;
		}

		if (!b.argsBuf) {
			// Runtime rejected the UAV-capable indirect-args buffer — plain args + the copy path.
			bd.BindFlags = 0;
			bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			if (FAILED(device->CreateBuffer(&bd, &init, &b.argsBuf)) || !b.argsBuf) {
				logger::error("[GRASS OPTIMIZATIONS] args buffer create failed");
				b.ReleaseResources();
				return false;
			}
		}
		Util::SetResourceName(b.argsBuf, "GrassOptimizations::ArgsBuf");

		static bool loggedArgsPath = false;
		if (!loggedArgsPath) {
			loggedArgsPath = true;
			logger::info("[GRASS OPTIMIZATIONS] indirect args path: {}",
				b.argsUAV ? "CS writes instanceCount directly (no counter copy)" :
							"counter + CopySubresourceRegion fallback");
		}
	}

	b.capacityInstances = cap;
	b.dirty = true;
	return true;
}

bool GrassOptimizations::AabbVisible(const RE::NiFrustumPlanes& f, const RE::NiPoint3& mn, const RE::NiPoint3& mx)
{
	static constexpr RE::NiFrustumPlanes::ActivePlane kBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	for (uint32_t i = 0; i < 6; ++i) {
		if (!f.activePlanes.any(kBits[i]))
			continue;

		const auto& pl = f.cullingPlanes[i];

		const __m128 n = _mm_set_ps(0.0f, pl.normal.z, pl.normal.y, pl.normal.x);
		const __m128 p = _mm_set_ps(0.0f,
			pl.normal.z >= 0.0f ? mx.z : mn.z,
			pl.normal.y >= 0.0f ? mx.y : mn.y,
			pl.normal.x >= 0.0f ? mx.x : mn.x);

		__m128 dot = _mm_mul_ps(n, p);
		__m128 sum = _mm_add_ps(dot, _mm_movehl_ps(dot, dot));
		sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 1));

		// Gamebryo convention: dot(n, p) - constant < 0 → outside
		sum = _mm_sub_ss(sum, _mm_set_ss(pl.constant));

		if (_mm_comilt_ss(sum, _mm_setzero_ps()))
			return false;
	}
	return true;
}

void GrassOptimizations::CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape,
	RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData)
{
	if (!shape || !header || !instanceData)
		return;

	auto prop = shape->GetGeometryRuntimeData().shaderProperty;
	if (!prop || prop->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get())
		return;

	RE::NiSourceTexture* tex = prop->GetBaseTexture();
	if (!tex)
		return;

	const uint64_t descVal = *reinterpret_cast<const uint64_t*>(&shape->GetGeometryRuntimeData().vertexDesc);
	StageCapture(shape, instanceData, header->groupInstanceCount, ((descVal >> 2) & 0x3C), descVal, tex);
}

void GrassOptimizations::InitCullResources()
{
	auto* device = globals::d3d::device;

	// --- small CBs first: no external dependency, can't fail from missing shaders ---
	auto makeDynamicCB = [&](ID3D11Buffer** out, UINT bytes, const char* name) -> bool {
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = bytes;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bd, nullptr, out)) || !*out) {
			logger::error("[GRASS OPTIMIZATIONS] {} create failed", name);
			return false;
		}
		Util::SetResourceName(*out, name);
		return true;
	};

	if (!makeDynamicCB(&cullParamsCB, sizeof(CullParamsCB), "GrassOptimizations::CullParamsCB"))
		return;
	if (!makeDynamicCB(&cullBucketCB, sizeof(CullBucketCB), "GrassOptimizations::CullBucketCB"))
		return;
	if (!makeDynamicCB(&detectParamsCB, 16, "GrassOptimizations::DetectParamsCB"))
		return;

	std::vector<std::pair<const char*, const char*>> defines;
	cullCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", defines, "cs_5_0"));
	if (!cullCS) {
		logger::error("[GRASS OPTIMIZATIONS] cull CS load failed — feature disabled");
		return;
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(uint32_t);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &detectResultBuf)) || !detectResultBuf) {
			logger::error("[GRASS OPTIMIZATIONS] detect result buffer create failed");
			return;
		}
		Util::SetResourceName(detectResultBuf, "GrassOptimizations::DetectResultBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = 1;
		uav.Buffer.Flags = 0;
		if (FAILED(device->CreateUnorderedAccessView(detectResultBuf, &uav, &detectResultUAV)) || !detectResultUAV) {
			logger::error("[GRASS OPTIMIZATIONS] detect result UAV create failed");
			return;
		}
		Util::SetResourceName(detectResultUAV, "GrassOptimizations::DetectResultBuf UAV");
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_STAGING;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &detectStaging)) || !detectStaging) {
			logger::error("[GRASS OPTIMIZATIONS] detect staging create failed");
			return;
		}
	}

	detectCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\DetectComplexCS.hlsl", defines, "cs_5_0"));
	if (!detectCS) {
		logger::error("[GRASS OPTIMIZATIONS] detect CS load failed — complex detection disabled");
	}

	if (FAILED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) {
		logger::error("[GRASS OPTIMIZATIONS] ID3D11DeviceContext1 unavailable — feature disabled");
		ctx1 = nullptr;
		return;
	}
}

void GrassOptimizations::CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	if (b.cullSlot == UINT32_MAX)
		return;

	// The CS always InterlockedAdds at address 0 of whatever is bound to u2. With the UAV-capable
	// args buffer that window starts at args[1], so the survivor count lands in the indirect args
	// directly — no counter buffer, no copy, one less dependency for the draw to wait on.
	ID3D11UnorderedAccessView* countUAV = b.argsUAV ? b.argsUAV : b.counterUAV;
	if (b.argsUAV) {
		const UINT zeros[4] = { 0, 0, 0, 0 };
		ctx->ClearUnorderedAccessViewUint(b.argsUAV, zeros);
	} else {
		const uint32_t zero = 0;
		const D3D11_BOX box{ 0, 0, 0, 4, 1, 1 };
		ctx->UpdateSubresource(b.counterBuf, 0, &box, &zero, 0, 0);
	}

	// Second bin. Null UAVs are legal when the bucket has no LOD mesh — lodEnabled is 0 in that
	// case, so the CS never routes anything here regardless.
	ID3D11UnorderedAccessView* lodCountUAV = nullptr;
	if (b.lodActive) {
		lodCountUAV = b.lodArgsUAV ? b.lodArgsUAV : b.lodCounterUAV;
		if (b.lodArgsUAV) {
			const UINT zeros[4] = { 0, 0, 0, 0 };
			ctx->ClearUnorderedAccessViewUint(b.lodArgsUAV, zeros);
		} else {
			const uint32_t zero = 0;
			const D3D11_BOX box{ 0, 0, 0, 4, 1, 1 };
			ctx->UpdateSubresource(b.lodCounterBuf, 0, &box, &zero, 0, 0);
		}
	}

	ID3D11UnorderedAccessView* uavs[6] = {
		b.compactedUAV, b.extrasUAV, countUAV,
		b.lodActive ? b.lodCompactedUAV : nullptr,
		b.lodActive ? b.lodExtrasUAV : nullptr,
		lodCountUAV
	};
	ctx->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);

	ID3D11ShaderResourceView* srvs[3] = { b.instanceSRV, b.originSRV, hiZValid ? hiZSRV : nullptr };
	ctx->CSSetShaderResources(0, 3, srvs);

	UINT first = b.cullSlot * 16;
	UINT num = 16;
	ctx1->CSSetConstantBuffers1(1, 1, &cullBucketCB, &first, &num);

	ctx->Dispatch((b.totalInstances + 63) / 64, 1, 1);

	if (!b.argsUAV) {
		const D3D11_BOX src{ 0, 0, 0, 4, 1, 1 };
		ctx->CopySubresourceRegion(b.argsBuf, 0, kArgsInstanceCountOffset, 0, 0, b.counterBuf, 0, &src);
	}
	if (b.lodActive && !b.lodArgsUAV) {
		const D3D11_BOX src{ 0, 0, 0, 4, 1, 1 };
		ctx->CopySubresourceRegion(b.lodArgsBuf, 0, kArgsInstanceCountOffset, 0, 0, b.lodCounterBuf, 0, &src);
	}
}

bool GrassOptimizations::EnsureCullBucketCapacity(uint32_t slots, ID3D11Device* device)
{
	if (cullBucketCB && cullBucketCBSlots >= slots)
		return true;

	uint32_t cap = cullBucketCBSlots ? cullBucketCBSlots : 64;
	while (cap < slots)
		cap *= 2;

	if (cullBucketCB) {
		cullBucketCB->Release();
		cullBucketCB = nullptr;
	}

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = cap * kSlotBytes;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&bd, nullptr, &cullBucketCB)) || !cullBucketCB) {
		logger::error("[GRASS OPTIMIZATIONS] cull bucket CB create failed slots={}", cap);
		cullBucketCBSlots = 0;
		return false;
	}
	Util::SetResourceName(cullBucketCB, "GrassOptimizations::CullBucketCB");
	cullBucketCBSlots = cap;
	return true;
}

void GrassOptimizations::UpdateCoarseBounds(GrassBucket& b)
{
	RE::NiPoint3 mn{ FLT_MAX, FLT_MAX, FLT_MAX };
	RE::NiPoint3 mx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto& s : b.slices) {
		mn.x = std::min(mn.x, s.origin.x + s.localMin.x);
		mn.y = std::min(mn.y, s.origin.y + s.localMin.y);
		mn.z = std::min(mn.z, s.origin.z + s.localMin.z);
		mx.x = std::max(mx.x, s.origin.x + s.localMax.x);
		mx.y = std::max(mx.y, s.origin.y + s.localMax.y);
		mx.z = std::max(mx.z, s.origin.z + s.localMax.z);
	}
	if (mn.x > mx.x) {  // no slices / no instances — leave an empty box
		mn = { 0.0f, 0.0f, 0.0f };
		mx = { 0.0f, 0.0f, 0.0f };
	}

	const float pad = b.clumpRadius + 128.0f;
	mn.x -= pad;
	mn.y -= pad;
	mn.z -= pad;
	mx.x += pad;
	mx.y += pad;
	mx.z += pad;

	b.coarseMin = mn;
	b.coarseMax = mx;
	b.coarseValid = true;
}

bool GrassOptimizations::DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11Device*, ID3D11DeviceContext* ctx)
{
	if (auto it = complexCache.find(tex); it != complexCache.end())
		return it->second;

	bool complex = false;

	auto* rt = tex ? tex->rendererTexture : nullptr;
	if (detectCS && rt && rt->resourceView && rt->height > 0) {
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(ctx->Map(detectParamsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				struct DetectParams
				{
					uint32_t texHeight;
					float threshold;
					uint32_t pad[2];
				};
				auto* dp = static_cast<DetectParams*>(m.pData);
				dp->texHeight = rt->height;
				dp->threshold = globals::features::grassLighting.settings.ComplexGrassThreshold;
				dp->pad[0] = dp->pad[1] = 0;
				ctx->Unmap(detectParamsCB, 0);
			}
		}

		UINT initialCount = 0;
		ctx->CSSetUnorderedAccessViews(0, 1, &detectResultUAV, &initialCount);
		ctx->CSSetShader(detectCS, nullptr, 0);
		ctx->CSSetConstantBuffers(0, 1, &detectParamsCB);
		ctx->CSSetShaderResources(0, 1, &rt->resourceView);
		ctx->Dispatch(1, 1, 1);

		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ctx->CSSetShaderResources(0, 1, &nullSRV);

		// one stall per unique texture, at cell load — never per frame
		ctx->CopyResource(detectStaging, detectResultBuf);
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(detectStaging, 0, D3D11_MAP_READ, 0, &m))) {
			complex = (*static_cast<const uint32_t*>(m.pData)) != 0;
			ctx->Unmap(detectStaging, 0);
		}
	}

	complexCache.emplace(tex, complex);
	return complex;
}

static RE::BSTriShape* FindFirstTriShape(RE::NiAVObject* obj)
{
	if (!obj)
		return nullptr;
	if (auto* ts = obj->AsTriShape())
		return ts;
	if (auto* node = obj->AsNode()) {
		for (auto& child : node->GetChildren()) {
			if (auto* ts = FindFirstTriShape(child.get()))
				return ts;
		}
	}
	return nullptr;
}

uint32_t GrassOptimizations::ResolveMeshId(RE::BSMultiStreamInstanceTriShape* shape)
{
	if (!shape)
		return 0;
	if (auto it = shapeMeshId.find(shape); it != shapeMeshId.end())
		return it->second;

	// The LoadGrassType hook recorded this shape's source .nif when the grass type was created,
	// which always happens before any instance of it is captured — so by the time we get here the
	// entry exists. (The previous approach walked BGSGrassManager's grass-type map looking for a
	// matching typeShape; it failed on both counts — most captured shapes matched no entry, and
	// the one that did still produced an empty model path.)
	std::string stem;
	{
		std::scoped_lock lk(meshPathMutex);
		if (auto it = shapeModelStem.find(shape); it != shapeModelStem.end())
			stem = it->second;
	}

	// Temporary: confirms the hook actually covers the shapes we draw. A run with no
	// "stem=''" lines means every bucket is now keyed by mesh.
	static int meshIdLogCount = 0;
	if (meshIdLogCount < 16) {
		++meshIdLogCount;
		logger::info("[GRASS OPTIMIZATIONS][meshid] shape={:p} stem='{}'", (void*)shape, stem);
	}

	// intern the stem so bucket keys and the LOD cache work on integers, not strings
	uint32_t meshId = 0;
	if (!stem.empty()) {
		if (auto it = meshIdMap.find(stem); it != meshIdMap.end()) {
			meshId = it->second;
		} else {
			meshStems.push_back(stem);
			meshId = static_cast<uint32_t>(meshStems.size());  // ids are 1-based
			meshIdMap.emplace(stem, meshId);
		}
	}

	shapeMeshId.emplace(shape, meshId);
	return meshId;
}

void GrassOptimizations::EnsureLODMesh(uint32_t meshId)
{
	if (!settings.EnableMeshLOD || meshId == 0 || meshId > meshStems.size())
		return;
	if (lodMeshCache.contains(meshId))
		return;

	LODMesh entry;  // valid == false: no/incompatible LOD mesh → the full mesh is drawn

	// Convention: meshes\LOD\Grass\<source-mesh-stem>.nif, authored in the same local space and
	// vertex format as the source grass (BSModelDB::Demand prepends "meshes\").
	const std::string modelPath = "LOD\\Grass\\" + meshStems[meshId - 1] + ".nif";

	RE::BSModelDB::DBTraits::ArgsType args{};
	args.unk8 = false;
	args.unkA = false;
	args.postProcess = false;
	RE::NiPointer<RE::NiNode> root;
	if (RE::BSModelDB::Demand(modelPath.c_str(), root, args) == RE::BSResource::ErrorCode::kNone && root) {
		if (auto* ts = FindFirstTriShape(root.get())) {
			auto& grd = ts->GetGeometryRuntimeData();
			auto* rd = grd.rendererData;
			if (rd && rd->vertexBuffer && rd->indexBuffer) {
				entry.keepAlive = RE::NiPointer<RE::NiAVObject>(root.get());
				entry.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(rd->vertexBuffer);
				entry.indexBuffer = reinterpret_cast<ID3D11Buffer*>(rd->indexBuffer);
				entry.descVal = *reinterpret_cast<const uint64_t*>(&grd.vertexDesc);
				entry.meshStride = (uint32_t)((4 * entry.descVal) & 0x3C);
				entry.indexCount = 3u * ts->GetTrishapeRuntimeData().triangleCount;
				entry.valid = true;
				logger::info("[GRASS OPTIMIZATIONS] LOD mesh {} loaded: tris={} descVal={:016X}",
					modelPath, entry.indexCount / 3, entry.descVal);
			} else {
				logger::warn("[GRASS OPTIMIZATIONS] LOD mesh {} has no GPU buffers", modelPath);
			}
		} else {
			logger::warn("[GRASS OPTIMIZATIONS] LOD mesh {} has no TriShape", modelPath);
		}
	}
	// file absent → error != kNone → silent; that grass type keeps its full mesh

	lodMeshCache.emplace(meshId, std::move(entry));
}

bool GrassOptimizations::BuildHiZ(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	hiZValid = false;
	if (!settings.EnableOcclusionCulling)
		return false;

	// The live main depth is bound as the DSV while grass draws, so it cannot also be an SRV. The
	// scene depth copy is already populated with this frame's statics by the time the first grass
	// SetupGeometry runs — which is what makes this cheap.
	//
	// Which resource that is, and its format, depends on TerrainBlending:
	//   loaded     -> TerrainBlending::blendedDepthTexture   (R32_FLOAT)
	//   not loaded -> the game's kPOST_ZPREPASS_COPY         (R24_UNORM_X8_TYPELESS)
	// The HLSL declaration must match, so the shader is compiled with TERRAIN_BLENDING to flip
	// SrcDepth between `Texture2D<float>` and `Texture2D<unorm float>`.
	auto* srcSRV = Util::GetCurrentSceneDepthSRV(false);
	if (!srcSRV)
		return false;

	const auto [screenW, screenH] = globals::game::renderer->GetScreenSize();
	const uint32_t dstW = ((uint32_t)screenW + kHiZTileSize - 1) / kHiZTileSize;
	const uint32_t dstH = ((uint32_t)screenH + kHiZTileSize - 1) / kHiZTileSize;
	if (!dstW || !dstH)
		return false;

	if (dstW != hiZWidth || dstH != hiZHeight) {
		auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		for (auto*& v : hiZMipUAVs)
			rel(v);
		for (auto*& v : hiZMipSRVs)
			rel(v);
		hiZMipUAVs.clear();
		hiZMipSRVs.clear();
		rel(hiZSRV);
		rel(hiZUAV);
		rel(hiZTex);

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
		if (FAILED(device->CreateTexture2D(&td, nullptr, &hiZTex)) || !hiZTex) {
			logger::error("[GRASS OPTIMIZATIONS] HiZ texture create failed");
			return false;
		}
		Util::SetResourceName(hiZTex, "GrassOptimizations::HiZ");

		// Full-chain SRV for the cull (it Loads from an explicit level), plus single-level views
		// for the reduction passes.
		if (FAILED(device->CreateShaderResourceView(hiZTex, nullptr, &hiZSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] HiZ SRV create failed");
			return false;
		}

		for (uint32_t m = 0; m < mips; ++m) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
			ud.Format = DXGI_FORMAT_R32_FLOAT;
			ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			ud.Texture2D.MipSlice = m;
			ID3D11UnorderedAccessView* uav = nullptr;
			if (FAILED(device->CreateUnorderedAccessView(hiZTex, &ud, &uav))) {
				logger::error("[GRASS OPTIMIZATIONS] HiZ mip UAV create failed");
				return false;
			}
			hiZMipUAVs.push_back(uav);

			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			sd.Format = DXGI_FORMAT_R32_FLOAT;
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MostDetailedMip = m;
			sd.Texture2D.MipLevels = 1;
			ID3D11ShaderResourceView* srv = nullptr;
			if (FAILED(device->CreateShaderResourceView(hiZTex, &sd, &srv))) {
				logger::error("[GRASS OPTIMIZATIONS] HiZ mip SRV create failed");
				return false;
			}
			hiZMipSRVs.push_back(srv);
		}

		hiZUAV = hiZMipUAVs[0];
		hiZWidth = dstW;
		hiZHeight = dstH;
		hiZMipCount = mips;
	}

	// The depth SRV's format is baked into the shader, so a change in TerrainBlending's loaded
	// state invalidates the compiled shader — reading R32_FLOAT through a unorm declaration (or
	// the reverse) yields garbage depths and would cull visible grass.
	const bool terrainBlending = globals::features::terrainBlending.loaded;
	if (hiZCS && terrainBlending != hiZCSTerrainBlending) {
		hiZCS->Release();
		hiZCS = nullptr;
	}

	if (!hiZCS) {
		// TERRAIN_BLENDING flips SrcDepth's HLSL type from `Texture2D<unorm float>`
		// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
		std::vector<std::pair<const char*, const char*>> defines;
		if (terrainBlending)
			defines.push_back({ "TERRAIN_BLENDING", "" });

		hiZCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassHiZCS.hlsl", defines, "cs_5_0"));
		if (!hiZCS) {
			logger::error("[GRASS OPTIMIZATIONS] HiZ CS load failed — occlusion culling disabled");
			return false;
		}
		hiZCSTerrainBlending = terrainBlending;
	}

	// Reads our own R32_FLOAT pyramid, so it needs no TERRAIN_BLENDING variant.
	if (!hiZMipCS) {
		hiZMipCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassHiZMipCS.hlsl", {}, "cs_5_0"));
		if (!hiZMipCS)
			logger::error("[GRASS OPTIMIZATIONS] HiZ mip CS load failed — large clumps will not be occlusion culled");
	}

	if (!hiZParamsCB) {
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 16;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &hiZParamsCB)) || !hiZParamsCB)
			return false;
	}

	{
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(ctx->Map(hiZParamsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
			return false;
		auto* p = static_cast<uint32_t*>(m.pData);
		p[0] = (uint32_t)screenW;
		p[1] = (uint32_t)screenH;
		p[2] = dstW;
		p[3] = dstH;
		ctx->Unmap(hiZParamsCB, 0);
	}

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11ShaderResourceView* nullSRV = nullptr;

	ctx->CSSetShader(hiZCS, nullptr, 0);
	ctx->CSSetConstantBuffers(0, 1, &hiZParamsCB);
	ctx->CSSetShaderResources(0, 1, &srcSRV);
	ctx->CSSetUnorderedAccessViews(0, 1, &hiZUAV, nullptr);
	ctx->Dispatch(dstW, dstH, 1);
	ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ctx->CSSetShaderResources(0, 1, &nullSRV);

	// Reduce the chain. Each level is the exact max of the one above, so the cull can test a
	// clump of any on-screen size against a fixed number of texels.
	if (hiZMipCS && hiZMipCount > 1) {
		ctx->CSSetShader(hiZMipCS, nullptr, 0);
		uint32_t srcW = dstW, srcH = dstH;
		for (uint32_t m = 1; m < hiZMipCount; ++m) {
			const uint32_t mw = std::max(1u, srcW >> 1);
			const uint32_t mh = std::max(1u, srcH >> 1);

			D3D11_MAPPED_SUBRESOURCE mm{};
			if (SUCCEEDED(ctx->Map(hiZParamsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
				auto* p = static_cast<uint32_t*>(mm.pData);
				p[0] = srcW;
				p[1] = srcH;
				p[2] = mw;
				p[3] = mh;
				ctx->Unmap(hiZParamsCB, 0);
			}

			ctx->CSSetShaderResources(0, 1, &hiZMipSRVs[m - 1]);
			ctx->CSSetUnorderedAccessViews(0, 1, &hiZMipUAVs[m], nullptr);
			ctx->Dispatch((mw + 7) / 8, (mh + 7) / 8, 1);
			ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			ctx->CSSetShaderResources(0, 1, &nullSRV);

			srcW = mw;
			srcH = mh;
		}
	}

	static bool loggedHiZ = false;
	if (!loggedHiZ) {
		loggedHiZ = true;
		logger::info("[GRASS OPTIMIZATIONS] HiZ occlusion cull active: {}x{} tiles (1/{} res), {} mips, terrainBlending={} (depth SRV format {})",
			dstW, dstH, kHiZTileSize, hiZMipCS ? hiZMipCount : 1u, terrainBlending, terrainBlending ? "R32_FLOAT" : "R24_UNORM");
	}

	hiZValid = true;
	return true;
}

bool GrassOptimizations::EnsureLODBin(GrassBucket& b, ID3D11Device* device)
{
	if (!settings.EnableMeshLOD || b.meshId == 0) {
		if (b.lodCapacityInstances)
			b.ReleaseLODBin();
		return false;
	}

	// Cheap after the first call — a missing .nif caches an invalid entry rather than re-Demanding.
	EnsureLODMesh(b.meshId);
	auto lodIt = lodMeshCache.find(b.meshId);
	if (lodIt == lodMeshCache.end() || !lodIt->second.valid) {
		if (b.lodCapacityInstances)
			b.ReleaseLODBin();
		return false;
	}

	const uint32_t cap = b.capacityInstances;
	if (!cap)
		return false;
	if (b.lodCapacityInstances >= cap && b.lodCompactedBuf)
		return true;

	// Worst case every survivor takes the LOD mesh, so the bin is sized like the full-detail one.
	b.ReleaseLODBin();

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 32;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.lodCompactedBuf)) || !b.lodCompactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] LOD compacted buffer create failed");
			b.ReleaseLODBin();
			return false;
		}
		Util::SetResourceName(b.lodCompactedBuf, "GrassOptimizations::LODCompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.lodCompactedBuf, &uav, &b.lodCompactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] LOD compacted UAV create failed");
			b.ReleaseLODBin();
			return false;
		}
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 2 * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.lodExtrasBuf)) || !b.lodExtrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] LOD extras buffer create failed");
			b.ReleaseLODBin();
			return false;
		}
		Util::SetResourceName(b.lodExtrasBuf, "GrassOptimizations::LODExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 2;
		uav.Buffer.Flags = 0;
		if (FAILED(device->CreateUnorderedAccessView(b.lodExtrasBuf, &uav, &b.lodExtrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] LOD extras UAV create failed");
			b.ReleaseLODBin();
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap * 2;
		if (FAILED(device->CreateShaderResourceView(b.lodExtrasBuf, &sv, &b.lodExtrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] LOD extras SRV create failed");
			b.ReleaseLODBin();
			return false;
		}
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 4;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		const uint32_t zero = 0;
		D3D11_SUBRESOURCE_DATA init{ &zero, 0, 0 };
		if (FAILED(device->CreateBuffer(&bd, &init, &b.lodCounterBuf)) || !b.lodCounterBuf) {
			logger::error("[GRASS OPTIMIZATIONS] LOD counter buffer create failed");
			b.ReleaseLODBin();
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = 1;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.lodCounterBuf, &uav, &b.lodCounterUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] LOD counter UAV create failed");
			b.ReleaseLODBin();
			return false;
		}
	}

	// Same preference as the main bin: UAV-writable args so the CS drops the survivor count in
	// directly, else the counter + copy fallback.
	{
		const uint32_t initArgs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 8 * sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

		if (SUCCEEDED(device->CreateBuffer(&bd, &init, &b.lodArgsBuf)) && b.lodArgsBuf) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R32_TYPELESS;
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.FirstElement = kArgsInstanceCountOffset / sizeof(uint32_t);
			uav.Buffer.NumElements = 1;
			uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			if (FAILED(device->CreateUnorderedAccessView(b.lodArgsBuf, &uav, &b.lodArgsUAV)))
				b.lodArgsUAV = nullptr;
		}

		if (!b.lodArgsBuf) {
			bd.BindFlags = 0;
			bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			if (FAILED(device->CreateBuffer(&bd, &init, &b.lodArgsBuf)) || !b.lodArgsBuf) {
				logger::error("[GRASS OPTIMIZATIONS] LOD args buffer create failed");
				b.ReleaseLODBin();
				return false;
			}
		}
		Util::SetResourceName(b.lodArgsBuf, "GrassOptimizations::LODArgsBuf");
	}

	b.lodCapacityInstances = cap;
	return true;
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_dtor::thunk(RE::BSMultiStreamInstanceTriShape* shape)
{
	auto& self = globals::features::grassOptimizations;
	{
		std::scoped_lock lk(self.pendingMutex);
		self.pendingRemoves.push_back(shape);
	}
	func(shape);
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_OnVisible::thunk(RE::BSMultiStreamInstanceTriShape* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex)
{
	auto prop = This->GetGeometryRuntimeData().shaderProperty;
	if (prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		process->AppendVirtual(This, alphaGroupIndex);
		return;
	}

	func(This, process, alphaGroupIndex);
}

void GrassOptimizations::Hooks::DoneAddingInstances::thunk(RE::BSMultiStreamInstanceTriShape* shape,
	RE::BSTArray<std::uint32_t>& a_instances)
{
	auto& self = globals::features::grassOptimizations;

	auto& rt = shape->GetMultiStreamTrishapeRuntimeData();
	auto prop = shape->GetGeometryRuntimeData().shaderProperty;
	if (rt.groupAlloc && prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		if (auto* tex = prop->GetBaseTexture()) {
			const uint64_t descVal = *reinterpret_cast<uint64_t*>(&shape->GetGeometryRuntimeData().vertexDesc);
			self.StageCapture(shape, rt.groupAlloc, rt.instanceCount,
				2u * rt.instanceSize, descVal, tex);
		}
	}
	func(shape, a_instances);
}

void GrassOptimizations::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags)
{
	auto& self = globals::features::grassOptimizations;

	const auto frame = globals::game::graphicsState->frameCount;
	if (self.lastFrame != frame) {
		self.UpdateGrass();
		self.lastFrame = frame;
	}

	func(This, a2, flags);
}

bool GrassOptimizations::Hooks::BSMultiBoundAABB_WithinFrustum::thunk(RE::BSMultiBoundAABB* a_this, RE::NiFrustumPlanes* a_planes)
{
	const auto mask = a_planes->activePlanes.underlying();
	if (!mask)
		return true;

	struct alignas(16) Cache
	{
		std::uint32_t mask{ 0xFFFFFFFF };
		std::uint32_t _pad0{ 0 };
		std::uint64_t _pad1{ 0 };

		RE::NiPlane raw[6]{};

		__m128 nx[2]{ _mm_setzero_ps(), _mm_setzero_ps() };
		__m128 ny[2]{ _mm_setzero_ps(), _mm_setzero_ps() };
		__m128 nz[2]{ _mm_setzero_ps(), _mm_setzero_ps() };
		__m128 d[2]{ _mm_setzero_ps(), _mm_setzero_ps() };

		__m128 ax[2]{ _mm_setzero_ps(), _mm_setzero_ps() };
		__m128 ay[2]{ _mm_setzero_ps(), _mm_setzero_ps() };
		__m128 az[2]{ _mm_setzero_ps(), _mm_setzero_ps() };
	};
	static thread_local Cache c;

	// Rebuild SoA only when the frustum actually changed
	if (mask != c.mask ||
		std::memcmp(c.raw, a_planes->cullingPlanes, sizeof(c.raw)) != 0) {
		c.mask = mask;
		std::memcpy(c.raw, a_planes->cullingPlanes, sizeof(c.raw));

		alignas(16) float nx[8], ny[8], nz[8], d[8];
		for (int i = 0; i < 8; ++i) {
			if (i < 6 && (mask & (1u << i))) {
				const auto& p = a_planes->cullingPlanes[i];
				nx[i] = p.normal.x;
				ny[i] = p.normal.y;
				nz[i] = p.normal.z;
				d[i] = -p.constant;  // Gamebryo: dot(n, x) - c, so fold in negated
			} else {
				nx[i] = ny[i] = nz[i] = d[i] = 0.0f;  // inactive/pad: always passes
			}
		}

		const __m128 sign = _mm_set1_ps(-0.0f);
		for (int b = 0; b < 2; ++b) {
			c.nx[b] = _mm_load_ps(nx + b * 4);
			c.ny[b] = _mm_load_ps(ny + b * 4);
			c.nz[b] = _mm_load_ps(nz + b * 4);
			c.d[b] = _mm_load_ps(d + b * 4);
			c.ax[b] = _mm_andnot_ps(sign, c.nx[b]);
			c.ay[b] = _mm_andnot_ps(sign, c.ny[b]);
			c.az[b] = _mm_andnot_ps(sign, c.nz[b]);
		}
	}

	const __m128 cx = _mm_set1_ps(a_this->center.x);
	const __m128 cy = _mm_set1_ps(a_this->center.y);
	const __m128 cz = _mm_set1_ps(a_this->center.z);
	const __m128 ex = _mm_set1_ps(a_this->size.x);
	const __m128 ey = _mm_set1_ps(a_this->size.y);
	const __m128 ez = _mm_set1_ps(a_this->size.z);

	int outside = 0;
	for (int b = 0; b < 2; ++b) {
		const __m128 s = _mm_fmadd_ps(cx, c.nx[b],
			_mm_fmadd_ps(cy, c.ny[b],
				_mm_fmadd_ps(cz, c.nz[b], c.d[b])));
		const __m128 r = _mm_fmadd_ps(ex, c.ax[b],
			_mm_fmadd_ps(ey, c.ay[b],
				_mm_mul_ps(ez, c.az[b])));
		outside |= _mm_movemask_ps(_mm_add_ps(s, r));  // sign bit => dot(n,c) + r - const < 0
	}
	return outside == 0;
}

std::uint32_t GrassOptimizations::Hooks::AddGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3)
{
	auto& self = globals::features::grassOptimizations;
	self.CaptureGIDGroup(a1, a2, a3);
	return func(a1, a2, a3);
}

std::uint32_t GrassOptimizations::Hooks::AddQueuedGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4)
{
	auto& self = globals::features::grassOptimizations;
	self.CaptureGIDGroup(a1, a2, a3);
	return func(a1, a2, a3, a4);
}

thread_local RE::BSMultiStreamInstanceTriShape::GroupHeader tl_lastFileGroupHeader{};
thread_local std::vector<uint16_t> tl_lastFileInstanceData;
thread_local bool tl_haveFileGroup = false;

void GrassOptimizations::Hooks::ReadGroupHeaderStreamTraits::thunk(RE::BSStreamHeader* streamHeader, RE::BSMultiStreamInstanceTriShape::GroupHeader* groupHeader, uint32_t size)
{
	func(streamHeader, groupHeader, size);
	std::memcpy(&tl_lastFileGroupHeader, groupHeader, std::min<uint32_t>(size, sizeof(tl_lastFileGroupHeader)));
}

void GrassOptimizations::Hooks::ReadInstanceGroupStreamTraits::thunk(RE::BSStreamHeader* streamHeader, uint16_t* instanceData, uint32_t size)
{
	func(streamHeader, instanceData, size);
	tl_lastFileInstanceData.resize(size / sizeof(uint16_t));
	std::memcpy(tl_lastFileInstanceData.data(), instanceData, size);
	tl_haveFileGroup = true;
}

void GrassOptimizations::Hooks::AddGroupQueuedGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3)
{
	tl_haveFileGroup = false;
	func(a1, a2, a3);

	if (tl_haveFileGroup) {
		auto& self = globals::features::grassOptimizations;
		self.CaptureGIDGroup(a1, &tl_lastFileGroupHeader, tl_lastFileInstanceData.data());
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	tl_haveFileGroup = false;
	func(a1, a2);

	if (tl_haveFileGroup) {
		auto& self = globals::features::grassOptimizations;
		self.CaptureGIDGroup(a1, &tl_lastFileGroupHeader, tl_lastFileInstanceData.data());
		tl_haveFileGroup = false;
	}
}

void VanillaDrawInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto* ctx = globals::d3d::context;
	auto& groups = geometry->GetMultiStreamTrishapeRuntimeData().unk160;

	for (uint32_t i = 0; i < groups.size(); ++i) {
		auto curInstanceGroup = groups[i];
		if (!curInstanceGroup || !curInstanceGroup->unk50)
			continue;

		uint32_t indexCount = 0;
		uint32_t* indexCountPtr = &indexCount;
		static REL::Relocation<ID3D11Buffer** (*)(RE::BSGraphics::Renderer*, uint64_t, uint32_t**, uint32_t)>
			MapDynamicBuffer{ REL::RelocationID(75561, 77362) };
		auto buffer = MapDynamicBuffer(globals::game::renderer, 1, &indexCountPtr, 7);
		*indexCountPtr = i;
		if (*buffer)
			ctx->Unmap(*buffer, 0);
		ctx->VSSetConstantBuffers(7u, 1u, buffer);

		static REL::Relocation<void (*)(RE::BSGraphics::Renderer*, RE::BSGraphics::TriShape*, uint32_t, uint32_t, uint32_t, RE::BSGraphics::VertexDesc, ID3D11Buffer**)>
			DrawInstancedTriShape{ REL::RelocationID(75479, 77265) };
		DrawInstancedTriShape(globals::game::renderer, geometry->GetGeometryRuntimeData().rendererData, 0,
			geometry->GetTrishapeRuntimeData().triangleCount, curInstanceGroup->instanceCount,
			geometry->GetGeometryRuntimeData().vertexDesc,
			reinterpret_cast<ID3D11Buffer**>(curInstanceGroup->buffer));
	}
}

RE::BSMultiStreamInstanceTriShape* GrassOptimizations::Hooks::LoadGrassType::thunk(
	RE::BGSGrassManager* grassManager,
	RE::GrassParam* a_param,
	uint32_t CellXDivided,
	uint32_t CellYDivided,
	uint64_t* typeKey,
	RE::BSFixedString* modelPath)
{
	auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

	// On return the game has prepared this grass type's "meshes\..." path into modelPath and given
	// us the shape every instance of the type will be added to. This is the only point where both
	// are available together, and it runs before any instance is captured — so recording the pair
	// here gives ResolveMeshId a hit for every shape we later draw.
	if (shape && modelPath) {
		if (const char* path = modelPath->c_str(); path && *path) {
			const std::string p = path;
			const size_t slash = p.find_last_of("\\/");
			const size_t start = slash == std::string::npos ? 0 : slash + 1;
			const size_t dot = p.find_last_of('.');
			const size_t len = (dot == std::string::npos || dot < start) ? std::string::npos : dot - start;

			auto& self = globals::features::grassOptimizations;
			std::scoped_lock lk(self.meshPathMutex);
			self.shapeModelStem.insert_or_assign(shape, p.substr(start, len));
		}
	}

	return shape;
}

void GrassOptimizations::Hooks::DrawInstanceTriShape::thunk(RE::BSRenderPass* pass, RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto& self = globals::features::grassOptimizations;
	auto* ctx = globals::d3d::context;

	if (auto rtti = geometry->GetGeometryRuntimeData().shaderProperty->GetRTTI()) {
		if (rtti != globals::rtti::BSGrassShaderPropertyRTTI.get()) {
			VanillaDrawInstanceTriShape(geometry);
			return;
		}
	}
	RE::NiSourceTexture* diffuseTexture = geometry->GetGeometryRuntimeData().shaderProperty->GetBaseTexture();
	if (!diffuseTexture) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	const uint64_t descVal = *reinterpret_cast<uint64_t*>(&geometry->GetGeometryRuntimeData().vertexDesc);
	const uint32_t frame = globals::game::graphicsState->frameCount;

	GrassBucket* b = nullptr;
	{
		std::scoped_lock lk(self.bucketMutex);
		// Same identity rule as capture: mesh id when resolvable, else the texture. The per-shape
		// result is cached, so this stays an integer hash lookup on the hot path.
		const uint32_t meshId = self.ResolveMeshId(geometry);
		auto it = self.buckets.find({ meshId, meshId ? nullptr : diffuseTexture, descVal });
		if (it == self.buckets.end() || !it->second.totalInstances || !it->second.instanceBuf) {
			VanillaDrawInstanceTriShape(geometry);
			return;
		}
		b = &it->second;
		if (b->drawnFrame == frame && b->drawnPass == pass)
			return;
		b->drawnFrame = frame;
		b->drawnPass = pass;
	}

	if (!b->cullVisible) {
		return;
	}

	const uint32_t meshStride = (uint32_t)((4 * descVal) & 0x3C);
	auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
	auto* meshVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	auto* indexB = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

	if (!b->argsIndexCountWritten) {
		const uint32_t indexCount = 3u * geometry->GetTrishapeRuntimeData().triangleCount;
		const D3D11_BOX argBox{ kArgsByteOffset, 0, 0, kArgsByteOffset + sizeof(uint32_t), 1, 1 };
		ctx->UpdateSubresource(b->argsBuf, 0, &argBox, &indexCount, 0, 0);
		b->argsIndexCountWritten = true;
	}

	// engine state
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	if (shadowState.vertexDesc != descVal) {
		shadowState.vertexDesc = descVal;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	}
	if (shadowState.topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		shadowState.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}
	static REL::Relocation<void (*)(uint32_t)> SetDirtyStates{ REL::RelocationID(75580, 77386) };
	SetDirtyStates(0);

	ctx->IASetIndexBuffer(indexB, DXGI_FORMAT_R16_UINT, 0);

	ID3D11Buffer* vbs[2] = { meshVB, nullptr };
	UINT strides[2] = { meshStride, 32 };
	UINT offsets[2] = { 0, 0 };

	vbs[1] = b->compactedBuf;
	ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
	ctx->VSSetShaderResources(2, 1, &b->extrasSRV);
	ctx->DrawIndexedInstancedIndirect(b->argsBuf, kArgsByteOffset);

	// --- Mesh-swap LOD: second draw for the instances the cull CS routed to bin 1 ---
	if (!b->lodActive)
		return;

	const LODMesh* lod = nullptr;
	{
		std::scoped_lock lk(self.bucketMutex);
		if (auto it = self.lodMeshCache.find(b->meshId); it != self.lodMeshCache.end() && it->second.valid)
			lod = &it->second;
	}
	if (!lod || !lod->vertexBuffer || !lod->indexBuffer)
		return;

	if (!b->lodArgsIndexCountWritten) {
		const D3D11_BOX argBox{ kArgsByteOffset, 0, 0, kArgsByteOffset + sizeof(uint32_t), 1, 1 };
		ctx->UpdateSubresource(b->lodArgsBuf, 0, &argBox, &lod->indexCount, 0, 0);
		b->lodArgsIndexCountWritten = true;
	}

	// The LOD nif is expected to share the source mesh's vertex format. It is drawn either way (by
	// request), but a mismatch means the input layout below no longer matches what the grass VS
	// was compiled against — so log it once, since the symptom is corrupt geometry rather than
	// merely coarser geometry.
	if (lod->descVal != descVal) {
		static std::unordered_set<uint32_t> warned;
		std::scoped_lock lk(self.bucketMutex);
		if (warned.insert(b->meshId).second) {
			logger::warn("[GRASS OPTIMIZATIONS] LOD mesh '{}' vertexDesc {:016X} != source {:016X} — drawing anyway; expect corrupt geometry if the formats really differ",
				b->meshId <= self.meshStems.size() ? self.meshStems[b->meshId - 1] : "?",
				lod->descVal, descVal);
		}
	}

	if (shadowState.vertexDesc != lod->descVal) {
		shadowState.vertexDesc = lod->descVal;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
		SetDirtyStates(0);
	}

	ctx->IASetIndexBuffer(lod->indexBuffer, DXGI_FORMAT_R16_UINT, 0);

	vbs[0] = lod->vertexBuffer;
	vbs[1] = b->lodCompactedBuf;
	strides[0] = lod->meshStride;
	ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
	ctx->VSSetShaderResources(2, 1, &b->lodExtrasSRV);
	ctx->DrawIndexedInstancedIndirect(b->lodArgsBuf, kArgsByteOffset);
}
