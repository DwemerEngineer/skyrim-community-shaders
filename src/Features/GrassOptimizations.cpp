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
	RenderDistanceOverride,
	EnableOcclusionCulling,
	SimpleShadingPixelSize,
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

	ImGui::SliderFloat(T(TKEY("render_distance_override"), "Grass Render Distance"), &settings.RenderDistanceOverride, 0.0f, 100000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("render_distance_override_tooltip"),
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

	ImGui::SliderFloat(T(TKEY("simple_shading_px"), "Simple Shading Below"), &settings.SimpleShadingPixelSize, 0.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("simple_shading_px_tooltip"),
							  "Grass clumps smaller than this on screen skip pixel-shader detail they are too small to show: contact shadows, specular highlights, and the second texture sample used by complex grass. Base colour, lighting and shadows are unchanged. 0 disables it. Raise until you can see the transition, then back off."));
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
	std::scoped_lock blk(bucketStore.bucketMutex);
	auto* device = globals::d3d::device;
	auto* ctx = globals::d3d::context;

	if (!GetCullCS() || !ctx1 || !cullParamsCB)
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

	maxGrassDistance = settings.RenderDistanceOverride > 0.0f ? settings.RenderDistanceOverride : vanillaMaxDistance;
	maxDistSq = maxGrassDistance * maxGrassDistance;

	bucketStore.BeginFrame({ settings.EnableMeshLOD, timeAccum });
	bucketStore.RefreshComplexGrass(globals::features::grassLighting.settings.ComplexGrassThreshold, ctx);
	bucketStore.ApplyPending(device, ctx);

	RE::NiCamera* cam = RE::Main::WorldRootCamera();
	if (!cam)
		return;

	RE::NiFrustumPlanes frustum{};
	ComputeFrustumPlanes(frustum, cam->GetRuntimeData2().viewFrustum, cam->world);
	const RE::NiPoint3 camPos = cam->world.translate;
	const __m128 camPosV = _mm_setr_ps(camPos.x, camPos.y, camPos.z, 0.0f);
	FrustumSoA frustumSoA;
	BuildFrustumSoA(frustumSoA, frustum);

	const auto [screenW, screenH] = globals::game::renderer->GetScreenSize();

	// Rebuilt before any bucket is culled; the depth copy already holds this frame's statics.
	hiZ.Build(device, ctx);

	{
		CullParamsCB cp{};
		for (int i = 0; i < 6; ++i) {
			cp.frustumPlanes[i][0] = frustum.cullingPlanes[i].normal.x;
			cp.frustumPlanes[i][1] = frustum.cullingPlanes[i].normal.y;
			cp.frustumPlanes[i][2] = frustum.cullingPlanes[i].normal.z;
			cp.frustumPlanes[i][3] = frustum.cullingPlanes[i].constant;
		}

		cp.alphaParam1 = grassStartFadeDistance;
		cp.alphaParam2 = maxGrassDistance;
		cp.fadeNow = timeAccum;
		cp.fadeInTimeRcp = fadeInTimeRcp;
		cp.invisibleFadeCull = settings.InvisibleFadeCull;
		cp.meshLODPixelSize = settings.MeshLODPixelSize;
		cp.meshLODBandPx = std::max(0.0f, settings.MeshLODBandPixels);
		cp.hiZEnabled = hiZ.IsValid() ? 1.0f : 0.0f;
		cp.hiZSizeX = (float)hiZ.GetWidth();
		cp.hiZSizeY = (float)hiZ.GetHeight();
		cp.hiZTexelPixels = (float)HiZPyramid::GetTileSize();
		cp.hiZMipCount = (float)hiZ.GetMipCount();
		cp.simpleShadingPixelSize = std::max(0.0f, settings.SimpleShadingPixelSize);

		cp.maxDistSq = maxDistSq;
		cp.fullDetailPixelSize = settings.FullDetailPixelSize;
		cp.meshCostBias = settings.MeshCostBias;
		cp.lodMinKeep = settings.MinDensity;
		cp.lodFadeBand = 0.15f;
		const auto& vf = cam->GetRuntimeData2().viewFrustum;
		cp.projScale = screenH / (2.0f * std::abs(vf.fTop));
		cp.minPixelSize = settings.MinPixelSize;
		cp.edgeFadeStart = 0.85f;
		cp.collisionDistSq = 2048.0f * 2048.0f;
		cullParamsCB->Update(cp);
	}

	// Coarse per-bucket cull. Sequential by design: a thread pool here is a per-frame fork-join on
	// the render thread whose wake-up latency and contention with the game's job threads cost more
	// than the work itself.
	uint32_t visibleBuckets = 0;
	sliceTableCPU.clear();

	for (auto& [key, b] : bucketStore.buckets) {
		b.cullSlot = UINT32_MAX;
		b.cullVisible = false;
		b.sliceTableCount = 0;
		b.visibleInstances = 0;
		if (!b.totalInstances || !b.instanceSRV)
			continue;

		if (!b.coarseValid)
			bucketStore.UpdateCoarseBounds(b);

		CullBucketSlices(b, frustumSoA, camPosV);

		if (!b.cullVisible)
			continue;

		// Lazy and only for visible buckets with an LOD mesh, so the setting takes effect without
		// a reload and grass types with no authored LOD .nif never pay for the second bin.
		b.lodActive = bucketStore.EnsureLODBin(b, device);
		++visibleBuckets;
	}


	UploadCullState(device, ctx, visibleBuckets);
}

void GrassOptimizations::CullBucketSlices(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV)
{

	// Cheap first test: the union box over every slice. If that misses, nothing in the bucket
	// can be visible.
	b.cullVisible = AabbVisible(frustumSoA,
		_mm_setr_ps(b.coarseMin.x, b.coarseMin.y, b.coarseMin.z, 0.0f),
		_mm_setr_ps(b.coarseMax.x, b.coarseMax.y, b.coarseMax.z, 0.0f));

	b.sliceTableOffset = (uint32_t)sliceTableCPU.size();

	// The union spans every loaded cell of this mesh, so with the camera inside it it is almost
	// always "visible". Refining per run keeps the dispatch to slices that survive, instead of
	// spawning a thread per instance for whole cells behind the camera.
	{
		if (b.cullVisible && b.sliceBounds.size() == b.slices.size()) {
			const __m128 padV = _mm_set1_ps(b.clumpRadius + 64.0f);
			const uint32_t sliceN = (uint32_t)b.slices.size();

			// Rebuilt only when slices change, not per frame.
			if (!b.clustersValid) {
				constexpr float kCell = 4096.0f;
				b.sliceRuns.clear();

				auto cellOf = [](const RE::NiPoint3& o) {
					const int32_t cx = (int32_t)std::floor(o.x / kCell);
					const int32_t cy = (int32_t)std::floor(o.y / kCell);
					return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cy;
				};

				uint32_t i = 0;
				while (i < sliceN) {
					if (b.slices[i].bufferOffset == UINT32_MAX || !b.slices[i].count) {
						++i;
						continue;
					}

					GrassBucket::SliceRun run;
					__m128 rlo = _mm_load_ps(b.sliceBounds[i].lo);
					__m128 rhi = _mm_load_ps(b.sliceBounds[i].hi);
					run.firstOffset = b.slices[i].bufferOffset;
					run.instanceCount = b.slices[i].count;
					const uint64_t cell = cellOf(b.slices[i].origin);
					uint32_t nextOffset = run.firstOffset + b.slices[i].count;
					++i;

					// Extend while the next slice is in the same cell AND continues the buffer
					// range. Requiring contiguity is what lets the whole run become a single
					// (offset, count) entry; if it ever breaks we simply start a new run, so the
					// worst case degrades to one run per slice rather than mis-indexing.
					while (i < sliceN) {
						const BucketSlice& s = b.slices[i];
						if (s.bufferOffset != nextOffset || !s.count ||
							s.bufferOffset == UINT32_MAX || cellOf(s.origin) != cell)
							break;
						rlo = _mm_min_ps(rlo, _mm_load_ps(b.sliceBounds[i].lo));
						rhi = _mm_max_ps(rhi, _mm_load_ps(b.sliceBounds[i].hi));
						run.instanceCount += s.count;
						nextOffset += s.count;
						++i;
					}

					_mm_store_ps(run.lo, rlo);
					_mm_store_ps(run.hi, rhi);
					b.sliceRuns.push_back(run);
				}
				b.clustersValid = true;
			}

			for (const GrassBucket::SliceRun& run : b.sliceRuns) {
				const __m128 lo = _mm_sub_ps(_mm_load_ps(run.lo), padV);
				const __m128 hi = _mm_add_ps(_mm_load_ps(run.hi), padV);

				// Squared distance from the camera to the nearest point of the box. max(lo-c,
				// c-hi, 0) per axis is exact because lo <= hi means at most one term can be
				// positive, and it does all three axes at once with no branches.
				const __m128 d = _mm_max_ps(
					_mm_max_ps(_mm_sub_ps(lo, camPosV), _mm_sub_ps(camPosV, hi)),
					_mm_setzero_ps());
				if (_mm_cvtss_f32(_mm_dp_ps(d, d, 0x71)) > maxDistSq)
					continue;

				if (!AabbVisible(frustumSoA, lo, hi))
					continue;

				// One entry for the whole run. The CS binary-searches on the running total, so
				// fewer, larger entries also shorten its search.
				sliceTableCPU.emplace_back(run.firstOffset, b.visibleInstances);
				++b.sliceTableCount;
				b.visibleInstances += run.instanceCount;
			}
		}
	}

	if (!b.sliceTableCount) {
		b.cullVisible = false;
		b.lodActive = false;
		sliceTableCPU.resize(b.sliceTableOffset);
	}
}

void GrassOptimizations::UploadCullState(ID3D11Device* device, ID3D11DeviceContext* ctx,
	uint32_t visibleBuckets)
{
	// One map fills every visible bucket's slot — replaces a Map/Unmap per bucket.
	if (visibleBuckets && EnsureCullBucketCapacity(visibleBuckets, device)) {
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(cullBucketCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			auto* bytes = static_cast<uint8_t*>(m.pData);
			uint32_t slot = 0;
			for (auto& [key, b] : bucketStore.buckets) {
				if (!b.cullVisible)
					continue;
				b.cullSlot = slot;
				auto* cb = reinterpret_cast<CullBucketCB*>(bytes + (size_t)slot * kSlotBytes);
				cb->instanceCount = b.visibleInstances;
				cb->sliceTableOffset = b.sliceTableOffset;
				cb->sliceCount = b.sliceTableCount;
				cb->wavePeriod = b.wavePeriod;
				cb->timeBase = timeBase;
				cb->prevTimeBase = prevTimeBase;
				cb->boundCenter[0] = b.boundCenter.x;
				cb->boundCenter[1] = b.boundCenter.y;
				cb->boundCenter[2] = b.boundCenter.z;
				cb->clumpRadius = b.clumpRadius;
				cb->distScale = b.distScale;
				cb->minPixelScale = b.minPixelScale;
				cb->isComplex = b.isComplex ? 1.0f : 0.0f;
				cb->lodEnabled = b.lodActive ? 1.0f : 0.0f;
				++slot;
			}
			ctx->Unmap(cullBucketCB->CB(), 0);
		}
	}

	ID3D11Buffer* paramsCB = cullParamsCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &paramsCB);
	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	ctx->CSSetConstantBuffers(12, 1, frameBuffers);

	if (!sliceTableCPU.empty()) {
		if (sliceTableCPU.size() > sliceTableCapacity) {
			sliceTable.reset();
			sliceTableCapacity = 0;

			uint32_t cap = 256;
			while (cap < sliceTableCPU.size())
				cap *= 2;

			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = cap * 2 * sizeof(uint32_t);
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = 2 * sizeof(uint32_t);
			try {
				sliceTable = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::SliceTable");
				D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
				sv.Format = DXGI_FORMAT_UNKNOWN;
				sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				sv.Buffer.NumElements = cap;
				sliceTable->CreateSRV(sv);
				sliceTableCapacity = cap;
			} catch (...) {
				logger::error("[GRASS OPTIMIZATIONS] slice table create failed elements={}", cap);
				sliceTable.reset();
			}
		}

		if (sliceTable && sliceTable->srv) {
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(ctx->Map(sliceTable->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				std::memcpy(m.pData, sliceTableCPU.data(), sliceTableCPU.size() * 2 * sizeof(uint32_t));
				ctx->Unmap(sliceTable->resource.get(), 0);
			}
		}
	}

	ctx->CSSetShader(cullCS, nullptr, 0);

	for (auto& [key, b] : bucketStore.buckets)
		if (b.cullVisible)
			CullBucket(b, ctx);

	ID3D11UnorderedAccessView* nullUAVs[7] = {};
	ctx->CSSetUnorderedAccessViews(0, 7, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	ctx->CSSetShaderResources(0, 4, nullSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);



}

void GrassOptimizations::BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& f)
{
	static constexpr RE::NiFrustumPlanes::ActivePlane kBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	// Slots 6 and 7, and any inactive plane, get a zero normal with constant -1: the dot is then
	// 0 and 0 - (-1) = 1 >= 0, so the slot always passes. Padding this way keeps the inner test
	// completely branch-free instead of testing activePlanes per slice.
	alignas(16) float nx[8], ny[8], nz[8], d[8];
	for (uint32_t i = 0; i < 8; ++i) {
		nx[i] = ny[i] = nz[i] = 0.0f;
		d[i] = -1.0f;
	}

	for (uint32_t i = 0; i < 6; ++i) {
		if (!f.activePlanes.any(kBits[i]))
			continue;
		const auto& pl = f.cullingPlanes[i];
		nx[i] = pl.normal.x;
		ny[i] = pl.normal.y;
		nz[i] = pl.normal.z;
		d[i] = pl.constant;
	}

	for (uint32_t g = 0; g < 2; ++g) {
		out.nx[g] = _mm_load_ps(nx + g * 4);
		out.ny[g] = _mm_load_ps(ny + g * 4);
		out.nz[g] = _mm_load_ps(nz + g * 4);
		out.d[g] = _mm_load_ps(d + g * 4);
	}
}

bool GrassOptimizations::AabbVisible(const FrustumSoA& f, __m128 lo, __m128 hi)
{
	const __m128 lx = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 ly = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 lz = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 hx = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 hy = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 hz = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(2, 2, 2, 2));

	for (uint32_t g = 0; g < 2; ++g) {
		// Positive vertex: the box corner furthest along each plane normal. blendv keys off the
		// normal's sign bit.
		const __m128 px = _mm_blendv_ps(hx, lx, f.nx[g]);
		const __m128 py = _mm_blendv_ps(hy, ly, f.ny[g]);
		const __m128 pz = _mm_blendv_ps(hz, lz, f.nz[g]);

		const __m128 dot = _mm_add_ps(
			_mm_add_ps(_mm_mul_ps(f.nx[g], px), _mm_mul_ps(f.ny[g], py)),
			_mm_mul_ps(f.nz[g], pz));

		// Gamebryo convention: dot(n, p) - constant < 0 → outside
		if (_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(dot, f.d[g]), _mm_setzero_ps())))
			return false;
	}
	return true;
}

void GrassOptimizations::SetupResources()
{
	cullParamsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CullParamsCB>(), "GrassOptimizations::CullParamsCB");
	hiZ.SetupResources();
	bucketStore.SetupResources();

	if (FAILED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) {
		logger::error("[GRASS OPTIMIZATIONS] ID3D11DeviceContext1 unavailable — feature disabled");
		ctx1 = nullptr;
	}
}

void GrassOptimizations::ClearShaderCache()
{
	auto release = [](ID3D11ComputeShader*& shader) {
		if (shader)
			shader->Release();
		shader = nullptr;
	};
	release(cullCS);
	hiZ.ClearShaderCache();
	bucketStore.ClearShaderCache();
}

ID3D11ComputeShader* GrassOptimizations::GetCullCS()
{
	if (!cullCS) {
		cullCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", {}, "cs_5_0"));
		if (!cullCS)
			logger::error("[GRASS OPTIMIZATIONS] cull CS load failed — feature disabled");
	}
	return cullCS;
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

	ID3D11ShaderResourceView* sliceTableSRV = sliceTable ? sliceTable->srv.get() : nullptr;
	ID3D11ShaderResourceView* srvs[4] = { b.instanceSRV, b.originSRV,
		hiZ.GetSRV(), sliceTableSRV };
	ctx->CSSetShaderResources(0, 4, srvs);

	ID3D11Buffer* bucketCB = cullBucketCB->CB();
	UINT first = b.cullSlot * 16;
	UINT num = 16;
	ctx1->CSSetConstantBuffers1(1, 1, &bucketCB, &first, &num);

	// Skipping the dispatch leaves instanceCount at the zero it was just cleared to; the copies
	// below must still run so the fallback args path does not keep last frame's count.
	if (b.visibleInstances && b.sliceTableCount && sliceTableSRV)
		ctx->Dispatch((b.visibleInstances + 63) / 64, 1, 1);

	if (!b.argsUAV) {
		const D3D11_BOX src{ 0, 0, 0, 4, 1, 1 };
		ctx->CopySubresourceRegion(b.argsBuf, 0, kArgsInstanceCountOffset, 0, 0, b.counterBuf, 0, &src);
	}
	if (b.lodActive && !b.lodArgsUAV) {
		const D3D11_BOX src{ 0, 0, 0, 4, 1, 1 };
		ctx->CopySubresourceRegion(b.lodArgsBuf, 0, kArgsInstanceCountOffset, 0, 0, b.lodCounterBuf, 0, &src);
	}
}

bool GrassOptimizations::EnsureCullBucketCapacity(uint32_t slots, [[maybe_unused]] ID3D11Device* device)
{
	if (cullBucketCB && cullBucketCBSlots >= slots)
		return true;

	uint32_t cap = cullBucketCBSlots ? cullBucketCBSlots : 64;
	while (cap < slots)
		cap *= 2;

	cullBucketCB.reset();
	cullBucketCBSlots = 0;

	try {
		cullBucketCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc(cap * kSlotBytes), "GrassOptimizations::CullBucketCB");
	} catch (...) {
		logger::error("[GRASS OPTIMIZATIONS] cull bucket CB create failed slots={}", cap);
		return false;
	}
	cullBucketCBSlots = cap;
	return true;
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_dtor::thunk(RE::BSMultiStreamInstanceTriShape* shape)
{
	globals::features::grassOptimizations.bucketStore.StageRemoval(shape);
	func(shape);
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_OnVisible::thunk(RE::BSMultiStreamInstanceTriShape* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex)
{
	auto prop = This->GetGeometryRuntimeData().shaderProperty;
	if (prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		auto& self = globals::features::grassOptimizations;

		// Not appending is how culling is expressed here, so the engine skips SetupGeometry for
		// this shape — that setup, not the draw, is the real per-shape cost. One shape per bucket
		// suffices since the draw path renders the whole bucket. First to arrive wins rather than
		// a designated shape, which could itself be engine-culled while its siblings are visible.
		GrassBucket* bucket = self.bucketStore.FindBucketForShape(This);
		// A bucket with no instance buffer falls back to vanilla per-shape drawing, so every one of
		// its shapes must still be queued. Checked here rather than when the map is built, so the
		// map can be maintained incrementally without tracking buffer state.
		if (bucket && bucket->instanceBuf) {
			const uint32_t frame = globals::game::graphicsState->frameCount;
			uint32_t prev = bucket->lastQueuedFrame.load(std::memory_order_relaxed);
			if (prev == frame)
				return;  // already queued this frame — the overwhelmingly common path, no write
			if (!bucket->lastQueuedFrame.compare_exchange_strong(prev, frame, std::memory_order_relaxed))
				return;  // another culling thread claimed it first
		}

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
			self.bucketStore.StageCapture(shape, rt.groupAlloc, rt.instanceCount,
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

	// Rebuild SoA only when the frustum actually changed.
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
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3);
	return func(a1, a2, a3);
}

std::uint32_t GrassOptimizations::Hooks::AddQueuedGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4)
{
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3);
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
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader, tl_lastFileInstanceData.data());
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	tl_haveFileGroup = false;
	func(a1, a2);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader, tl_lastFileInstanceData.data());
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

	// On return the game has filled modelPath and returned the shape every instance of this type
	// is added to. The only point where both are available, and it runs before any capture, so
	// recording the pair here gives ResolveMeshId a hit for every shape later drawn.
	if (shape && modelPath)
		globals::features::grassOptimizations.bucketStore.meshLibrary.RecordModelPath(shape, modelPath->c_str());

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
		std::scoped_lock lk(self.bucketStore.bucketMutex);
		// Same identity rule as capture: mesh id when resolvable, else the texture. The per-shape
		// result is cached, so this stays an integer hash lookup on the hot path.
		const uint32_t meshId = self.bucketStore.meshLibrary.ResolveMeshId(geometry);
		auto it = self.bucketStore.buckets.find({ meshId, meshId ? nullptr : diffuseTexture, descVal });
		if (it == self.bucketStore.buckets.end() || !it->second.totalInstances || !it->second.instanceBuf) {
			VanillaDrawInstanceTriShape(geometry);
			return;
		}
		b = &it->second;

		// One bucket already contains every instance of this mesh across every loaded cell, so it
		// only ever needs one draw per pass. Key on the pass, not the geometry — see drawnPassKey.
		uint32_t descriptor = 0;
		if (globals::game::currentPixelShader && *globals::game::currentPixelShader)
			descriptor = (*globals::game::currentPixelShader)->id;
		const uint64_t passKey = (static_cast<uint64_t>(pass->passEnum) << 32) | descriptor;

		if (b->drawnFrame == frame && b->drawnPassKey == passKey)
			return;
		b->drawnFrame = frame;
		b->drawnPassKey = passKey;
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

	// Mesh-swap LOD: second draw for the instances the cull CS routed to bin 1.
	if (!b->lodActive)
		return;

	const GrassMeshLibrary::LODMesh* lod = nullptr;
	{
		std::scoped_lock lk(self.bucketStore.bucketMutex);
		lod = self.bucketStore.meshLibrary.GetLODMesh(b->meshId);
	}
	if (!lod || !lod->vertexBuffer || !lod->indexBuffer)
		return;

	if (!b->lodArgsIndexCountWritten) {
		const D3D11_BOX argBox{ kArgsByteOffset, 0, 0, kArgsByteOffset + sizeof(uint32_t), 1, 1 };
		ctx->UpdateSubresource(b->lodArgsBuf, 0, &argBox, &lod->indexCount, 0, 0);
		b->lodArgsIndexCountWritten = true;
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
