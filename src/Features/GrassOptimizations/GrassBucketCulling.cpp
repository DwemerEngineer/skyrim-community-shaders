#include "Features/GrassOptimizations.h"
#include "Profiler.h"

void GrassOptimizations::CullGrass(const RE::NiCamera* cam)
{
	auto* device = globals::d3d::device;
	auto* ctx = globals::d3d::context;

	RE::NiFrustumPlanes frustum{};
	ComputeFrustumPlanes(frustum, cam->GetRuntimeData2().viewFrustum, cam->world);

	const RE::NiPoint3 camPos = cam->world.translate;
	const __m128 camPosV = _mm_setr_ps(camPos.x, camPos.y, camPos.z, 0.0f);

	FrustumSoA frustumSoA;
	BuildFrustumSoA(frustumSoA, frustum);

	// The setup hook calls this update only when PerFrame contains the main camera.
	const auto& frameBuffer = globals::game::frameBufferCached;
	const auto& mainCameraViewProj = frameBuffer.GetCameraViewProjUnjittered();
	const auto& mainCameraPos = frameBuffer.GetCameraPosAdjust();

	if (settings.EnableOcclusionCulling)
		hiZ.Build(device, ctx);
	else
		hiZ.Invalidate();

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

		const auto* rowW = cp.cameraViewProj[3];
		cp.projectionWLength = std::sqrt(rowW[0] * rowW[0] + rowW[1] * rowW[1] + rowW[2] * rowW[2]);
		cp.hiZMipCount = (float)hiZ.GetMipCount();
		cp.occlusionBias = std::max(0.0f, settings.OcclusionBias);
		cp.costBiasStartDist = std::max(0.0f, settings.CostBiasStartDistance);

		cullParamsCB->Update(cp);
	}

	visibleBuckets.clear();
	sliceTableCPU.clear();

	globals::profiler->BeginPass("GrassOptimizations::SliceCull");
	for (auto& [key, bucket] : bucketStore.buckets) {

		bucket.ResetCullState();
		const auto* texture = bucket.asset.diffuseTexture.get();
		if (!bucket.totalInstances || !bucket.source.instances || !bucket.uploadedSlices || !texture || !texture->rendererTexture) {
			bucketStore.SetOptimizedReady(bucket, false);
			continue;
		}

		bucketStore.PrepareBucket(bucket);

		CullBucketSlices(bucket, frustumSoA, camPosV);
		if (bucket.sliceTableCount)
			visibleBuckets.push_back(&bucket);
		else
			bucketStore.SetOptimizedReady(bucket, true);
	}

	std::erase_if(visibleBuckets, [&](GrassBucket* bucket) {

		bucket->complexSRV = bucketStore.meshLibrary.GetComplexGrass(bucket->asset.diffuseTexture.get(), ctx);

		if (bucket->cullGroups.size() <= kMaxRecordGroups && bucketStore.UploadCullGroups(*bucket, ctx))
			return false;

		// Use engine draws if the bucket cannot supply valid cull resources.
		bucket->ResetCullState();
		bucketStore.SetOptimizedReady(*bucket, false);

		return true;
	});

	globals::profiler->EndPass();

	globals::profiler->BeginPass("GrassOptimizations::InstanceCull");
	UploadCullState(ctx);
	globals::profiler->EndPass();
}

ID3D11ComputeShader* GrassOptimizations::GetCullCS()
{
	if (!cullCS && !cullCSFailed) {
		cullCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\GrassCullingCS.hlsl", {}, "cs_5_0")));
		if (!cullCS) {
			cullCSFailed = true;
			logger::error("[GRASS OPTIMIZATIONS] cull CS load failed; optimized drawing disabled");
		}
	}

	return cullCS.get();
}

bool GrassOptimizations::IsMainCameraPerFrame(const RE::NiPoint3& mainCameraPos)
{
	const auto& posAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	const RE::NiPoint3 eyeOffset{ posAdjust.x - mainCameraPos.x, posAdjust.y - mainCameraPos.y, posAdjust.z - mainCameraPos.z };

	return eyeOffset.SqrLength() < 64.0f * 64.0f;
}

bool GrassOptimizations::HasCurrentCull() const
{
	return lastFrame == globals::game::graphicsState->frameCount;
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
		MakePlane(2, col2, _mm_add_ps(trans, _mm_mul_ps(col2, _mm_set1_ps(viewFrustum.fLeft))));
		MakePlane(3, _mm_xor_ps(col2, _mm_set1_ps(-0.0f)), _mm_add_ps(trans, _mm_mul_ps(col2, _mm_set1_ps(viewFrustum.fRight))));
		MakePlane(4, _mm_xor_ps(col1, _mm_set1_ps(-0.0f)), _mm_add_ps(trans, _mm_mul_ps(col1, _mm_set1_ps(viewFrustum.fTop))));
		MakePlane(5, col1, _mm_add_ps(trans, _mm_mul_ps(col1, _mm_set1_ps(viewFrustum.fBottom))));
	} else {
		// Normalize each side plane from the camera axis and frustum slope.
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

void GrassOptimizations::BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& f)
{
	static constexpr RE::NiFrustumPlanes::ActivePlane kBits[RE::NiFrustumPlanes::Planes::kTotal] = {
		RE::NiFrustumPlanes::ActivePlane::kNear, RE::NiFrustumPlanes::ActivePlane::kFar,
		RE::NiFrustumPlanes::ActivePlane::kLeft, RE::NiFrustumPlanes::ActivePlane::kRight,
		RE::NiFrustumPlanes::ActivePlane::kTop, RE::NiFrustumPlanes::ActivePlane::kBottom
	};

	// Inactive planes always pass, keeping slice tests branch-free.
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
		// Select the corner furthest along each plane normal using its sign bits.
		const __m128 px = _mm_blendv_ps(hx, lx, f.nx[g]);
		const __m128 py = _mm_blendv_ps(hy, ly, f.ny[g]);
		const __m128 pz = _mm_blendv_ps(hz, lz, f.nz[g]);

		const __m128 dot = _mm_add_ps(
			_mm_add_ps(_mm_mul_ps(f.nx[g], px), _mm_mul_ps(f.ny[g], py)),
			_mm_mul_ps(f.nz[g], pz));

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

uint32_t GrassOptimizations::LaneMask(uint32_t remaining)
{
	return (1u << std::min(4u, remaining)) - 1u;
}

void GrassOptimizations::CullBucketSlices(GrassBucket& bucket, const FrustumSoA& frustumSoA, __m128 cameraPosition)
{
	bucket.sliceTableOffset = (uint32_t)sliceTableCPU.size();

	const __m128 bucketLo = _mm_setr_ps(bucket.coarseMin.x, bucket.coarseMin.y, bucket.coarseMin.z, 0.0f);
	const __m128 bucketHi = _mm_setr_ps(bucket.coarseMax.x, bucket.coarseMax.y, bucket.coarseMax.z, 0.0f);
	const __m128 nearest = _mm_max_ps(_mm_max_ps(_mm_sub_ps(bucketLo, cameraPosition), _mm_sub_ps(cameraPosition, bucketHi)), _mm_setzero_ps());
	if (_mm_cvtss_f32(_mm_dp_ps(nearest, nearest, 0x71)) > maxDistSq)
		return;

	uint32_t planeMask;
	if (!ClassifyAabb(frustumSoA, bucketLo, bucketHi, planeMask))
		return;

	if (!bucket.clustersValid || bucket.clusteredSlices != bucket.uploadedSlices)
		bucketStore.MergeSlicesIntoRuns(bucket);

	const __m128 sign = _mm_set1_ps(-0.0f);
	const __m128 farthest = _mm_max_ps(_mm_andnot_ps(sign, _mm_sub_ps(bucketLo, cameraPosition)), _mm_andnot_ps(sign, _mm_sub_ps(bucketHi, cameraPosition)));
	const bool allWithinDistance = _mm_cvtss_f32(_mm_dp_ps(farthest, farthest, 0x71)) <= maxDistSq;

	if (!planeMask && allWithinDistance) {
		// Every run is visible, and its groups are contiguous in their original order.
		if (!bucket.cullGroups.empty())
			AppendVisibleGroups(bucket, 0, (uint32_t)bucket.cullGroups.size());

	} else if (planeMask && bucket.sliceBlocksUseful) {
		CullBucketSliceBlocks(bucket, frustumSoA, cameraPosition, planeMask, allWithinDistance);
	} else {
		CullSliceRuns(bucket, frustumSoA, cameraPosition, 0, bucket.sliceRunCount, planeMask, allWithinDistance);
	}
}

void GrassOptimizations::CullBucketSliceBlocks(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV, uint32_t planeMask, bool allWithinDistance)
{
	const __m128 zero = _mm_setzero_ps();
	const __m128 camX = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 camY = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 camZ = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 maxDistance = _mm_set1_ps(maxDistSq);

	const uint32_t blockCount = (b.sliceRunCount + GrassBucket::kRunsPerCullBlock - 1) / GrassBucket::kRunsPerCullBlock;
	for (uint32_t firstBlock = 0; firstBlock < blockCount; firstBlock += 4) {

		const auto& batch = b.sliceBlockBatches[firstBlock / 4];
		const BoxBatch blocks(batch);
		uint32_t visibleMask = LaneMask(blockCount - firstBlock);
		uint32_t withinDistanceMask = visibleMask;

		if (!allWithinDistance) {
			visibleMask &= _mm_movemask_ps(_mm_cmple_ps(blocks.NearestDistanceSq(camX, camY, camZ), maxDistance));
			if (!visibleMask)
				continue;
			withinDistanceMask = _mm_movemask_ps(_mm_cmple_ps(blocks.FarthestDistanceSq(camX, camY, camZ), maxDistance));
		}

		// A plane that contains a whole block needs no test on that block's runs.
		__m128i childPlanes = _mm_set1_epi32(planeMask);
		for (uint32_t remainingPlanes = planeMask; visibleMask && remainingPlanes; remainingPlanes &= remainingPlanes - 1) {
			const uint32_t planeIndex = std::countr_zero(remainingPlanes);

			const auto& plane = frustumSoA.planes[planeIndex];
			visibleMask &= ~_mm_movemask_ps(_mm_cmplt_ps(blocks.MaxPlaneDistance(plane), zero));
			const __m128i inside = _mm_castps_si128(_mm_cmpge_ps(blocks.MinPlaneDistance(plane), zero));
			childPlanes = _mm_andnot_si128(_mm_and_si128(inside, _mm_set1_epi32(1 << planeIndex)), childPlanes);
		}

		alignas(16) uint32_t childPlaneMasks[4];
		_mm_store_si128((__m128i*)childPlaneMasks, childPlanes);

		for (; visibleMask; visibleMask &= visibleMask - 1) {
			const uint32_t lane = std::countr_zero(visibleMask);
			const bool withinDistance = (withinDistanceMask >> lane) & 1u;

			if (!childPlaneMasks[lane] && withinDistance) {
				AppendVisibleGroups(b, batch.firstGroup[lane], batch.groupCount[lane]);
			} else {
				const uint32_t firstRun = (firstBlock + lane) * GrassBucket::kRunsPerCullBlock;
				CullSliceRuns(b, frustumSoA, camPosV, firstRun, std::min(firstRun + GrassBucket::kRunsPerCullBlock, b.sliceRunCount), childPlaneMasks[lane], withinDistance);
			}

		}
	}
}

void GrassOptimizations::CullSliceRuns(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV, uint32_t firstRun, uint32_t endRun, uint32_t planeMask, bool withinDistance)
{
	const __m128 zero = _mm_setzero_ps();
	const __m128 camX = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(0, 0, 0, 0));
	const __m128 camY = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(1, 1, 1, 1));
	const __m128 camZ = _mm_shuffle_ps(camPosV, camPosV, _MM_SHUFFLE(2, 2, 2, 2));
	const __m128 maxDistance = _mm_set1_ps(maxDistSq);

	for (; firstRun < endRun; firstRun += 4) {
		const auto& batch = b.sliceRunBatches[firstRun / 4];
		const BoxBatch runs(batch);
		uint32_t visibleMask = LaneMask(endRun - firstRun);
		if (!withinDistance)
			visibleMask &= _mm_movemask_ps(_mm_cmple_ps(runs.NearestDistanceSq(camX, camY, camZ), maxDistance));

		for (uint32_t remainingPlanes = planeMask; visibleMask && remainingPlanes; remainingPlanes &= remainingPlanes - 1)
			visibleMask &= ~_mm_movemask_ps(_mm_cmplt_ps(runs.MaxPlaneDistance(frustumSoA.planes[std::countr_zero(remainingPlanes)]), zero));

		// Consume lanes in order so adjacent visible groups still coalesce.
		for (; visibleMask; visibleMask &= visibleMask - 1) {
			const uint32_t lane = std::countr_zero(visibleMask);
			AppendVisibleGroups(b, batch.firstGroup[lane], batch.groupCount[lane]);
		}
	}
}

void GrassOptimizations::AppendVisibleGroups(GrassBucket& b, uint32_t firstGroup, uint32_t groupCount)
{
	const bool extendsLast = b.sliceTableCount &&
	                         sliceTableCPU.back().first + (b.groupCount - sliceTableCPU.back().second) == firstGroup;
	if (!extendsLast) {
		sliceTableCPU.emplace_back(firstGroup, b.groupCount);
		++b.sliceTableCount;
	}

	b.groupCount += groupCount;
}

void GrassOptimizations::DisableOptimizedDraws()
{
	for (auto& [key, b] : bucketStore.buckets) {
		b.ResetCullState();
		bucketStore.SetOptimizedReady(b, false);
	}

	visibleBuckets.clear();
}

void GrassOptimizations::UploadCullState(ID3D11DeviceContext* ctx)
{
	if (visibleBuckets.empty()) {
		bucketStore.PrepareCullOutputs({});
		return;
	}

	ID3D11ShaderResourceView* nullInstanceView = nullptr;
	ctx->VSSetShaderResources(2, 1, &nullInstanceView);
	// All resources and uploads must succeed before any bucket is dispatched.
	if (!bucketStore.PrepareCullOutputs(visibleBuckets) || !UploadCullBucketConstants(ctx) || !UploadSliceTable(ctx)) {
		DisableOptimizedDraws();
		return;
	}

	ID3D11Buffer* paramsCB = cullParamsCB->CB();
	ctx->CSSetConstantBuffers(0, 1, &paramsCB);
	ctx->CSSetShader(cullCS.get(), nullptr, 0);

	// Reset all bucket arguments with one upload before dispatching.
	const uint32_t argumentCount = (uint32_t)visibleBuckets.size() * kTierCount * kArgsStride / sizeof(uint32_t);
	cullArguments.assign(argumentCount, 0);
	for (const GrassBucket* b : visibleBuckets) {
		auto* args = cullArguments.data() + b->cullSlot * kTierCount * kArgsStride / sizeof(uint32_t);
		args[0] = 3u * b->asset.key.triCount;
		for (uint32_t tier = 1; tier < kTierCount; ++tier) {
			if (const auto* lod = b->lodMeshes[tier - 1])
				args[tier * kArgsStride / sizeof(uint32_t)] = lod->indexCount;
		}
	}
	const D3D11_BOX argumentBox{ 0, 0, 0, argumentCount * (UINT)sizeof(uint32_t), 1, 1 };
	ctx->UpdateSubresource(bucketStore.arguments->resource.get(), 0, &argumentBox, cullArguments.data(), 0, 0);

	ID3D11ShaderResourceView* sharedSRVs[2] = { hiZ.GetSRV(), sliceTable->srv.get() };
	ctx->CSSetShaderResources(2, 2, sharedSRVs);
	for (GrassBucket* b : visibleBuckets) {
		CullBucket(*b, ctx);
		bucketStore.SetOptimizedReady(*b, true);
	}

	ID3D11UnorderedAccessView* nullUAVs[2] = {};
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[4] = {};
	ctx->CSSetShaderResources(0, 4, nullSRVs);
	ctx->CSSetShader(nullptr, nullptr, 0);

	// Removals at Present may destroy these buckets before the next frame.
	visibleBuckets.clear();
}

uint32_t GrassOptimizations::DispatchWidth(uint32_t groupCount)
{
	return std::min(groupCount, (uint32_t)D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION);
}

void GrassOptimizations::CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	static_assert(
		(size_t)GrassMeshLibrary::LODTier::kMiddle == 0 &&
			(size_t)GrassMeshLibrary::LODTier::kFar == 1 &&
			kTierCount == 3,
		"GrassCullingCS tiers must match LODTier");

	// Match the output and argument bindings at u0 and u1 in GrassCullingCS.
	ID3D11UnorderedAccessView* uavs[2] = { b.output->records->uav.get(), bucketStore.arguments->uav.get() };
	ctx->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);

	// The Hi-Z pyramid and slice table at t2-t3 are shared and bound once by UploadCullState.
	ID3D11ShaderResourceView* srvs[2] = { b.source.instances->srv.get(), b.groups->srv.get() };
	ctx->CSSetShaderResources(0, 2, srvs);

	ID3D11Buffer* bucketCB = cullBucketCB->CB();
	UINT first = b.cullSlot * (kSlotBytes / 16);
	UINT num = 16;
	ctx1->CSSetConstantBuffers1(1, 1, &bucketCB, &first, &num);

	// Skipping the dispatch keeps the instance count at zero for the draw.
	if (b.groupCount) {
		const UINT width = DispatchWidth(b.groupCount);
		ctx->Dispatch(width, (b.groupCount + width - 1) / width, 1);
	}
}

void GrassBucketStore::PrepareBucket(GrassBucket& b)
{
	b.lodMeshes = {};
	const auto& draw = b.asset.key.draw;
	const auto center = DirectX::XMVectorSet(draw.boundCenter[0], draw.boundCenter[1], draw.boundCenter[2], 0.0f);
	b.boundExtent = DirectX::XMVectorGetX(DirectX::XMVector3Length(center)) + draw.modelRadius;

	if (frameParams.enableMeshLOD) {
		meshLibrary.EnsureLODMeshes(*b.asset.key.mesh);
		if (frameParams.enableMidLOD)
			b.lodMeshes[0] = meshLibrary.GetLODMesh(*b.asset.key.mesh, GrassMeshLibrary::LODTier::kMiddle);
		if (frameParams.enableFarLOD)
			b.lodMeshes[1] = meshLibrary.GetLODMesh(*b.asset.key.mesh, GrassMeshLibrary::LODTier::kFar);
		for (const auto* lod : b.lodMeshes) {
			if (lod)
				b.boundExtent = std::max(b.boundExtent, lod->boundExtent);
		}
	}

	const float padding = b.boundExtent * b.maxScale + 128.0f;
	if (b.boundsPadding != padding) {
		b.boundsPadding = padding;
		b.coarseValid = false;
		b.clustersValid = false;
	}

	if (!b.coarseValid)
		UpdateCoarseBounds(b);
}

void GrassBucketStore::UpdateCoarseBounds(GrassBucket& b)
{
	RE::NiPoint3 minimum{ FLT_MAX, FLT_MAX, FLT_MAX };
	RE::NiPoint3 maximum{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto* slice : b.slices) {
		const auto& bounds = slice->bounds;
		minimum.x = std::min(minimum.x, bounds.lo[0]);
		minimum.y = std::min(minimum.y, bounds.lo[1]);
		minimum.z = std::min(minimum.z, bounds.lo[2]);
		maximum.x = std::max(maximum.x, bounds.hi[0]);
		maximum.y = std::max(maximum.y, bounds.hi[1]);
		maximum.z = std::max(maximum.z, bounds.hi[2]);
	}

	if (minimum.x > maximum.x) {
		minimum = { 0.0f, 0.0f, 0.0f };
		maximum = { 0.0f, 0.0f, 0.0f };
	}

	// Pad the instance bounds to retain grass along screen edges.
	const float padding = b.boundsPadding;
	minimum.x -= padding;
	minimum.y -= padding;
	minimum.z -= padding;
	maximum.x += padding;
	maximum.y += padding;
	maximum.z += padding;

	b.coarseMin = minimum;
	b.coarseMax = maximum;
	b.coarseValid = true;
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
		b.deadGroups = 0;
		b.dirtyGroupRanges.clear();
	}

	const float pad = b.boundsPadding;
	const uint32_t uploadedEnd = b.uploadedSlices;
	for (uint32_t i = b.clusteredSlices; i < uploadedEnd; ++i) {
		auto& slice = *b.slices[i];
		// Runs require a shared cell, but their groups may reference noncontiguous source records.
		const bool sameCellAsPrevious = i && b.sliceRunCount &&
		                       cellOf(b.slices[i]->bounds) == cellOf(b.slices[i - 1]->bounds);

		if (!sameCellAsPrevious) {
			if (b.sliceRunCount % 4 == 0)
				b.sliceRunBatches.emplace_back();
			++b.sliceRunCount;
		}

		auto& batch = b.sliceRunBatches.back();
		const uint32_t lane = (b.sliceRunCount - 1) % 4;
		const auto& bounds = b.slices[i]->bounds;

		const uint32_t blockIndex = (b.sliceRunCount - 1) / GrassBucket::kRunsPerCullBlock;
		const uint32_t blockLane = blockIndex % 4;

		const bool startsBlock = !sameCellAsPrevious && (b.sliceRunCount - 1) % GrassBucket::kRunsPerCullBlock == 0;
		if (startsBlock && blockLane == 0)
			b.sliceBlockBatches.emplace_back();

		auto& block = b.sliceBlockBatches.back();
		block.IncludeBounds(blockLane, bounds, pad, startsBlock);
		batch.IncludeBounds(lane, bounds, pad, !sameCellAsPrevious);

		if (startsBlock)
			block.firstGroup[blockLane] = (uint32_t)b.cullGroups.size();
		if (!sameCellAsPrevious)
			batch.firstGroup[lane] = (uint32_t)b.cullGroups.size();

		// Groups never span slices, so the cull reads each instance's origin from its group.
		slice.firstCullGroup = (uint32_t)b.cullGroups.size();
		for (uint32_t first = 0; first < slice.count; first += kCullGroupSize) {
			b.cullGroups.push_back({ { slice.origin.x, slice.origin.y, slice.origin.z }, slice.fadeStart,
				slice.bufferOffset + first, std::min(kCullGroupSize, slice.count - first) });
			++batch.groupCount[lane];
			++block.groupCount[blockLane];
		}
	}

	// Use parent bounds only for spatially coherent runs; scattered runs use the flat SSE path.
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

	b.clusteredSlices = uploadedEnd;
	b.clustersValid = true;
}
