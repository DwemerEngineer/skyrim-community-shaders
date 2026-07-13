#include "GrassOptimizations.h"

#include <DirectXPackedVector.h>

#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.grass_optimizations."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassOptimizations::Settings,
	ShowDebugVisualization)

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
	ImGui::SeparatorText(T(TKEY("debug"), "Debug"));

	ImGui::Checkbox(T(TKEY("show_bucket_debug_visualization"), "Show Bucket Debug Visualization"), &settings.ShowDebugVisualization);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("show_bucket_debug_visualization_tooltip"),
							  "Colors grass instances by bucket"));
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

	if (!ctx1 && !triedCtx1Init) {
		triedCtx1Init = true;
		InitRunBaseCB();
	}

	const uint32_t frame = globals::game::graphicsState->frameCount;
	if (runBaseCBRetired && retireFrame != frame) {
		runBaseCBRetired->Release();
		runBaseCBRetired = nullptr;
	}

	timeAccum += globals::game::smState->timerValues[1];

	if (fadeInTimeRcp == 0.0f) {
		const float t = RE::GetINISetting("fGrassFadeInTime:Grass")->GetFloat();
		fadeInTimeRcp = t > 0.0f ? 1.0f / t : 1e6f;  // zero fade time → instant
	}

	if (shadowGrassDistance != settings.shadowGrassDistance) {
		shadowGrassDistance = settings.shadowGrassDistance;
		shadowGrassDistSq = shadowGrassDistance * shadowGrassDistance;
	}

	ApplyRemovals(rems);
	ApplyCaptures(caps);
	UploadDirtyBuckets(device, ctx);
	BuildVisibleRuns();
	UploadRunBases(ctx);
}

void GrassOptimizations::ApplyRemovals(const std::vector<RE::BSMultiStreamInstanceTriShape*>& removes)
{
	if (removes.empty())
		return;

	std::unordered_set<RE::BSMultiStreamInstanceTriShape*> dead(removes.begin(), removes.end());
	for (auto it = buckets.begin(); it != buckets.end();) {
		auto& b = it->second;
		const size_t before = b.slices.size();

		std::erase_if(b.slices, [&](const BucketSlice& s) { return dead.count(s.shape) != 0; });

		if (b.slices.empty()) {
			RE::NiSourceTexture* tex = it->first.tex;
			b.Release();
			it = buckets.erase(it);

			bool texStillUsed = false;
			for (const auto& [k, _] : buckets) {
				if (k.tex == tex) {
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
		if (b.slices.size() != before)
			b.dirty = true;
		++it;
	}
}

void GrassOptimizations::ApplyCaptures(std::vector<PendingCapture>& captures)
{
	for (auto& pc : captures) {
		const BucketKey bk{ pc.diffuseTexture, pc.descVal };
		auto& b = buckets[bk];

		if (b.firstNewSlice == UINT32_MAX)
			b.firstNewSlice = (uint32_t)b.slices.size();

		{
			std::unique_lock lk(bucketKeysMutex);
			bucketKeys.insert(pc.diffuseTexture);
		}

		b.descVal = pc.descVal;

		BuildSubCellSlices(b, pc.shape, pc.bytes.data(), pc.count, pc.instanceStride, pc.origin, timeAccum);
	}
}

void GrassOptimizations::UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	for (auto& [key, b] : buckets) {
		const uint32_t stride = (uint32_t)((b.descVal >> 2) & 0x3C);

		uint32_t total = 0;
		for (auto& s : b.slices)
			total += s.count;
		b.totalInstances = total;

		if (b.dirty) {
			RebuildBucket(b, stride, device, ctx);
		} else if (b.firstNewSlice != UINT32_MAX) {
			if (total > b.capacityInstances)
				RebuildBucket(b, stride, device, ctx);
			else
				AppendNewSlices(b, stride, ctx);
		}
	}
}

void GrassOptimizations::RebuildBucket(GrassBucket& bucket, uint32_t instanceStride, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	bucket.dirty = false;
	bucket.firstNewSlice = UINT32_MAX;

	if (!bucket.totalInstances)
		return;

	if (!EnsureBucketCapacity(bucket, bucket.totalInstances, instanceStride, device)) {
		bucket.totalInstances = 0;
		return;
	}

	static thread_local std::vector<uint8_t> concat;
	static thread_local std::vector<float> fades;
	concat.clear();
	fades.clear();
	concat.reserve((size_t)bucket.totalInstances * instanceStride);
	fades.reserve(bucket.totalInstances);
	uint32_t off = 0;
	for (auto& s : bucket.slices) {
		s.bufferOffset = off;
		concat.insert(concat.end(), s.data.begin(), s.data.end());
		fades.insert(fades.end(), s.count, s.fadeStart);
		off += s.count;
	}

	const D3D11_BOX box{ 0, 0, 0, (UINT)concat.size(), 1, 1 };
	ctx->UpdateSubresource(bucket.instanceBuf, 0, &box, concat.data(), 0, 0);

	const D3D11_BOX fbox{ 0, 0, 0, (UINT)(fades.size() * sizeof(float)), 1, 1 };
	ctx->UpdateSubresource(bucket.fadeBuf, 0, &fbox, fades.data(), 0, 0);
}

void GrassOptimizations::AppendNewSlices(GrassBucket& bucket, uint32_t instanceStride, ID3D11DeviceContext* ctx)
{

	uint32_t prefix = 0;
	for (uint32_t i = 0; i < bucket.firstNewSlice; ++i) {
		const auto& s = bucket.slices[i];
		if (s.bufferOffset == UINT32_MAX || s.bufferOffset != prefix) {
			logger::warn("[GRASS OPTIMIZATIONS] append prefix mismatch slice={} stored={} expected={} — rebuilding",
				i, s.bufferOffset, prefix);
			bucket.dirty = true;
			RebuildBucket(bucket, instanceStride, globals::d3d::device, ctx);
			return;
		}
		prefix += s.count;
	}

	static thread_local std::vector<uint8_t> tail;
	static thread_local std::vector<float> fadeTail;
	tail.clear();
	fadeTail.clear();

	uint32_t tailOff = 0;
	for (uint32_t i = bucket.firstNewSlice; i < (uint32_t)bucket.slices.size(); ++i) {
		auto& s = bucket.slices[i];
		s.bufferOffset = prefix + tailOff;  // the running tail offset
		tail.insert(tail.end(), s.data.begin(), s.data.end());
		fadeTail.insert(fadeTail.end(), s.count, s.fadeStart);
		tailOff += s.count;
	}

	if (!tail.empty()) {
		const D3D11_BOX box{ prefix * instanceStride, 0, 0,
			prefix * instanceStride + (UINT)tail.size(), 1, 1 };
		ctx->UpdateSubresource(bucket.instanceBuf, 0, &box, tail.data(), 0, 0);

		const D3D11_BOX fbox{ prefix * (UINT)sizeof(float), 0, 0,
			(prefix + tailOff) * (UINT)sizeof(float), 1, 1 };
		ctx->UpdateSubresource(bucket.fadeBuf, 0, &fbox, fadeTail.data(), 0, 0);
	}
	bucket.firstNewSlice = UINT32_MAX;
}





void GrassOptimizations::BuildVisibleRuns()
{
	const uint32_t frame = globals::game::graphicsState->frameCount;

	RE::NiFrustumPlanes frustum{};
	RE::NiPoint3 camPos{};
	bool haveFrustum = false;

	{
		std::scoped_lock lk(planesMutex);
		if (planesFrame == frame || planesFrame + 1 == frame) {
			frustum = capturedPlanes;
			camPos = capturedCamPos;
			haveFrustum = true;
		}
	}

	if (maxGrassDistance == 0.0f) {
		maxGrassDistance = RE::GetINISetting("fGrassStartFadeDistance:Grass")->GetFloat() +
		                   RE::GetINISetting("fGrassFadeRange:Grass")->GetFloat();
		maxDistSq = maxGrassDistance * maxGrassDistance;
	}

	// lane order x,y,z to match the min/max vectors below
	const __m128 cam4 = _mm_set_ps(0.0f, camPos.z, camPos.y, camPos.x);

	for (auto& [key, b] : buckets) {
		b.visibleRuns.clear();
		b.visibleRuns.reserve((b.slices.size() >> 3) + 8);

		uint32_t runStart = 0;
		uint32_t runLen = 0;
		float runSortKeySq = FLT_MAX;  // sort key only — never a cull threshold
		RE::NiPoint3 runOrigin{};

		auto flush = [&] {
			if (runLen > 0) {
				uint32_t draw = runLen;
				const float d2 = runSortKeySq;
				// begin thinning past ~2000 units, floor at 15% by ~8000
				if (d2 > settings.BeginThinningDistance * settings.BeginThinningDistance) {
					const float d = sqrtf(d2);
					const float t = std::clamp((d - settings.BeginThinningDistance) / settings.ThinningDistance, 0.0f, 1.0f);
					const float keep = 1.0f - (1.0f - settings.MinDistantAmmount) * t;
					draw = std::max(1u, (uint32_t)(runLen * keep));
				}
				b.visibleRuns.push_back({ runStart, runLen, UINT32_MAX, runSortKeySq, runOrigin, draw });
				runLen = 0;
			}
		};

		for (const auto& s : b.slices) {
			// Not in the GPU buffer (failed/pending upload): undrawable regardless
			// of visibility — and must not be merged into any window.
			if (s.bufferOffset == UINT32_MAX) {
				flush();
				continue;
			}

			bool vis = true;
			float distSq = 0.0f;

			if (haveFrustum) {
				const __m128 minV = _mm_set_ps(0.0f, s.aabbMin.z, s.aabbMin.y, s.aabbMin.x);
				const __m128 maxV = _mm_set_ps(0.0f, s.aabbMax.z, s.aabbMax.y, s.aabbMax.x);

				// squared distance from camera to nearest point of the AABB
				const __m128 clamped = _mm_max_ps(minV, _mm_min_ps(cam4, maxV));
				const __m128 diff = _mm_sub_ps(clamped, cam4);
				const __m128 sq = _mm_mul_ps(diff, diff);

				__m128 sum = _mm_add_ps(sq, _mm_movehl_ps(sq, sq));
				sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 1));
				distSq = _mm_cvtss_f32(sum);

				// threshold is the constant max grass distance — per slice, no state
				vis = (distSq <= maxDistSq) && AabbVisible(frustum, s.aabbMin, s.aabbMax);
			}

			if (vis) {
				const bool adjacent = runLen > 0 && s.bufferOffset == runStart + runLen;
				const bool sameOrigin = runLen > 0 && fabsf(s.origin.x - runOrigin.x) < 0.1f && fabsf(s.origin.y - runOrigin.y) < 0.1f && fabsf(s.origin.z - runOrigin.z) < 0.1f;
				if (runLen > 0 && !(adjacent && sameOrigin))
					flush();
				if (runLen == 0) {
					runStart = s.bufferOffset;
					runSortKeySq = FLT_MAX;
					runOrigin = s.origin;
				}
				runLen += s.count;
				runSortKeySq = std::min(runSortKeySq, distSq);
			} else {
				flush();
			}
		}
		flush();

		std::ranges::sort(b.visibleRuns,
			[](const VisibleRun& a, const VisibleRun& r) { return a.sortKeySq < r.sortKeySq; });
	}
}

bool GrassOptimizations::EnsureBucketCapacity(GrassBucket& b, uint32_t neededInstances, uint32_t instanceStride, ID3D11Device* device)
{
	if (b.instanceBuf && b.capacityInstances >= neededInstances)
		return true;

	uint32_t cap = b.capacityInstances ? b.capacityInstances : 4096;
	while (cap < neededInstances)
		cap *= 2;

	b.ReleaseResources();

	D3D11_BUFFER_DESC ib{};
	ib.ByteWidth = cap * instanceStride;
	ib.Usage = D3D11_USAGE_DEFAULT;
	ib.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	HRESULT hr = device->CreateBuffer(&ib, nullptr, &b.instanceBuf);
	if (FAILED(hr) || !b.instanceBuf) {
		logger::error("[GRASS OPTIMIZATIONS] instance VB create failed hr={:08X} bytes={} instances={}", (unsigned)hr, ib.ByteWidth, cap);
		b.capacityInstances = 0;
		return false;
	}
	Util::SetResourceName(b.instanceBuf, "GrassOptimizations::BucketInstanceVB");

	D3D11_BUFFER_DESC ab{};
	ab.ByteWidth = cap * sizeof(float);
	ab.Usage = D3D11_USAGE_DEFAULT;
	ab.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ab.CPUAccessFlags = 0;
	ab.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	ab.StructureByteStride = sizeof(float);
	hr = device->CreateBuffer(&ab, nullptr, &b.fadeBuf);
	if (FAILED(hr) || !b.fadeBuf) {
		logger::error("[GRASS OPTIMIZATIONS] fade buffer create failed hr={:08X} bytes={}", (unsigned)hr, ab.ByteWidth);
		b.ReleaseResources();
		return false;
	}
	Util::SetResourceName(b.fadeBuf, "GrassOptimizations::BucketFadeBuf");

	D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format = DXGI_FORMAT_UNKNOWN;
	sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	sv.Buffer.NumElements = cap;
	hr = device->CreateShaderResourceView(b.fadeBuf, &sv, &b.fadeSRV);
	if (FAILED(hr) || !b.fadeSRV) {
		logger::error("[GRASS OPTIMIZATIONS] fade SRV create failed hr={:08X} elements={}", (unsigned)hr, cap);
		b.ReleaseResources();
		return false;
	}
	Util::SetResourceName(b.fadeSRV, "GrassOptimizations::BucketFadeBuf SRV");

	b.capacityInstances = cap;
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

		// p-vertex: corner most positive along the plane normal. If even it is
		// outside, all 8 corners are outside → box culled by this plane.
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

void GrassOptimizations::CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData)
{
	if (!shape || !header || !instanceData)
		return;

	auto prop = shape->GetGeometryRuntimeData().shaderProperty;
	if (!prop || prop->GetRTTI() != BSGrassShaderProperty_Ni_RTTI.get())
		return;

	RE::NiSourceTexture* tex = prop->GetBaseTexture();
	if (!tex)
		return;

	const uint32_t count = header->groupInstanceCount;
	const uint64_t descVal = *reinterpret_cast<const uint64_t*>(&shape->GetGeometryRuntimeData().vertexDesc);
	const uint32_t stride = (uint32_t)((descVal >> 2) & 0x3C);
	if (!count || stride < 8) {
		logger::warn("[GRASS OPTIMIZATIONS] GID capture: bad count={} stride={} desc={:016X} shape={:p}",
			count, stride, descVal, (void*)shape);
		return;
	}

	PendingCapture pc;
	pc.shape = shape;
	pc.descVal = descVal;
	pc.diffuseTexture = tex;
	pc.instanceStride = stride;
	pc.count = count;
	pc.origin = shape->world.translate;
	pc.bytes.resize(static_cast<size_t>(count) * stride);
	std::memcpy(pc.bytes.data(), instanceData, pc.bytes.size());

	std::scoped_lock lk(pendingMutex);
	pendingCaptures.push_back(std::move(pc));
}

void GrassOptimizations::BuildSubCellSlices(GrassBucket& bucket, RE::BSMultiStreamInstanceTriShape* shape, const uint8_t* src, uint32_t count, uint32_t stride, const RE::NiPoint3& wt, float fadeStart)
{
	constexpr int kGrid = 4;
	constexpr int kCells = kGrid * kGrid;
	constexpr float kCellSize = 4096.0f;

	const __m128 wtV = _mm_set_ps(0.0f, wt.z, wt.y, wt.x);
	const __m128 invV = _mm_set1_ps(kGrid / kCellSize);  // hoisted reciprocal
	const __m128i gridMax = _mm_set1_epi32(kGrid - 1);

	uint32_t cellCount[kCells] = {};
	__m128 cellMin[kCells];
	__m128 cellMax[kCells];
	for (int c = 0; c < kCells; ++c) {
		cellMin[c] = _mm_set1_ps(FLT_MAX);
		cellMax[c] = _mm_set1_ps(-FLT_MAX);
	}

	static thread_local std::vector<uint8_t> binOf; 
	binOf.resize(count);

	{
		const uint8_t* p = src;
		for (uint32_t i = 0; i < count; ++i, p += stride) {
			const __m128i h = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p));  // 4×u16
			const __m128 local = _mm_cvtph_ps(h);                                    // {lx,ly,lz,_}
			const __m128 world = _mm_add_ps(local, wtV);

			const __m128i idx = _mm_cvttps_epi32(_mm_mul_ps(local, invV));
			const __m128i cl = _mm_max_epi32(_mm_setzero_si128(),
				_mm_min_epi32(idx, gridMax));
			const int gx = _mm_cvtsi128_si32(cl);
			const int gy = _mm_extract_epi32(cl, 1);
			const int slot = gy * kGrid + gx;

			binOf[i] = (uint8_t)slot;
			cellCount[slot]++;
			cellMin[slot] = _mm_min_ps(cellMin[slot], world);
			cellMax[slot] = _mm_max_ps(cellMax[slot], world);
		}
	}

	struct CellOut
	{
		std::vector<uint8_t> bytes;
		uint32_t cursor;
	};
	static thread_local std::array<CellOut, kCells> out;
	for (int c = 0; c < kCells; ++c) {
		if (cellCount[c]) {
			out[c].bytes.resize((size_t)cellCount[c] * stride);  // one alloc per cell
			out[c].cursor = 0;
		}
	}

	{
		const uint8_t* p = src;
		for (uint32_t i = 0; i < count; ++i, p += stride) {
			CellOut& co = out[binOf[i]];
			std::memcpy(co.bytes.data() + co.cursor, p, stride);
			co.cursor += stride;
		}
	}

	for (int c = 0; c < kCells; ++c) {
		if (!cellCount[c])
			continue;

		alignas(16) float mn[4], mx[4];
		_mm_store_ps(mn, cellMin[c]);
		_mm_store_ps(mx, cellMax[c]);

		BucketSlice s;
		s.shape = shape;
		s.count = cellCount[c];
		s.fadeStart = fadeStart;
		s.origin = wt;
		s.aabbMin = { mn[0], mn[1], mn[2] };
		s.aabbMax = { mx[0], mx[1], mx[2] };
		s.data = std::move(out[c].bytes); 
		bucket.slices.push_back(std::move(s));
	}
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

void GrassOptimizations::InitRunBaseCB()
{
	if (FAILED(globals::d3d::context->QueryInterface(
			__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) {
		ctx1 = nullptr;
		logger::warn("[GRASS OPTIMIZATIONS] ID3D11DeviceContext1 unavailable — per-run cb7 fallback in use");
	}
}

bool GrassOptimizations::EnsureRunBaseCapacity(uint32_t slots, ID3D11Device* device)
{
	if (runBaseCB && runBaseCBCapacity >= slots)
		return true;

	uint32_t cap = runBaseCBCapacity ? runBaseCBCapacity : 256;
	while (cap < slots)
		cap *= 2;

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = cap * 256;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	ID3D11Buffer* newCB = nullptr;
	const HRESULT hr = device->CreateBuffer(&bd, nullptr, &newCB);
	if (FAILED(hr) || !newCB) {
		logger::error("[GRASS OPTIMIZATIONS] run base CB create failed hr={:08X} bytes={}", (unsigned)hr, bd.ByteWidth);
		return false; 
	}
	Util::SetResourceName(newCB, "GrassOptimizations::RunBaseCB");

	
	if (runBaseCB) {
		if (runBaseCBRetired)
			runBaseCBRetired->Release(); 
		runBaseCBRetired = runBaseCB;
		retireFrame = globals::game::graphicsState->frameCount;
	}
	runBaseCB = newCB;
	runBaseCBCapacity = cap;
	return true;
}

void GrassOptimizations::UploadRunBases(ID3D11DeviceContext* ctx)
{
	ZoneScoped;

	if (!ctx1)
		return; 

	uint32_t totalRuns = 0;
	for (auto& [key, b] : buckets)
		totalRuns += (uint32_t)b.visibleRuns.size();
	if (!totalRuns)
		return;

	if (!EnsureRunBaseCapacity(totalRuns, globals::d3d::device)) {
		for (auto& [key, b] : buckets)
			for (auto& r : b.visibleRuns)
				r.cbFirstConst = UINT32_MAX;
		return;
	}

	D3D11_MAPPED_SUBRESOURCE m{};
	const HRESULT hr = ctx->Map(runBaseCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
	if (FAILED(hr)) {
		logger::error("[GRASS OPTIMIZATIONS] run base CB map failed hr={:08X} runs={}", (unsigned)hr, totalRuns);
		for (auto& [key, b] : buckets)
			for (auto& r : b.visibleRuns)
				r.cbFirstConst = UINT32_MAX;
		return;
	}

	auto* bytes = static_cast<uint8_t*>(m.pData);
	uint32_t slot = 0;
	for (auto& [key, b] : buckets) {
		b.slotBase = slot;
		for (auto& r : b.visibleRuns) {
			auto* rs = reinterpret_cast<RunSlot*>(bytes + (size_t)slot * 256);
			rs->base = r.base;
			rs->fadeNow = timeAccum;
			rs->fadeInTimeRcp = fadeInTimeRcp;
			rs->debugFlags = settings.ShowDebugVisualization;
			rs->origin[0] = r.origin.x;
			rs->origin[1] = r.origin.y;
			rs->origin[2] = r.origin.z;
			rs->pad1 = 0.0f;
			rs->slotIndex = slot;
			rs->isFar = (r.sortKeySq > settings.farGrassDistance * settings.farGrassDistance) ? 1u : 0u;
			rs->pad2[0] = rs->pad2[1] = 0;
			r.cbFirstConst = slot * 16;
			++slot;
		}
	}
	ctx->Unmap(runBaseCB, 0);
}

static bool IsGrassWorthyPass(RE::NiCullingProcess* proc)
{
	auto& self = globals::features::grassOptimizations;

	if (proc->GetRTTI() == self.BSParabolicCullingProcess_Ni_RTTI.get())
		return false;

	if (auto* cam = proc->camera; cam && cam->GetRTTI() == self.BSCubeMapCamera_Ni_RTTI.get())
		return false;

	return true;
}

bool GrassOptimizations::EnsureTriggerCB(ID3D11Device* device)
{
	if (triggerCB)
		return true;
	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	const HRESULT hr = device->CreateBuffer(&bd, nullptr, &triggerCB);
	if (FAILED(hr) || !triggerCB) {
		logger::error("[GRASS OPTIMIZATIONS] trigger CB create failed hr={:08X}", (unsigned)hr);
		return false;
	}
	Util::SetResourceName(triggerCB, "GrassOptimizations::TriggerCB");
	return true;
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_OnVisible::thunk(RE::BSMultiStreamInstanceTriShape* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex)
{
	auto& self = globals::features::grassOptimizations;

	const uint32_t frame = globals::game::graphicsState->frameCount;
	if (self.planesFrame != frame) {
		auto* cam = process->camera;
		if (cam && cam == RE::Main::WorldRootCamera()) {
			std::scoped_lock lk(self.planesMutex);
			self.capturedPlanes = process->planes;
			self.capturedCamPos = cam->world.translate;
			self.planesFrame = frame;
		}
	}

	auto prop = This->GetGeometryRuntimeData().shaderProperty;
	if (prop && prop->GetRTTI() == self.BSGrassShaderProperty_Ni_RTTI.get()) {
		process->AppendVirtual(This, alphaGroupIndex);
		return;
	}

	func(This, process, alphaGroupIndex);
}

void GrassOptimizations::Hooks::DoneAddingInstances::thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::BSTArray<std::uint32_t>& a_instances)
{
	auto groupAlloc = shape->GetMultiStreamTrishapeRuntimeData().groupAlloc;
	const uint32_t count = shape->GetMultiStreamTrishapeRuntimeData().instanceCount;
	const uint32_t stride = 2u * shape->GetMultiStreamTrishapeRuntimeData().instanceSize;

	if (groupAlloc && count && stride >= 8) {
		PendingCapture pc;
		pc.shape = shape;
		pc.descVal = *reinterpret_cast<uint64_t*>(&shape->GetGeometryRuntimeData().vertexDesc);
		pc.diffuseTexture = shape->GetGeometryRuntimeData().shaderProperty->GetBaseTexture();
		pc.instanceStride = stride;
		pc.count = count;
		pc.origin = shape->world.translate;
		pc.bytes.resize(static_cast<size_t>(count) * stride);
		std::memcpy(pc.bytes.data(), groupAlloc, pc.bytes.size());

		auto& self = globals::features::grassOptimizations;
		std::scoped_lock lk(self.pendingMutex);
		self.pendingCaptures.push_back(std::move(pc));
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


void GrassOptimizations::Hooks::DrawInstanceTriShape::thunk(RE::BSRenderPass* pass, RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto& self = globals::features::grassOptimizations;
	auto* ctx = globals::d3d::context;

	if (auto rtti = geometry->GetGeometryRuntimeData().shaderProperty->GetRTTI()) {
		if (rtti != self.BSGrassShaderProperty_Ni_RTTI.get()) {
			VanillaDrawInstanceTriShape(geometry);
			return;
		}
	}

	auto grassProperty = reinterpret_cast<RE::BSGrassShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
	RE::NiSourceTexture* diffuseTexture = grassProperty->GetBaseTexture();
	if (!diffuseTexture) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	const uint64_t descVal = *reinterpret_cast<uint64_t*>(&geometry->GetGeometryRuntimeData().vertexDesc);
	const uint32_t frame = globals::game::graphicsState->frameCount;
	GrassBucket* b = nullptr;
	{
		std::scoped_lock lk(self.bucketMutex);
		auto it = self.buckets.find({ diffuseTexture, descVal });
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

	const uint32_t meshStride = (uint32_t)((4 * descVal) & 0x3C);
	const uint32_t instanceStride = (uint32_t)((descVal >> 2) & 0x3C);

	auto* meshVB = reinterpret_cast<ID3D11Buffer*>(geometry->GetGeometryRuntimeData().rendererData->vertexBuffer);
	auto* indexB = reinterpret_cast<ID3D11Buffer*>(geometry->GetGeometryRuntimeData().rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

	const UINT indexCount = 3u * geometry->GetTrishapeRuntimeData().triangleCount;

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

	ID3D11Buffer* buffers[2] = { meshVB, b->instanceBuf };
	UINT strides[2] = { meshStride, instanceStride };
	UINT offsets[2] = { 0, 0 };
	ctx->IASetVertexBuffers(0, 2, buffers, strides, offsets);

	ctx->VSSetShaderResources(2, 1, &b->fadeSRV);

	if (!self.EnsureTriggerCB(globals::d3d::device))
		return;

	const auto& wt = geometry->world.translate;
	if (self.lastTriggerOrigin.x != wt.x || self.lastTriggerOrigin.y != wt.y || self.lastTriggerOrigin.z != wt.z) {
		D3D11_MAPPED_SUBRESOURCE tm{};
		if (FAILED(ctx->Map(self.triggerCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &tm)))
			return;
		float* t = static_cast<float*>(tm.pData);
		t[0] = wt.x;
		t[1] = wt.y;
		t[2] = wt.z;
		t[3] = 0.0f;
		ctx->Unmap(self.triggerCB, 0);
		self.lastTriggerOrigin = wt;
	}
	ctx->VSSetConstantBuffers(8, 1, &self.triggerCB);

	if (!self.ctx1 || !self.runBaseCB) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	if (b->slotBase == UINT32_MAX)
		return; 

	const bool shadowPass = pass != grassProperty->depthPass && globals::game::smState->currentShaderTechnique == 0x5C00005C;

	ID3D11Buffer* cbs[1] = { self.runBaseCB };
	uint32_t runIndex = 0;
	for (const auto& r : b->visibleRuns) {
		const UINT derived = (b->slotBase + runIndex) * 16;
		++runIndex;

		if (shadowPass && self.shadowGrassDistSq > 0.0f && r.sortKeySq > self.shadowGrassDistSq)
			continue;

		if (r.cbFirstConst == UINT32_MAX)
			continue;
		if (r.cbFirstConst != derived) {
			logger::error("[GRASS OPTIMIZATIONS] slot pairing mismatch: stored={} derived={} bucketBase={} run={}",
				r.cbFirstConst, derived, b->slotBase, runIndex - 1);
			continue;
		}

		UINT first = derived;
		UINT num = 16;
		self.ctx1->VSSetConstantBuffers1(7, 1, cbs, &first, &num);
		ctx->DrawIndexedInstanced(indexCount, r.drawCount, 0, 0, r.base);  // drawCount from LOD thinning
	}
}


