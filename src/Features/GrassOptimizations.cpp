#include "GrassOptimizations.h"
#include "GrassLighting.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"

#define I18N_KEY_PREFIX "feature.grass_optimizations."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassOptimizations::Settings,
	MinPixelSize,
	FullDetailPixelSize,
	MinDensity,
	MeshCostBias,
	CostBiasStartDistance,
	InvisibleFadeCull,
	RenderDistanceOverride,
	EdgeFadeStart,
	EnableOcclusionCulling,
	OcclusionBias,
	SimpleShadingPixelSize,
	CollisionDistance,
	EnableMeshLOD,
	EnableMidLOD,
	MidLODPixelSize,
	EnableFarLOD,
	FarLODPixelSize,
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
							  "Instances whose on-screen radius is above this render at full density. Below it, density is increasingly thinned down to Minimum Density at Min Pixel Size. Increasing this setting improves performance by removing closer grass."));
	}

	ImGui::SliderFloat(T(TKEY("min_pixel_size"), "Min Pixel Size"), &settings.MinPixelSize, 1.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_pixel_size_tooltip"),
							  "Individual grass instances that visibly take up less space on the screen than this are dropped entirely. Higher values improve performance by removing far-away grass instances sooner."));
	}

	Util::PercentageSlider(T(TKEY("min_density"), "Minimum Density"), &settings.MinDensity);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_density_tooltip"),
							  "The percentage of grass that remains at the smallest (Min Pixel Size) LOD level before culling."));
	}

	Util::PercentageSlider(T(TKEY("mesh_cost_bias"), "Mesh Cost Bias"), &settings.MeshCostBias);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_cost_bias_tooltip"),
							  "Culls or removes grass meshes based on their complexity (performance impact). At 0, removal is identical between all grass types regardless of complexity. At 1, heavier and more complex meshes are culled 2-6x sooner than simple ones. Only applies beyond the Cost Bias Start Distance, so nearby grass is never thinned."));
	}

	ImGui::SliderFloat(T(TKEY("cost_bias_start_distance"), "Cost Bias Start Distance"), &settings.CostBiasStartDistance, 0.0f, 20000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("cost_bias_start_distance_tooltip"),
				"Distance at which Mesh Cost Bias starts taking effect, ramping to full over the same distance again. Nearer than this, all grass types are treated identically no matter how complex. Zero applies the bias everywhere, including right in front of the player."),
			Util::Units::FormatDistance(settings.CostBiasStartDistance)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::SliderFloat(T(TKEY("render_distance_override"), "Grass Render Distance"), &settings.RenderDistanceOverride, 0.0f, 100000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("render_distance_override_tooltip"),
							  "Max grass render distance in units. 0 = use the game's INI cap (fGrassStartFadeDistance + fGrassFadeRange). Any grass beyond the vanilla range or this range will be removed."));
	}

	Util::PercentageSlider(T(TKEY("edge_fade_start"), "Edge Fade Start"), &settings.EdgeFadeStart);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("edge_fade_start_tooltip"),
				"Percent of the grass render distance at which grass starts fading out. The default of 0.85 fades over the last 15%. A lower value results in a longer, smoother fade out, while a value of 1.0 disables the fade and grass pops out at the render distance."),
			Util::Units::FormatDistance(maxGrassDistance * settings.EdgeFadeStart)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::SliderFloat(T(TKEY("invisible_fade_cull"), "Invisible Fade Cull"), &settings.InvisibleFadeCull, 0.0f, 0.5f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("invisible_fade_cull_tooltip"),
							  "Skip drawing grass whose transparency is below this threshold. Grass with a fade value of zero is completely invisible and thus is removed early for performance reasons."));
	}

	ImGui::SliderFloat(T(TKEY("simple_shading_px"), "Simple Shading Below"), &settings.SimpleShadingPixelSize, 0.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("simple_shading_px_tooltip"),
							  "Grass instances smaller than this size on screen will skip barely visible detail including contact shadows, specular highlights, and other complex grass visual elements. Zero disables this feature."));
	}

	ImGui::SliderFloat(T(TKEY("collision_distance"), "Collision Distance"), &settings.CollisionDistance, 0.0f, 8192.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("collision_distance_tooltip"),
				"Grass beyond this distance skips any collision detection. Zero disables collision on all grass. Requires the Grass Collision feature."),
			Util::Units::FormatDistance(settings.CollisionDistance)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::Spacing();

	ImGui::Checkbox(T(TKEY("occlusion_culling"), "Occlusion Culling"), &settings.EnableOcclusionCulling);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("occlusion_culling_tooltip"),
							  "Skips grass hidden behind rocks, buildings and NPCs. Depending on how much grass is not visible, this may cost more than its benefits. If you see grass flickering when moving, try disabling this. Terrain such as hills is not treated as an occluder when Terrain Blending is enabled, which reduces the benefit."));
	}

	ImGui::SliderFloat(T(TKEY("occlusion_bias"), "Occlusion Bias"), &settings.OcclusionBias, 0.0f, 0.05f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("occlusion_bias_tooltip"),
							  "How far behind an occluder grass must sit before Occlusion Culling removes it. Raise this if grass disappears around the edges of rocks and hills, lower it to reclaim more performance. Has no effect unless Occlusion Culling is enabled."));
	}

	ImGui::SeparatorText(T(TKEY("mesh_lod"), "Mesh LOD"));

	ImGui::Checkbox(T(TKEY("enable_mesh_lod"), "Enable Mesh LOD"), &settings.EnableMeshLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_mesh_lod_tooltip"),
							  "Improves performance by swapping distant grass instances for a simpler LOD mesh, in two bands. Requires an LOD .nif per grass type at meshes\\LOD\\Grass\\<source-mesh-name>_LOD0.nif, plus an optional _LOD1.nif for the far band. Grass with no LOD mesh keeps its full mesh."));
	}

	ImGui::BeginDisabled(!settings.EnableMeshLOD);

	ImGui::Checkbox(T(TKEY("enable_mid_lod"), "Enable Middle LOD"), &settings.EnableMidLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_mid_lod_tooltip"),
							  "Swaps mid-distance grass to the _LOD0.nif mesh. With this off, grass stays on its full mesh until the far band takes over."));
	}

	ImGui::SliderFloat(T(TKEY("mid_lod_pixel_size"), "Middle LOD Pixel Size"), &settings.MidLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mid_lod_pixel_size_tooltip"),
							  "Instances whose on-screen radius is below this but above the Far LOD Pixel Size swap to the _LOD0.nif mesh."));
	}

	ImGui::Checkbox(T(TKEY("enable_far_lod"), "Enable Far LOD"), &settings.EnableFarLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_far_lod_tooltip"),
							  "Swaps the most distant grass to the _LOD1.nif mesh. Grass types without that file reuse their _LOD0.nif, so the far band still gets its own brightness."));
	}

	ImGui::SliderFloat(T(TKEY("far_lod_pixel_size"), "Far LOD Pixel Size"), &settings.FarLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("far_lod_pixel_size_tooltip"),
							  "Instances whose on-screen radius is below this but above Min Pixel Size swap to the _LOD1.nif mesh. Values above the Middle LOD Pixel Size are clamped to it, since the far band is always the more distant of the two."));
	}

	ImGui::SliderFloat(T(TKEY("mesh_lod_band"), "Mesh LOD Transition Band"), &settings.MeshLODBandPixels, 0.0f, 16.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_lod_band_tooltip"),
							  "The range of on-screen sizes over which a random amount of meshes are swapped out before completely transitioning to the next LOD. Applies to both transitions. A wider range results in a smoother transition."));
	}

	ImGui::EndDisabled();
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
		// SetFrustrumPlanes: s = 1/sqrt(slope²+1); n = fwd*(±slope*s) + axis*(±s)
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

	constexpr float edgePadding = 128.0f;
	out.cullingPlanes[2].constant -= edgePadding;
	out.cullingPlanes[3].constant -= edgePadding;
	out.cullingPlanes[4].constant -= edgePadding;
	out.cullingPlanes[5].constant -= edgePadding;
}

void GrassOptimizations::UpdateGrass()
{
	std::scoped_lock blk(bucketStore.bucketMutex);
	auto* device = globals::d3d::device;
	auto* ctx = globals::d3d::context;

	const bool canCull = GetCullCS() && ctx1 && cullParamsCB;

	timeAccum += globals::game::smState->timerValues[1];
	prevTimeBase = timeBase;
	timeBase = globals::game::smState->timerValues[4] * 0.0016666667f * 6.2831802f;

	const auto iniFloat = [](const char* name, float fallback) {
		auto* setting = RE::GetINISetting(name);
		return setting ? setting->GetFloat() : fallback;
	};
	if (fadeInTimeRcp == 0.0f) {
		const float t = iniFloat("fGrassFadeInTime:Grass", 0.0f);
		fadeInTimeRcp = t > 0.0f ? 1.0f / t : 1e6f;
	}
	if (vanillaMaxDistance == 0.0f) {
		grassStartFadeDistance = iniFloat("fGrassStartFadeDistance:Grass", 6000.0f);
		vanillaMaxDistance = grassStartFadeDistance + iniFloat("fGrassFadeRange:Grass", 2000.0f);
	}

	maxGrassDistance = settings.RenderDistanceOverride > 0.0f ? settings.RenderDistanceOverride : vanillaMaxDistance;
	maxDistSq = maxGrassDistance * maxGrassDistance;

	bucketStore.BeginFrame({ settings.EnableMeshLOD, settings.EnableMidLOD, settings.EnableFarLOD, timeAccum });

	globals::profiler->BeginPass("GrassOptimizations::ApplyPending");
	bucketStore.ApplyPending(device, ctx);
	globals::profiler->EndPass();
	if (!canCull) {
		for (auto& [key, b] : bucketStore.buckets) {
			b.ResetCullState();
			b.optimizedReady.store(false, std::memory_order_relaxed);
		}
		return;
	}

	RE::NiCamera* cam = RE::Main::WorldRootCamera();
	if (!cam) {
		// Leaving last frame's flags up would let the draw path re-issue its indirect draws.
		for (auto& [key, b] : bucketStore.buckets) {
			b.cullVisible = false;
			b.optimizedReady.store(false, std::memory_order_relaxed);
		}
		return;
	}

	RE::NiFrustumPlanes frustum{};
	ComputeFrustumPlanes(frustum, cam->GetRuntimeData2().viewFrustum, cam->world);
	const RE::NiPoint3 camPos = cam->world.translate;
	const __m128 camPosV = _mm_setr_ps(camPos.x, camPos.y, camPos.z, 0.0f);
	FrustumSoA frustumSoA;
	BuildFrustumSoA(frustumSoA, frustum);

	// PerFrame holds the light's camera during shadow passes, so only snapshot it when it matches the
	// main camera. Shadow cameras sit far from the eye, so the tolerance only absorbs eye offsets.
	const auto& frameBuffer = globals::game::frameBufferCached;
	const auto& posAdjust = frameBuffer.GetCameraPosAdjust();
	const RE::NiPoint3 eyeOffset{ posAdjust.x - camPos.x, posAdjust.y - camPos.y, posAdjust.z - camPos.z };
	const bool mainCameraCurrent = eyeOffset.SqrLength() < 64.0f * 64.0f;
	if (mainCameraCurrent || !mainCameraValid) {
		mainCameraViewProj = frameBuffer.GetCameraViewProjUnjittered();
		mainCameraPos = posAdjust;
		mainCameraValid = true;
	}

	// A previous frame's camera is close enough for fading, but not for occlusion.
	if (settings.EnableOcclusionCulling && mainCameraCurrent) {
		hiZ.Build(device, ctx);
	} else {
		hiZ.Invalidate();
	}

	{
		CullParamsCB cp{};
		for (int i = 0; i < 6; ++i) {
			cp.frustumPlanes[i][0] = frustum.cullingPlanes[i].normal.x;
			cp.frustumPlanes[i][1] = frustum.cullingPlanes[i].normal.y;
			cp.frustumPlanes[i][2] = frustum.cullingPlanes[i].normal.z;
			cp.frustumPlanes[i][3] = frustum.cullingPlanes[i].constant;
		}
		std::memcpy(cp.cameraViewProj, &mainCameraViewProj, sizeof(cp.cameraViewProj));
		cp.cameraPos[0] = mainCameraPos.x;
		cp.cameraPos[1] = mainCameraPos.y;
		cp.cameraPos[2] = mainCameraPos.z;

		cp.minPixelSize = settings.MinPixelSize;
		cp.fullDetailPixelSize = settings.FullDetailPixelSize;
		cp.lodMinKeep = settings.MinDensity;
		cp.lodFadeBand = 0.15f;

		const auto& vf = cam->GetRuntimeData2().viewFrustum;
		const auto [screenW, screenH] = globals::game::renderer->GetScreenSize();
		cp.meshCostBias = settings.MeshCostBias;
		cp.projScale = screenH / (2.0f * std::abs(vf.fTop));
		cp.maxDistSq = maxDistSq;
		cp.edgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 1.0f);

		cp.alphaParam1 = grassStartFadeDistance;
		cp.alphaParam2 = maxGrassDistance;
		cp.fadeNow = timeAccum;
		cp.fadeInTimeRcp = fadeInTimeRcp;

		const float collisionDist = std::max(0.0f, settings.CollisionDistance);
		cp.invisibleFadeCull = settings.InvisibleFadeCull;
		cp.simpleShadingPixelSize = std::max(0.0f, settings.SimpleShadingPixelSize);
		cp.collisionDistSq = collisionDist * collisionDist;
		cp.midLODPixelSize = settings.MidLODPixelSize;
		cp.farLODPixelSize = settings.EnableMidLOD ? std::min(settings.FarLODPixelSize, settings.MidLODPixelSize) : settings.FarLODPixelSize;

		cp.meshLODBandPx = std::max(0.0f, settings.MeshLODBandPixels);
		cp.hiZEnabled = hiZ.IsValid() ? 1.0f : 0.0f;
		cp.hiZSizeX = (float)hiZ.GetWidth();
		cp.hiZSizeY = (float)hiZ.GetHeight();

		cp.hiZTexelPixels = hiZ.GetTexelPixels();
		cp.hiZMipCount = (float)hiZ.GetMipCount();
		cp.occlusionBias = std::max(0.0f, settings.OcclusionBias);
		cp.costBiasStartDist = std::max(0.0f, settings.CostBiasStartDistance);

		cullParamsCB->Update(cp);
	}

	visibleBuckets.clear();
	sliceTableCPU.clear();

	globals::profiler->BeginPass("GrassOptimizations::SliceCull");
	for (auto& [key, b] : bucketStore.buckets) {
		b.ResetCullState();
		b.optimizedReady.store(false, std::memory_order_relaxed);
		if (!b.totalInstances || !b.instanceSRV || b.dirty || b.firstNewSlice != UINT32_MAX)
			continue;

		if (!b.coarseValid)
			bucketStore.UpdateCoarseBounds(b);

		CullBucketSlices(b, frustumSoA, camPosV);
		b.optimizedReady.store(true, std::memory_order_relaxed);

		if (!b.cullVisible)
			continue;

		bucketStore.RefreshComplexGrass(b, ctx);

		if (!bucketStore.UploadCullGroups(b, device, ctx)) {
			// Without its groups the bucket cannot be culled, so draw it the vanilla way.
			sliceTableCPU.resize(b.sliceTableOffset);
			b.ResetCullState();
			b.optimizedReady.store(false, std::memory_order_relaxed);
			continue;
		}

		for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier)
			b.lodBins[tier].active = bucketStore.EnsureLODBin(b, (GrassMeshLibrary::LODTier)tier, device);
		visibleBuckets.push_back(&b);
	}
	globals::profiler->EndPass();

	globals::profiler->BeginPass("GrassOptimizations::InstanceCull");
	UploadCullState(device, ctx);
	globals::profiler->EndPass();
}

void GrassOptimizations::CullBucketSlices(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV)
{
	b.sliceTableOffset = (uint32_t)sliceTableCPU.size();
	b.sliceTableCount = 0;
	b.groupCount = 0;
	b.cullVisible = false;
	for (GrassBucket::LODBin& bin : b.lodBins)
		bin.active = false;

	if (b.sliceBounds.size() != b.slices.size()) {
		return;
	}

	const __m128 bucketLo = _mm_setr_ps(b.coarseMin.x, b.coarseMin.y, b.coarseMin.z, 0.0f);
	const __m128 bucketHi = _mm_setr_ps(b.coarseMax.x, b.coarseMax.y, b.coarseMax.z, 0.0f);
	const __m128 zero = _mm_setzero_ps();
	const __m128 nearest = _mm_max_ps(_mm_max_ps(_mm_sub_ps(bucketLo, camPosV), _mm_sub_ps(camPosV, bucketHi)), zero);
	if (_mm_cvtss_f32(_mm_dp_ps(nearest, nearest, 0x71)) > maxDistSq) {
		return;
	}

	uint32_t planeMask;
	if (!ClassifyAabb(frustumSoA, bucketLo, bucketHi, planeMask)) {
		return;
	}

	if (!b.clustersValid || b.clusteredSlices != b.slices.size())
		bucketStore.MergeSlicesIntoRuns(b);

	const __m128 sign = _mm_set1_ps(-0.0f);
	const __m128 farthest = _mm_max_ps(_mm_andnot_ps(sign, _mm_sub_ps(bucketLo, camPosV)), _mm_andnot_ps(sign, _mm_sub_ps(bucketHi, camPosV)));
	const bool allWithinDistance = _mm_cvtss_f32(_mm_dp_ps(farthest, farthest, 0x71)) <= maxDistSq;
	if (!planeMask && allWithinDistance) {
		b.groupCount = (uint32_t)b.cullGroups.size();
		if (b.groupCount) {
			// Every run is visible, and its groups are contiguous in their original order.
			sliceTableCPU.emplace_back(0, 0);
			b.sliceTableCount = 1;
			b.cullVisible = true;
		}
		return;
	}

	if (planeMask && b.sliceBlocksUseful) {
		CullBucketSliceBlocks(b, frustumSoA, camPosV, planeMask, allWithinDistance);
	} else {
		const __m128 camX = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(0, 0, 0, 0));
		const __m128 camY = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(1, 1, 1, 1));
		const __m128 camZ = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(2, 2, 2, 2));
		const __m128 maxDistance = _mm_set1_ps(maxDistSq);
		for (uint32_t firstRun = 0; firstRun < b.sliceRunCount; firstRun += 4) {
			const auto& batch = b.sliceRunBatches[firstRun / 4];
			const __m128 lx = _mm_load_ps(batch.loX), ly = _mm_load_ps(batch.loY), lz = _mm_load_ps(batch.loZ);
			const __m128 hx = _mm_load_ps(batch.hiX), hy = _mm_load_ps(batch.hiY), hz = _mm_load_ps(batch.hiZ);
			uint32_t visibleMask = (1u << std::min(4u, b.sliceRunCount - firstRun)) - 1u;
			if (!allWithinDistance) {
				const __m128 dx = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lx, camX), _mm_sub_ps(camX, hx)), zero);
				const __m128 dy = _mm_max_ps(_mm_max_ps(_mm_sub_ps(ly, camY), _mm_sub_ps(camY, hy)), zero);
				const __m128 dz = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lz, camZ), _mm_sub_ps(camZ, hz)), zero);
				const __m128 distanceSq = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz));
				visibleMask &= _mm_movemask_ps(_mm_cmple_ps(distanceSq, maxDistance));
			}

			uint32_t remainingPlanes = planeMask;
			while (visibleMask && remainingPlanes) {
				unsigned long planeIndex;
				_BitScanForward(&planeIndex, remainingPlanes);
				remainingPlanes &= remainingPlanes - 1;
				const auto& plane = frustumSoA.planes[planeIndex];
				const __m128 px = _mm_blendv_ps(hx, lx, plane.nx);
				const __m128 py = _mm_blendv_ps(hy, ly, plane.ny);
				const __m128 pz = _mm_blendv_ps(hz, lz, plane.nz);
				const __m128 dot = _mm_add_ps(_mm_add_ps(_mm_mul_ps(plane.nx, px), _mm_mul_ps(plane.ny, py)), _mm_mul_ps(plane.nz, pz));
				visibleMask &= ~_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(dot, plane.d), zero));
			}

			while (visibleMask) {
				unsigned long lane;
				_BitScanForward(&lane, visibleMask);
				visibleMask &= visibleMask - 1;
				// Consume lanes in order so adjacent visible groups still coalesce.
				const bool extendsLast = b.sliceTableCount &&
				                         sliceTableCPU.back().first + (b.groupCount - sliceTableCPU.back().second) == batch.firstGroup[lane];
				if (!extendsLast) {
					sliceTableCPU.emplace_back(batch.firstGroup[lane], b.groupCount);
					++b.sliceTableCount;
				}
				b.groupCount += batch.groupCount[lane];
			}
		}
	}

	b.cullVisible = b.sliceTableCount != 0;
	if (!b.cullVisible)
		sliceTableCPU.resize(b.sliceTableOffset);
}

void GrassOptimizations::CullBucketSliceBlocks(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV, uint32_t planeMask, bool allWithinDistance)
{
	const __m128 zero = _mm_setzero_ps();
	const __m128 sign = _mm_set1_ps(-0.0f);
	const __m128 camX = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 camY = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 camZ = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 maxDistance = _mm_set1_ps(maxDistSq);
	const auto appendGroups = [&](uint32_t firstGroup, uint32_t groupCount) {
		const bool extendsLast = b.sliceTableCount &&
		                         sliceTableCPU.back().first + (b.groupCount - sliceTableCPU.back().second) == firstGroup;
		if (!extendsLast) {
			sliceTableCPU.emplace_back(firstGroup, b.groupCount);
			++b.sliceTableCount;
		}
		b.groupCount += groupCount;
	};

	const auto cullRuns = [&](uint32_t firstRun, uint32_t endRun, uint32_t testPlanes, bool withinDistance) {
		for (; firstRun < endRun; firstRun += 4) {
			const auto& batch = b.sliceRunBatches[firstRun / 4];
			const __m128 lx = _mm_load_ps(batch.loX), ly = _mm_load_ps(batch.loY), lz = _mm_load_ps(batch.loZ);
			const __m128 hx = _mm_load_ps(batch.hiX), hy = _mm_load_ps(batch.hiY), hz = _mm_load_ps(batch.hiZ);
			uint32_t visibleMask = (1u << std::min(4u, endRun - firstRun)) - 1u;
			if (!withinDistance) {
				const __m128 dx = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lx, camX), _mm_sub_ps(camX, hx)), zero);
				const __m128 dy = _mm_max_ps(_mm_max_ps(_mm_sub_ps(ly, camY), _mm_sub_ps(camY, hy)), zero);
				const __m128 dz = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lz, camZ), _mm_sub_ps(camZ, hz)), zero);
				const __m128 distanceSq = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz));
				visibleMask &= _mm_movemask_ps(_mm_cmple_ps(distanceSq, maxDistance));
			}

			uint32_t remainingPlanes = testPlanes;
			while (visibleMask && remainingPlanes) {
				unsigned long planeIndex;
				_BitScanForward(&planeIndex, remainingPlanes);
				remainingPlanes &= remainingPlanes - 1;
				const auto& plane = frustumSoA.planes[planeIndex];
				const __m128 px = _mm_blendv_ps(hx, lx, plane.nx);
				const __m128 py = _mm_blendv_ps(hy, ly, plane.ny);
				const __m128 pz = _mm_blendv_ps(hz, lz, plane.nz);
				const __m128 dot = _mm_add_ps(_mm_add_ps(_mm_mul_ps(plane.nx, px), _mm_mul_ps(plane.ny, py)), _mm_mul_ps(plane.nz, pz));
				visibleMask &= ~_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(dot, plane.d), zero));
			}

			while (visibleMask) {
				unsigned long lane;
				_BitScanForward(&lane, visibleMask);
				visibleMask &= visibleMask - 1;
				appendGroups(batch.firstGroup[lane], batch.groupCount[lane]);
			}
		}
	};

	const uint32_t blockCount = (b.sliceRunCount + GrassBucket::kRunsPerCullBlock - 1) / GrassBucket::kRunsPerCullBlock;
	for (uint32_t firstBlock = 0; firstBlock < blockCount; firstBlock += 4) {
		const auto& batch = b.sliceBlockBatches[firstBlock / 4];
		const __m128 lx = _mm_load_ps(batch.loX), ly = _mm_load_ps(batch.loY), lz = _mm_load_ps(batch.loZ);
		const __m128 hx = _mm_load_ps(batch.hiX), hy = _mm_load_ps(batch.hiY), hz = _mm_load_ps(batch.hiZ);
		uint32_t visibleMask = (1u << std::min(4u, blockCount - firstBlock)) - 1u;
		uint32_t withinDistanceMask = visibleMask;
		if (!allWithinDistance) {
			const __m128 dx = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lx, camX), _mm_sub_ps(camX, hx)), zero);
			const __m128 dy = _mm_max_ps(_mm_max_ps(_mm_sub_ps(ly, camY), _mm_sub_ps(camY, hy)), zero);
			const __m128 dz = _mm_max_ps(_mm_max_ps(_mm_sub_ps(lz, camZ), _mm_sub_ps(camZ, hz)), zero);
			const __m128 distanceSq = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz));
			visibleMask &= _mm_movemask_ps(_mm_cmple_ps(distanceSq, maxDistance));
			if (!visibleMask)
				continue;

			const __m128 fx = _mm_max_ps(_mm_andnot_ps(sign, _mm_sub_ps(lx, camX)), _mm_andnot_ps(sign, _mm_sub_ps(hx, camX)));
			const __m128 fy = _mm_max_ps(_mm_andnot_ps(sign, _mm_sub_ps(ly, camY)), _mm_andnot_ps(sign, _mm_sub_ps(hy, camY)));
			const __m128 fz = _mm_max_ps(_mm_andnot_ps(sign, _mm_sub_ps(lz, camZ)), _mm_andnot_ps(sign, _mm_sub_ps(hz, camZ)));
			const __m128 farthestSq = _mm_add_ps(_mm_add_ps(_mm_mul_ps(fx, fx), _mm_mul_ps(fy, fy)), _mm_mul_ps(fz, fz));
			withinDistanceMask = _mm_movemask_ps(_mm_cmple_ps(farthestSq, maxDistance));
		}

		__m128i childPlanes = _mm_set1_epi32(planeMask);
		uint32_t remainingPlanes = planeMask;
		while (visibleMask && remainingPlanes) {
			unsigned long planeIndex;
			_BitScanForward(&planeIndex, remainingPlanes);
			remainingPlanes &= remainingPlanes - 1;
			const auto& plane = frustumSoA.planes[planeIndex];
			const __m128 px = _mm_blendv_ps(hx, lx, plane.nx);
			const __m128 py = _mm_blendv_ps(hy, ly, plane.ny);
			const __m128 pz = _mm_blendv_ps(hz, lz, plane.nz);
			const __m128 positiveDot = _mm_add_ps(_mm_add_ps(_mm_mul_ps(plane.nx, px), _mm_mul_ps(plane.ny, py)), _mm_mul_ps(plane.nz, pz));
			visibleMask &= ~_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(positiveDot, plane.d), zero));

			const __m128 nx = _mm_blendv_ps(lx, hx, plane.nx);
			const __m128 ny = _mm_blendv_ps(ly, hy, plane.ny);
			const __m128 nz = _mm_blendv_ps(lz, hz, plane.nz);
			const __m128 negativeDot = _mm_add_ps(_mm_add_ps(_mm_mul_ps(plane.nx, nx), _mm_mul_ps(plane.ny, ny)), _mm_mul_ps(plane.nz, nz));
			const __m128i inside = _mm_castps_si128(_mm_cmpge_ps(_mm_sub_ps(negativeDot, plane.d), zero));
			childPlanes = _mm_andnot_si128(_mm_and_si128(inside, _mm_set1_epi32(1u << planeIndex)), childPlanes);
		}

		alignas(16) uint32_t childPlaneMasks[4];
		_mm_store_si128((__m128i*)childPlaneMasks, childPlanes);
		while (visibleMask) {
			unsigned long lane;
			_BitScanForward(&lane, visibleMask);
			visibleMask &= visibleMask - 1;
			const bool withinDistance = (withinDistanceMask & (1u << lane)) != 0;
			if (!childPlaneMasks[lane] && withinDistance) {
				appendGroups(batch.firstGroup[lane], batch.groupCount[lane]);
			} else {
				const uint32_t firstRun = (firstBlock + lane) * GrassBucket::kRunsPerCullBlock;
				cullRuns(firstRun, std::min(firstRun + GrassBucket::kRunsPerCullBlock, b.sliceRunCount), childPlaneMasks[lane], withinDistance);
			}
		}
	}
}

void GrassOptimizations::UploadCullState(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	// One map fills every visible bucket's slot — replaces a Map/Unmap per bucket.
	bool cullStateUploaded = false;
	if (!visibleBuckets.empty() &&
		EnsureCullBucketCapacity((uint32_t)visibleBuckets.size()) &&
		EnsureDrawArgsCapacity((uint32_t)visibleBuckets.size(), device)) {
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(cullBucketCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			auto* bytes = static_cast<uint8_t*>(m.pData);
			uint32_t slot = 0;
			for (GrassBucket* b : visibleBuckets) {
				b->cullSlot = slot;
				auto* cb = reinterpret_cast<CullBucketCB*>(bytes + (size_t)slot * kSlotBytes);
				cb->groupCount = b->groupCount;
				cb->sliceTableOffset = b->sliceTableOffset;
				cb->sliceCount = b->sliceTableCount;
				cb->dispatchWidth = std::min(b->groupCount, (uint32_t)D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION);
				cb->wavePeriod = b->wavePeriod;
				cb->timeBase = timeBase;
				cb->prevTimeBase = prevTimeBase;
				cb->boundCenter[0] = b->boundCenter.x;
				cb->boundCenter[1] = b->boundCenter.y;
				cb->boundCenter[2] = b->boundCenter.z;
				cb->modelRadius = b->modelRadius;
				cb->distScale = b->distScale;
				cb->minPixelScale = b->minPixelScale;
				cb->complexThreshold = globals::features::grassLighting.settings.ComplexGrassThreshold;
				cb->midLODEnabled = b->lodBins[(size_t)GrassMeshLibrary::LODTier::kMiddle].active ? 1.0f : 0.0f;
				cb->farLODEnabled = b->lodBins[(size_t)GrassMeshLibrary::LODTier::kFar].active ? 1.0f : 0.0f;
				if (b->pbrMaterial) {
					auto* material = reinterpret_cast<TruePBR::GrassPBRConstants*>(bytes + (size_t)slot * kSlotBytes + kSlotMaterialOffset);
					*material = TruePBR::GetGrassPBRConstants(static_cast<const BSLightingShaderMaterialPBR*>(b->material.get()));
				}
				++slot;
			}
			ctx->Unmap(cullBucketCB->CB(), 0);
			cullStateUploaded = true;
		}
	}

	// If the cull state failed to upload, skip all buckets to prevent the CS from running using garbage or out-of-date data.
	if (!visibleBuckets.empty() && !cullStateUploaded) {
		for (auto& [key, b] : bucketStore.buckets) {
			b.cullVisible = false;
			b.optimizedReady.store(false, std::memory_order_relaxed);
		}
		visibleBuckets.clear();
	}

	ID3D11Buffer* paramsCB = cullParamsCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &paramsCB);

	bool sliceTableUploaded = sliceTableCPU.empty();
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
				sliceTableUploaded = true;
			}
		}
	}

	if (!sliceTableUploaded) {
		for (auto& [key, b] : bucketStore.buckets) {
			b.cullVisible = false;
			b.optimizedReady.store(false, std::memory_order_relaxed);
		}
		visibleBuckets.clear();
	}

	ctx->CSSetShader(cullCS, nullptr, 0);

	// Upload complete args records in one command instead of clearing a count UAV per bucket.
	// The count windows preserve the cull shader's byte-zero addressing within each record.
	if (!visibleBuckets.empty()) {
		UploadDrawArgs(ctx);
	}

	// Only active LOD bins read these separate counters.
	const UINT zeros[4] = { 0, 0, 0, 0 };
	for (GrassBucket* b : visibleBuckets) {
		if (std::ranges::any_of(b->lodBins, [](const GrassBucket::LODBin& bin) { return bin.active; }))
			ctx->ClearUnorderedAccessViewUint(b->lodCounterUAV, zeros);
	}

	for (GrassBucket* b : visibleBuckets)
		CullBucket(*b, ctx);

	ID3D11UnorderedAccessView* nullUAVs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);

	// Copy the LOD counts once every dispatch is queued; copying after each one stalls the next.
	for (GrassBucket* b : visibleBuckets) {
		for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
			const GrassBucket::LODBin& bin = b->lodBins[tier];
			if (!bin.active || !bin.argsBuf)
				continue;
			const UINT countOffset = (UINT)(tier * sizeof(uint32_t));
			const D3D11_BOX countBox{ countOffset, 0, 0, static_cast<UINT>(countOffset + sizeof(uint32_t)), 1, 1 };
			ctx->CopySubresourceRegion(bin.argsBuf, 0, instanceCountOffset, 0, 0, b->lodCounterBuf, 0, &countBox);
		}
	}

	ID3D11ShaderResourceView* nullSRVs[5] = {};
	ctx->CSSetShaderResources(0, 5, nullSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);

	// Removals at Present may destroy these buckets before the next frame.
	visibleBuckets.clear();
}

void GrassOptimizations::BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& f)
{
	static constexpr RE::NiFrustumPlanes::ActivePlane kBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	// Pad unused and inactive slots with an always-pass plane (zero normal, constant -1), keeping the per-slice test branch-free.
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
	for (uint32_t i = 0; i < 6; ++i) {
		out.planes[i] = { _mm_set1_ps(nx[i]), _mm_set1_ps(ny[i]), _mm_set1_ps(nz[i]), _mm_set1_ps(d[i]) };
	}
}

bool GrassOptimizations::ClassifyAabb(const FrustumSoA& f, __m128 lo, __m128 hi, uint32_t& intersectingPlanes)
{
	intersectingPlanes = 0;
	const __m128 lx = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 ly = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 lz = _mm_shuffle_ps(lo, lo, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 hx = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 hy = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 hz = _mm_shuffle_ps(hi, hi, _MM_SHUFFLE(2, 2, 2, 2));

	for (uint32_t g = 0; g < 2; ++g) {
		// Positive vertex: the box corner furthest along each plane normal.
		// blendv keys off the normal's sign bit.
		const __m128 px = _mm_blendv_ps(hx, lx, f.nx[g]);
		const __m128 py = _mm_blendv_ps(hy, ly, f.ny[g]);
		const __m128 pz = _mm_blendv_ps(hz, lz, f.nz[g]);

		const __m128 dot = _mm_add_ps(
			_mm_add_ps(_mm_mul_ps(f.nx[g], px), _mm_mul_ps(f.ny[g], py)),
			_mm_mul_ps(f.nz[g], pz));

		// dot(n, p) - constant < 0 → outside
		if (_mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(dot, f.d[g]), _mm_setzero_ps())))
			return false;

		// A plane containing the negative vertex contains the whole coarse box.
		const __m128 qx = _mm_blendv_ps(lx, hx, f.nx[g]);
		const __m128 qy = _mm_blendv_ps(ly, hy, f.ny[g]);
		const __m128 qz = _mm_blendv_ps(lz, hz, f.nz[g]);
		const __m128 minimum = _mm_add_ps(_mm_add_ps(_mm_mul_ps(f.nx[g], qx), _mm_mul_ps(f.ny[g], qy)), _mm_mul_ps(f.nz[g], qz));
		const uint32_t inside = _mm_movemask_ps(_mm_cmpge_ps(_mm_sub_ps(minimum, f.d[g]), _mm_setzero_ps()));
		intersectingPlanes |= ((~inside) & 0xFu) << (g * 4);
	}
	intersectingPlanes &= 0x3Fu;
	return true;
}

void GrassOptimizations::SetupResources()
{
	cullParamsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CullParamsCB>(), "GrassOptimizations::CullParamsCB");
	perGeometryCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc((kMaxPerGeometryVariants + 1) * kPerGeometrySlotBytes), "GrassOptimizations::PerGeometryCB");
	hiZ.SetupResources();
	bucketStore.SetupResources();

	if (FAILED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) {
		logger::error("[GRASS OPTIMIZATIONS] ID3D11DeviceContext1 unavailable — feature disabled");
		ctx1 = nullptr;
	}
}

void GrassOptimizations::Reset()
{
	std::scoped_lock lk(bucketStore.bucketMutex);
	bucketStore.ProcessRemovals();
	bucketStore.meshLibrary.CommitLoadedLODMeshes();
}

void GrassOptimizations::ClearShaderCache()
{
	auto release = [](ID3D11ComputeShader*& shader) {
		if (shader)
			shader->Release();
		shader = nullptr;
	};
	release(cullCS);
	cullCSFailed = false;
	hiZ.ClearShaderCache();
	bucketStore.ClearShaderCache();
}

ID3D11ComputeShader* GrassOptimizations::GetCullCS()
{
	if (!cullCS && !cullCSFailed) {
		cullCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", {}, "cs_5_0"));
		if (!cullCS) {
			cullCSFailed = true;
			logger::error("[GRASS OPTIMIZATIONS] cull CS load failed — feature disabled");
		}
	}
	return cullCS;
}

void GrassOptimizations::CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	static_assert(
		(size_t)GrassMeshLibrary::LODTier::kMiddle == 0 &&
			(size_t)GrassMeshLibrary::LODTier::kFar == 1 &&
			(size_t)GrassMeshLibrary::LODTier::kCount == 2,
		"GrassCullingCS LOD counter offsets must match LODTier");

	if (b.cullSlot == UINT32_MAX)
		return;

	// Main outputs, two outputs per LOD tier, then the shared LOD counter, matching u0-u7 in GrassCullingCS.
	ID3D11UnorderedAccessView* uavs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = { b.compactedUAV, b.extrasUAV, drawCountUAVs[b.cullSlot].get() };
	for (size_t tier = 0; tier < (size_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		const GrassBucket::LODBin& bin = b.lodBins[tier];
		uavs[3 + tier * 2 + 0] = bin.active ? bin.compactedUAV : nullptr;
		uavs[3 + tier * 2 + 1] = bin.active ? bin.extrasUAV : nullptr;
	}
	uavs[std::size(uavs) - 1] = b.lodCounterUAV;
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);

	ID3D11ShaderResourceView* sliceTableSRV = sliceTable ? sliceTable->srv.get() : nullptr;
	ID3D11ShaderResourceView* srvs[5] = { b.instanceSRV, b.groupSRV,
		hiZ.GetSRV(), sliceTableSRV, b.complexSRV };
	ctx->CSSetShaderResources(0, 5, srvs);

	ID3D11Buffer* bucketCB = cullBucketCB->CB();
	UINT first = b.cullSlot * (kSlotBytes / 16);
	UINT num = 16;
	ctx1->CSSetConstantBuffers1(1, 1, &bucketCB, &first, &num);

	// Skipping the dispatch keeps the instance count at zero for the draw.
	if (b.groupCount && b.sliceTableCount && sliceTableSRV) {
		// Large buckets continue on further rows, staying under the per-dimension group limit.
		const UINT width = std::min(b.groupCount, (uint32_t)D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION);
		ctx->Dispatch(width, (b.groupCount + width - 1) / width, 1);
	}
}

void GrassOptimizations::UploadDrawArgs(ID3D11DeviceContext* ctx)
{
	drawArgsCPU.assign(visibleBuckets.size(), {});
	for (const GrassBucket* b : visibleBuckets)
		drawArgsCPU[b->cullSlot][argsByteOffset / sizeof(uint32_t)] = b->indexCount;
	const D3D11_BOX argsBox{ 0, 0, 0, (UINT)(drawArgsCPU.size() * argsSlotBytes), 1, 1 };
	ctx->UpdateSubresource(drawArgs->resource.get(), 0, &argsBox, drawArgsCPU.data(), 0, 0);
}

bool GrassOptimizations::EnsureDrawArgsCapacity(uint32_t slots, ID3D11Device* device)
{
	if (drawArgs && drawCountUAVs.size() >= slots)
		return true;

	constexpr uint32_t kMaxSlots = UINT32_MAX / argsSlotBytes;
	if (slots > kMaxSlots)
		return false;
	uint32_t cap = drawCountUAVs.empty() ? 64u : (uint32_t)drawCountUAVs.size();
	while (cap < slots)
		cap = std::min(cap * 2, kMaxSlots);

	try {
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * argsSlotBytes;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		auto buffer = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::DrawArgs");
		std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> views(cap);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = 1;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		for (uint32_t slot = 0; slot < cap; ++slot) {
			uav.Buffer.FirstElement = (slot * argsSlotBytes + instanceCountOffset) / sizeof(uint32_t);
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(buffer->resource.get(), &uav, views[slot].put()));
			Util::SetResourceName(views[slot].get(), "GrassOptimizations::DrawCount UAV");
		}
		drawCountUAVs = std::move(views);
		drawArgs = std::move(buffer);
	} catch (...) {
		logger::error("[GRASS OPTIMIZATIONS] shared draw args create failed slots={}", cap);
		return false;
	}
	return true;
}

bool GrassOptimizations::EnsureCullBucketCapacity(uint32_t slots)
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

		// Only queue one representative shape per frame for each bucket to skip redundant setup.
		if (!self.bucketStore.ClaimQueueSlot(This, globals::game::graphicsState->frameCount))
			return;

		// Skips redundant and costly frustum checks since they are now handled by the coarse slice cull and CS.
		auto& shape = *This;
		process->AppendVirtual(shape, alphaGroupIndex);
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

	auto* shadowState = globals::game::shadowState;
	shadowState->SetPSTextureAddressMode(0, RE::BSGraphics::TextureAddressMode::kClampSClampT);
	shadowState->SetPSTextureFilterMode(0, RE::BSGraphics::TextureFilterMode::kAnisotropic);
}

void GrassOptimizations::Hooks::BSGrassShader_SetupMaterial::thunk([[maybe_unused]] RE::BSShader* This, const RE::BSLightingShaderMaterialBase* material)
{
	globals::features::grassOptimizations.pendingMaterial = material;
}

bool GrassOptimizations::SetupGrassPass(RE::BSShader* shader, RE::BSRenderPass* pass)
{
	const auto* material = std::exchange(pendingMaterial, nullptr);

	if (loaded && SetupOptimizedPass(pass))
		return true;

	if (material)
		Hooks::BSGrassShader_SetupMaterial::func(shader, material);
	return false;
}

bool GrassOptimizations::SetupOptimizedPass(RE::BSRenderPass* pass)
{
	auto* geometry = pass->geometry;
	auto* shaderProperty = pass->shaderProperty;
	if (!geometry || !shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get())
		return false;
	auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(shaderProperty->material);
	if (!material || !material->diffuseTexture || !material->diffuseTexture->rendererTexture)
		return false;

	auto* smState = globals::game::smState;
	auto* sceneNode = smState->shadowSceneNode[0];
	auto* sunLight = sceneNode && sceneNode->GetRuntimeData().sunLight ? skyrim_cast<RE::NiDirectionalLight*>(sceneNode->GetRuntimeData().sunLight->light.get()) : nullptr;
	if (smState->liteBrite || !sunLight)
		return false;

	const bool pbrTechnique = static_cast<SIE::ShaderCache::GrassShaderTechniques>(globals::state->currentPixelDescriptor & 0b1111) ==
	                          SIE::ShaderCache::GrassShaderTechniques::TruePbr;

	std::scoped_lock lk(bucketStore.bucketMutex);
	auto* b = bucketStore.FindBucket(static_cast<RE::BSMultiStreamInstanceTriShape*>(geometry));
	if (!b || !b->totalInstances || !b->optimizedReady.load(std::memory_order_relaxed) || (pbrTechnique && !b->pbrMaterial))
		return false;

	// The draw hook skips this pass, so it needs no setup.
	if (!b->cullVisible || (b->drawnFrame == globals::game::graphicsState->frameCount && b->drawnPassKey == GetPassKey(pass)))
		return true;

	if (!BindPerGeometry(geometry, shaderProperty, sunLight))
		return false;
	auto* shadowState = globals::game::shadowState;
	shadowState->SetPSTexture(0, material->diffuseTexture->rendererTexture);
	shadowState->SetPSTextureAddressMode(0, RE::BSGraphics::TextureAddressMode::kClampSClampT);
	if (pbrTechnique) {
		TruePBR::SetGrassPBRTextures(static_cast<const BSLightingShaderMaterialPBR*>(material));

		ID3D11Buffer* bucketCB = cullBucketCB->CB();
		UINT first = (b->cullSlot * kSlotBytes + kSlotMaterialOffset) / 16;
		UINT num = 16;
		ctx1->PSSetConstantBuffers1(1, 1, &bucketCB, &first, &num);
	}
	return true;
}

bool GrassOptimizations::BindPerGeometry(const RE::BSGeometry* geometry, const RE::BSShaderProperty* shaderProperty, RE::NiDirectionalLight* sunLight)
{
	// Engine globals vanilla SetupGeometry reads. SetFogNearColor writes the fog colour to its PerGeometry copy at c16.
	static const auto* fogNearColor = reinterpret_cast<const float*>(REL::RelocationID(527805, 414756).address() + 0x100);
	static const auto* grassWindAmount = reinterpret_cast<const float*>(REL::RelocationID(527802, 414753).address());
	static const auto* grassWindAmountMax = reinterpret_cast<const float*>(REL::RelocationID(504665, 373550).address());
	static const auto* windGrassMultiplier = reinterpret_cast<const float*>(REL::RelocationID(513337, 391112).address());

	if (!perGeometryCB || !ctx1)
		return false;

	GrassPerGeometry data{};
	std::memcpy(data.fogNearColor, fogNearColor, sizeof(data.fogNearColor));

	// The wind blows along the model's Y axis.
	RE::NiPoint3 windDirection{ geometry->world.rotate.entry[1][0], geometry->world.rotate.entry[1][1], geometry->world.rotate.entry[1][2] };
	windDirection.Unitize();
	data.windVector[0] = windDirection.x;
	data.windVector[1] = windDirection.y;
	data.windVector[2] = std::min(*grassWindAmount, *grassWindAmountMax) * *windGrassMultiplier;

	const auto& sunDirection = sunLight->GetWorldDirection();
	data.dirLightDirection[0] = -sunDirection.x;
	data.dirLightDirection[1] = -sunDirection.y;
	data.dirLightDirection[2] = -sunDirection.z;

	const auto& sun = sunLight->GetLightRuntimeData();
	data.dirLightColor[0] = sun.diffuse.red;
	data.dirLightColor[1] = sun.diffuse.green;
	data.dirLightColor[2] = sun.diffuse.blue;
	data.ambientColor[0] = sun.ambient.red;
	data.ambientColor[1] = sun.ambient.green;
	data.ambientColor[2] = sun.ambient.blue;

	const bool uniformScale = shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kUniformScale);
	data.scaleMask[0] = uniformScale ? 1.0f : 0.0f;
	data.scaleMask[1] = uniformScale ? 1.0f : 0.0f;
	data.scaleMask[2] = 1.0f;

	auto variant = std::ranges::find_if(perGeometryVariants, [&](const GrassPerGeometry& v) { return std::memcmp(&v, &data, sizeof(data)) == 0; });
	if (variant == perGeometryVariants.end()) {
		if (perGeometryVariants.size() == kMaxPerGeometryVariants)
			perGeometryVariants.clear();
		perGeometryVariants.push_back(data);

		auto* ctx = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(ctx->Map(perGeometryCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			perGeometryVariants.clear();
			return false;
		}
		auto* bytes = static_cast<uint8_t*>(m.pData);
		for (size_t i = 0; i < perGeometryVariants.size(); ++i)
			std::memcpy(bytes + (i + 1) * kPerGeometrySlotBytes, &perGeometryVariants[i], sizeof(GrassPerGeometry));
		ctx->Unmap(perGeometryCB->CB(), 0);
		variant = perGeometryVariants.end() - 1;
	}
	ID3D11Buffer* cb = perGeometryCB->CB();
	UINT first = (UINT)(variant - perGeometryVariants.begin()) * (kPerGeometrySlotBytes / 16);
	UINT num = 2 * (kPerGeometrySlotBytes / 16);
	ctx1->VSSetConstantBuffers1(2, 1, &cb, &first, &num);
	ctx1->PSSetConstantBuffers1(2, 1, &cb, &first, &num);
	return true;
}

uint64_t GrassOptimizations::GetPassKey(const RE::BSRenderPass* pass)
{
	uint32_t descriptor = 0;
	if (globals::game::currentPixelShader && *globals::game::currentPixelShader)
		descriptor = (*globals::game::currentPixelShader)->id;
	return (static_cast<uint64_t>(pass->passEnum) << 32) | descriptor;
}

static size_t GIDGroupBytes(const RE::BSMultiStreamInstanceTriShape::GroupHeader* header)
{
	if (!header || !header->numShortsPerInstance)
		return 0;
	return (size_t)header->groupInstanceCount * header->numShortsPerInstance * sizeof(std::uint16_t);
}

std::uint32_t GrassOptimizations::Hooks::AddGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3)
{
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2));
	return func(a1, a2, a3);
}

std::uint32_t GrassOptimizations::Hooks::AddQueuedGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4)
{
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2));
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
	tl_lastFileInstanceData.resize((size + sizeof(uint16_t) - 1) / sizeof(uint16_t));
	std::memcpy(tl_lastFileInstanceData.data(), instanceData, size);
	tl_haveFileGroup = true;
}

void GrassOptimizations::Hooks::AddGroupQueuedGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3)
{
	tl_haveFileGroup = false;
	func(a1, a2, a3);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	tl_haveFileGroup = false;
	func(a1, a2);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

RE::BSMultiStreamInstanceTriShape* GrassOptimizations::Hooks::LoadGrassType::thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath)
{
	auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

	if (shape && modelPath)
		globals::features::grassOptimizations.bucketStore.meshLibrary.RecordModelPath(shape, modelPath->c_str());

	return shape;
}

void VanillaDrawInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto* ctx = globals::d3d::context;
	auto& groups = geometry->GetMultiStreamTrishapeRuntimeData().instanceGroups;

	for (uint32_t i = 0; i < groups.size(); ++i) {
		auto curInstanceGroup = groups[i];
		if (!curInstanceGroup || !curInstanceGroup->isVisible)
			continue;

		uint32_t indexCount = 0;
		uint32_t* indexCountPtr = &indexCount;
		static REL::Relocation<ID3D11Buffer** (*)(RE::BSGraphics::Renderer*, uint64_t, uint32_t**, uint32_t)> MapDynamicBuffer{ REL::RelocationID(75561, 77362) };
		auto buffer = MapDynamicBuffer(globals::game::renderer, 1, &indexCountPtr, 7);
		if (buffer && indexCountPtr) {
			*indexCountPtr = i;
			if (*buffer)
				ctx->Unmap(*buffer, 0);
			ctx->VSSetConstantBuffers(7u, 1u, buffer);
		}

		static REL::Relocation<void (*)(RE::BSGraphics::Renderer*, RE::BSGraphics::TriShape*, uint32_t, uint32_t, uint32_t, RE::BSGraphics::VertexDesc, RE::BSGraphics::VertexBuffer*)> DrawInstancedTriShape{ REL::RelocationID(75479, 77265) };
		DrawInstancedTriShape(globals::game::renderer, geometry->GetGeometryRuntimeData().rendererData, 0, geometry->GetTrishapeRuntimeData().triangleCount, curInstanceGroup->instanceCount, geometry->GetGeometryRuntimeData().vertexDesc, curInstanceGroup->vertexBuffer);
	}
}

void GrassOptimizations::Hooks::DrawInstanceTriShape::thunk(RE::BSRenderPass* pass, RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto& self = globals::features::grassOptimizations;
	auto* ctx = globals::d3d::context;

	auto shaderProperty = geometry->GetGeometryRuntimeData().shaderProperty;
	if (!shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}
	RE::NiSourceTexture* diffuseTexture = shaderProperty->GetBaseTexture();
	if (!diffuseTexture) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	const uint64_t descVal = *reinterpret_cast<uint64_t*>(&geometry->GetGeometryRuntimeData().vertexDesc);
	const uint32_t frame = globals::game::graphicsState->frameCount;

	// Keep the capture and its GPU resources alive through all of this bucket's draws.
	std::scoped_lock lk(self.bucketStore.bucketMutex);
	// Look up by shape, like OnVisible; a key rebuilt from engine state can miss the capture.
	auto* b = self.bucketStore.FindBucket(geometry);
	if (!b || !b->totalInstances || !b->optimizedReady.load(std::memory_order_relaxed)) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	// Multiple shapes share a bucket; draw it once per frame and technique.
	const uint64_t passKey = GetPassKey(pass);

	if (b->drawnFrame == frame && b->drawnPassKey == passKey)
		return;
	b->drawnFrame = frame;
	b->drawnPassKey = passKey;

	if (!b->cullVisible) {
		return;
	}

	auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData)
		return;
	auto* meshVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	auto* indexB = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

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

	// SetDirtyStates binds each dirty pixel texture on its own; bind the material's slots in one call instead.
	constexpr uint32_t kMaterialTextureBits = 0x1F;
	const uint32_t materialTextures = shadowState.PSResourceModifiedBits & kMaterialTextureBits;
	shadowState.PSResourceModifiedBits &= ~kMaterialTextureBits;
	SetDirtyStates(0);
	if (materialTextures) {
		const uint32_t firstTexture = std::countr_zero(materialTextures);
		const uint32_t textureCount = std::bit_width(materialTextures) - firstTexture;
		ctx->PSSetShaderResources(firstTexture, textureCount, reinterpret_cast<ID3D11ShaderResourceView* const*>(&shadowState.PSTexture[firstTexture]));
	}

	ctx->IASetIndexBuffer(indexB, DXGI_FORMAT_R16_UINT, 0);

	ID3D11Buffer* vbs[2] = { meshVB, nullptr };
	UINT strides[2] = { VertexStrideFromDesc(descVal), kGrassStride };
	UINT offsets[2] = { 0, 0 };
	if (!strides[0])
		return;

	vbs[1] = b->compactedBuf;
	ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
	ctx->VSSetShaderResources(2, 1, &b->extrasSRV);
	ctx->DrawIndexedInstancedIndirect(self.drawArgs->resource.get(), b->cullSlot * argsSlotBytes + argsByteOffset);

	for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		GrassBucket::LODBin& bin = b->lodBins[tier];
		if (!bin.active)
			continue;

		const auto* lod = self.bucketStore.meshLibrary.GetLODMesh(b->meshId, (GrassMeshLibrary::LODTier)tier);
		if (!lod || !lod->vertexBuffer || !lod->indexBuffer)
			continue;

		if (bin.argsIndexCount != lod->indexCount) {
			const D3D11_BOX argBox{ argsByteOffset, 0, 0, argsByteOffset + sizeof(uint32_t), 1, 1 };
			ctx->UpdateSubresource(bin.argsBuf, 0, &argBox, &lod->indexCount, 0, 0);
			bin.argsIndexCount = lod->indexCount;
		}

		if (shadowState.vertexDesc != lod->descVal) {
			shadowState.vertexDesc = lod->descVal;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
			SetDirtyStates(0);
		}

		ctx->IASetIndexBuffer(lod->indexBuffer, DXGI_FORMAT_R16_UINT, 0);

		vbs[0] = lod->vertexBuffer;
		vbs[1] = bin.compactedBuf;
		strides[0] = lod->meshStride;
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->VSSetShaderResources(2, 1, &bin.extrasSRV);
		ctx->DrawIndexedInstancedIndirect(bin.argsBuf, argsByteOffset);
	}
}
