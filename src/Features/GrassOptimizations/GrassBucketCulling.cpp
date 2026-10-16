#include "Features/GrassOptimizations.h"
#include "Features/GrassLighting.h"
#include "Profiler.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"

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

	// Snapshot PerFrame only when it matches the main camera, since shadow passes upload a light camera.
	// The position tolerance allows stereo eye offsets.
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
	UploadCullState(ctx);
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

void GrassOptimizations::UploadCullState(ID3D11DeviceContext* ctx)
{
	// One map fills every visible bucket's slot — replaces a Map/Unmap per bucket.
	bool cullStateUploaded = false;
	if (!visibleBuckets.empty() && EnsureCullBucketCapacity((uint32_t)visibleBuckets.size())) {
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

	// Clear every counter up front, so no dispatch waits on its own clear. Only LOD bins read the LOD counts.
	const UINT zeros[4] = { 0, 0, 0, 0 };
	for (GrassBucket* b : visibleBuckets) {
		if (b->argsIndexCount != b->indexCount) {
			const D3D11_BOX argBox{ argsByteOffset, 0, 0, argsByteOffset + sizeof(uint32_t), 1, 1 };
			ctx->UpdateSubresource(b->argsBuf, 0, &argBox, &b->indexCount, 0, 0);
			b->argsIndexCount = b->indexCount;
		}
		ctx->ClearUnorderedAccessViewUint(b->argsUAV, zeros);
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
	ID3D11UnorderedAccessView* uavs[4 + 2 * (size_t)GrassMeshLibrary::LODTier::kCount] = { b.compactedUAV, b.extrasUAV, b.argsUAV };
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

void GrassBucketStore::MergeSlicesIntoRuns(GrassBucket& b)
{
	// Group slices by their instance bounds because several cells can share a shape origin.
	const auto cellOf = [](const SliceBounds& bounds) {
		constexpr float kCellSize = 4096.0f;
		const auto cellX = (int32_t)std::floor((bounds.lo[0] + bounds.hi[0]) * 0.5f / kCellSize);
		const auto cellY = (int32_t)std::floor((bounds.lo[1] + bounds.hi[1]) * 0.5f / kCellSize);
		return ((uint64_t)(uint32_t)cellX << 32) | (uint32_t)cellY;
	};

	if (!b.clustersValid) {
		b.sliceRunBatches.clear();
		b.sliceRunCount = 0;
		b.sliceBlockBatches.clear();
		b.sliceBlocksUseful = false;
		b.cullGroups.clear();
		b.clusteredSlices = 0;
		b.residentGroups = 0;
	}
	const float pad = b.modelRadius + 64.0f;
	for (uint32_t i = b.clusteredSlices; i < (uint32_t)b.slices.size(); ++i) {
		const auto& slice = b.slices[i];
		if (slice.bufferOffset == UINT32_MAX || !slice.count)
			continue;

		// Groups carry their own source offsets, so runs need a shared cell but not contiguous records.
		const bool continues = i && b.sliceRunCount &&
		                       b.slices[i - 1].bufferOffset != UINT32_MAX && b.slices[i - 1].count &&
		                       cellOf(b.sliceBounds[i]) == cellOf(b.sliceBounds[i - 1]);
		if (!continues) {
			if (b.sliceRunCount % 4 == 0)
				b.sliceRunBatches.emplace_back();
			++b.sliceRunCount;
		}
		auto& batch = b.sliceRunBatches.back();
		const uint32_t lane = (b.sliceRunCount - 1) % 4;
		const auto& bounds = b.sliceBounds[i];
		const float loX = bounds.lo[0] - pad, loY = bounds.lo[1] - pad, loZ = bounds.lo[2] - pad;
		const float hiX = bounds.hi[0] + pad, hiY = bounds.hi[1] + pad, hiZ = bounds.hi[2] + pad;
		const uint32_t blockIndex = (b.sliceRunCount - 1) / GrassBucket::kRunsPerCullBlock;
		const uint32_t blockLane = blockIndex % 4;
		const bool startsBlock = !continues && (b.sliceRunCount - 1) % GrassBucket::kRunsPerCullBlock == 0;
		if (startsBlock && blockLane == 0)
			b.sliceBlockBatches.emplace_back();
		auto& block = b.sliceBlockBatches.back();
		if (startsBlock) {
			block.loX[blockLane] = loX;
			block.loY[blockLane] = loY;
			block.loZ[blockLane] = loZ;
			block.hiX[blockLane] = hiX;
			block.hiY[blockLane] = hiY;
			block.hiZ[blockLane] = hiZ;
			block.firstGroup[blockLane] = (uint32_t)b.cullGroups.size();
		} else {
			block.loX[blockLane] = std::min(block.loX[blockLane], loX);
			block.loY[blockLane] = std::min(block.loY[blockLane], loY);
			block.loZ[blockLane] = std::min(block.loZ[blockLane], loZ);
			block.hiX[blockLane] = std::max(block.hiX[blockLane], hiX);
			block.hiY[blockLane] = std::max(block.hiY[blockLane], hiY);
			block.hiZ[blockLane] = std::max(block.hiZ[blockLane], hiZ);
		}
		if (continues) {
			batch.loX[lane] = std::min(batch.loX[lane], loX);
			batch.loY[lane] = std::min(batch.loY[lane], loY);
			batch.loZ[lane] = std::min(batch.loZ[lane], loZ);
			batch.hiX[lane] = std::max(batch.hiX[lane], hiX);
			batch.hiY[lane] = std::max(batch.hiY[lane], hiY);
			batch.hiZ[lane] = std::max(batch.hiZ[lane], hiZ);
		} else {
			batch.loX[lane] = loX;
			batch.loY[lane] = loY;
			batch.loZ[lane] = loZ;
			batch.hiX[lane] = hiX;
			batch.hiY[lane] = hiY;
			batch.hiZ[lane] = hiZ;
			batch.firstGroup[lane] = (uint32_t)b.cullGroups.size();
		}

		// Groups never span slices, so the cull reads each instance's origin from its group.
		for (uint32_t first = 0; first < slice.count; first += kCullGroupSize) {
			b.cullGroups.push_back({ { slice.origin.x, slice.origin.y, slice.origin.z }, slice.fadeStart,
				slice.bufferOffset + first, std::min(kCullGroupSize, slice.count - first) });
			++batch.groupCount[lane];
			++block.groupCount[blockLane];
		}
	}
	// Use parent bounds only when run order has spatial locality.
	// Scattered parents rarely eliminate leaf tests, so use the flat SSE path for those buckets.
	b.sliceBlocksUseful = false;
	if (b.sliceRunCount >= GrassBucket::kRunsPerCullBlock * 2) {
		float loX = FLT_MAX, loY = FLT_MAX, hiX = -FLT_MAX, hiY = -FLT_MAX;
		float blockArea = 0.0f;
		const uint32_t blockCount = (b.sliceRunCount + GrassBucket::kRunsPerCullBlock - 1) / GrassBucket::kRunsPerCullBlock;
		for (uint32_t i = 0; i < blockCount; ++i) {
			const auto& block = b.sliceBlockBatches[i / 4];
			const uint32_t lane = i % 4;
			loX = std::min(loX, block.loX[lane]);
			loY = std::min(loY, block.loY[lane]);
			hiX = std::max(hiX, block.hiX[lane]);
			hiY = std::max(hiY, block.hiY[lane]);
			blockArea += (block.hiX[lane] - block.loX[lane]) * (block.hiY[lane] - block.loY[lane]);
		}
		b.sliceBlocksUseful = blockArea < (hiX - loX) * (hiY - loY) * (0.5f * blockCount);
	}
	b.clusteredSlices = (uint32_t)b.slices.size();
	b.clustersValid = true;
}

void GrassBucketStore::UpdateCoarseBounds(GrassBucket& b)
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

	// Generously pad to prevent instances on the screen edge from being visibly culled.
	const float pad = b.modelRadius + 128.0f;
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
