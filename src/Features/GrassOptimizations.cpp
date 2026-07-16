#include "GrassLighting.h"
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
	if (!cullCS)
		return;

	timeAccum += globals::game::smState->timerValues[1];
	prevTimeBase = timeBase;
	timeBase = globals::game::smState->timerValues[4] * 0.0016666667f * 6.2831802f;

	if (fadeInTimeRcp == 0.0f) {
		const float t = RE::GetINISetting("fGrassFadeInTime:Grass")->GetFloat();
		fadeInTimeRcp = t > 0.0f ? 1.0f / t : 1e6f;
	}
	if (maxDistSq == 0.0f) {
		maxGrassDistance = RE::GetINISetting("fGrassStartFadeDistance:Grass")->GetFloat() +
		                   RE::GetINISetting("fGrassFadeRange:Grass")->GetFloat();
		maxDistSq = maxGrassDistance * maxGrassDistance;
	}

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
			cp->cameraPos[0] = camPos.x;
			cp->cameraPos[1] = camPos.y;
			cp->cameraPos[2] = camPos.z;
			cp->maxDistSq = maxDistSq;
			cp->pad1 = 0.0f;
			cp->lodNearDistSq = 8000.0f * 8000.0f;
			cp->lodFarDistSq = 16000.0f * 16000.0f;
			cp->lodMinKeep = 0.15f;
			cp->clumpRadius = 128.0f;
			cp->projScale = screenH / (2.0f * tanf(0.5f * Util::GetVerticalFOVRad()));
			cp->minPixelSize = 2.0f;
			const float d = maxGrassDistance;
			cp->bandDistSq[0] = (d * 0.15f) * (d * 0.15f);
			cp->bandDistSq[1] = (d * 0.35f) * (d * 0.35f);
			cp->bandDistSq[2] = (d * 0.65f) * (d * 0.65f);
			cp->pad2 = 0.0f;
			ctx->Unmap(cullParamsCB, 0);
		}
	}

	for (auto& [key, b] : buckets) {
		if (!b.totalInstances || !b.instanceSRV) {
			b.cullVisible = false;
			continue;
		}
		if (!b.coarseValid)
			UpdateCoarseBounds(b);
		b.cullVisible = AabbVisible(frustum, b.coarseMin, b.coarseMax);
	}

	// dispatch only visible buckets
	for (auto& [key, b] : buckets) {
		if (b.cullVisible)
			CullBucket(b, ctx);
	}

	{
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(grassFrameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			struct GrassFrame
			{
				float fadeNow;
				float fadeInTimeRcp;
				uint32_t debugFlags;
				uint32_t instanceStride;
			};
			auto* g = static_cast<GrassFrame*>(m.pData);
			g->fadeNow = timeAccum;
			g->fadeInTimeRcp = fadeInTimeRcp;
			g->debugFlags = settings.ShowDebugVisualization ? 1u : 0u;
			g->instanceStride = 32; 
			ctx->Unmap(grassFrameCB, 0);
		}
	}
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

		if (b.slices.empty() && b.totalInstances == 0)
			b.isComplex = DetectComplexGrass(pc.diffuseTexture, globals::d3d::device, globals::d3d::context);

		if (b.firstNewSlice == UINT32_MAX)
			b.firstNewSlice = (uint32_t)b.slices.size();

		{
			std::unique_lock lk(bucketKeysMutex);
			bucketKeys.insert(pc.diffuseTexture);
		}
		b.descVal = pc.descVal;

		BucketSlice s;
		s.shape = pc.shape;
		s.count = pc.count;
		s.fadeStart = timeAccum;
		s.origin = pc.origin;
		s.data = std::move(pc.bytes);
		b.slices.push_back(std::move(s));
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

static inline void ExpandInstanceRecord(const uint8_t* src, float* dst)
{
	// 16 halfs = two 128-bit loads of 8 halfs each
	const __m128i h0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));       // halfs 0..7
	const __m128i h1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 16));  // halfs 8..15
	_mm_storeu_ps(dst + 0, _mm_cvtph_ps(h0));                                        // floats 0..3
	_mm_storeu_ps(dst + 4, _mm_cvtph_ps(_mm_unpackhi_epi64(h0, h0)));                // floats 4..7
	_mm_storeu_ps(dst + 8, _mm_cvtph_ps(h1));                                        // floats 8..11
	_mm_storeu_ps(dst + 12, _mm_cvtph_ps(_mm_unpackhi_epi64(h1, h1)));               // floats 12..15
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

	static thread_local std::vector<float> expanded;  // 16 floats per instance
	static thread_local std::vector<float> fades;
	static thread_local std::vector<float> origins;
	expanded.clear();
	fades.clear();
	origins.clear();
	expanded.reserve((size_t)bucket.totalInstances * 16);
	fades.reserve(bucket.totalInstances);
	origins.reserve((size_t)bucket.totalInstances * 4);

	uint32_t off = 0;
	for (auto& s : bucket.slices) {
		s.bufferOffset = off;
		const uint8_t* rec = s.data.data();
		const float complexFlag = bucket.isComplex ? 1.0f : 0.0f;
		for (uint32_t i = 0; i < s.count; ++i, rec += instanceStride) {
			float tmp[16];
			ExpandInstanceRecord(rec, tmp);
			expanded.insert(expanded.end(), tmp, tmp + 16);
			fades.push_back(s.fadeStart);
			origins.push_back(s.origin.x);
			origins.push_back(s.origin.y);
			origins.push_back(s.origin.z);
			origins.push_back(complexFlag);
		}
		off += s.count;
	}

	const D3D11_BOX ibox{ 0, 0, 0, (UINT)(expanded.size() * sizeof(float)), 1, 1 };
	ctx->UpdateSubresource(bucket.instanceBuf, 0, &ibox, expanded.data(), 0, 0);

	const D3D11_BOX obox{ 0, 0, 0, (UINT)(origins.size() * sizeof(float)), 1, 1 };
	ctx->UpdateSubresource(bucket.originBuf, 0, &obox, origins.data(), 0, 0);

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

	static thread_local std::vector<float> expTail, fadeTail, originTail;
	expTail.clear();
	fadeTail.clear();
	originTail.clear();

	uint32_t tailOff = 0;
	for (uint32_t i = bucket.firstNewSlice; i < (uint32_t)bucket.slices.size(); ++i) {
		auto& s = bucket.slices[i];
		s.bufferOffset = prefix + tailOff;
		const uint8_t* rec = s.data.data();
		for (uint32_t j = 0; j < s.count; ++j, rec += instanceStride) {
			float tmp[16];
			ExpandInstanceRecord(rec, tmp);
			expTail.insert(expTail.end(), tmp, tmp + 16);
			fadeTail.push_back(s.fadeStart);
			originTail.push_back(s.origin.x);
			originTail.push_back(s.origin.y);
			originTail.push_back(s.origin.z);
			originTail.push_back(0.0f);
		}
		tailOff += s.count;
	}

	if (!expTail.empty()) {
		const D3D11_BOX ibox{ prefix * 64, 0, 0, (prefix + tailOff) * 64, 1, 1 };
		ctx->UpdateSubresource(bucket.instanceBuf, 0, &ibox, expTail.data(), 0, 0);

		const D3D11_BOX obox{ prefix * 4 * (UINT)sizeof(float), 0, 0,
			(prefix + tailOff) * 4 * (UINT)sizeof(float), 1, 1 };
		ctx->UpdateSubresource(bucket.originBuf, 0, &obox, originTail.data(), 0, 0);

		const D3D11_BOX fbox{ prefix * (UINT)sizeof(float), 0, 0,
			(prefix + tailOff) * (UINT)sizeof(float), 1, 1 };
		ctx->UpdateSubresource(bucket.fadeBuf, 0, &fbox, fadeTail.data(), 0, 0);
	}
	bucket.firstNewSlice = UINT32_MAX;
}

bool GrassOptimizations::EnsureBucketCapacity(GrassBucket& b, uint32_t neededInstances, uint32_t, ID3D11Device* device)
{
	if (b.instanceBuf && b.capacityInstances >= neededInstances)
		return true;

	uint32_t cap = b.capacityInstances ? b.capacityInstances : 4096;
	while (cap < neededInstances)
		cap *= 2;

	b.ReleaseResources();

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 64;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 16;
		HRESULT hr = device->CreateBuffer(&bd, nullptr, &b.instanceBuf);
		if (FAILED(hr) || !b.instanceBuf) {
			logger::error("[GRASS OPTIMIZATIONS] instance buffer create failed hr={:08X} bytes={}", (unsigned)hr, bd.ByteWidth);
			b.capacityInstances = 0;
			return false;
		}
		Util::SetResourceName(b.instanceBuf, "GrassOptimizations::InstanceBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.FirstElement = 0;
		sv.Buffer.NumElements = cap * 4;
		hr = device->CreateShaderResourceView(b.instanceBuf, &sv, &b.instanceSRV);
		if (FAILED(hr) || !b.instanceSRV) {
			logger::error("[GRASS OPTIMIZATIONS] instance SRV create failed hr={:08X}", (unsigned)hr);
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.instanceSRV, "GrassOptimizations::InstanceBuf SRV");
	}

	// --- Per-instance origin: StructuredBuffer<float3> ---
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		HRESULT hr = device->CreateBuffer(&bd, nullptr, &b.originBuf);
		if (FAILED(hr) || !b.originBuf) {
			logger::error("[GRASS OPTIMIZATIONS] origin buffer create failed hr={:08X}", (unsigned)hr);
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.originBuf, "GrassOptimizations::OriginBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap;
		hr = device->CreateShaderResourceView(b.originBuf, &sv, &b.originSRV);
		if (FAILED(hr) || !b.originSRV) {
			logger::error("[GRASS OPTIMIZATIONS] origin SRV create failed hr={:08X}", (unsigned)hr);
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.originSRV, "GrassOptimizations::OriginBuf SRV");
	}

	// --- Per-instance fade start: StructuredBuffer<float> ---
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(float);
		HRESULT hr = device->CreateBuffer(&bd, nullptr, &b.fadeBuf);
		if (FAILED(hr) || !b.fadeBuf) {
			logger::error("[GRASS OPTIMIZATIONS] fade buffer create failed hr={:08X}", (unsigned)hr);
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.fadeBuf, "GrassOptimizations::FadeBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap;
		HRESULT hr2 = device->CreateShaderResourceView(b.fadeBuf, &sv, &b.fadeSRV);
		if (FAILED(hr2) || !b.fadeSRV) {
			logger::error("[GRASS OPTIMIZATIONS] fade SRV create failed hr={:08X}", (unsigned)hr2);
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.fadeSRV, "GrassOptimizations::FadeBuf SRV");
	}

	for (uint32_t band = 0; band < GrassBucket::kBands; ++band) {
		{
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = cap * sizeof(uint32_t);
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = sizeof(uint32_t);
			HRESULT hr = device->CreateBuffer(&bd, nullptr, &b.visibleBuf[band]);
			if (FAILED(hr) || !b.visibleBuf[band]) {
				logger::error("[GRASS OPTIMIZATIONS] visible buffer {} create failed hr={:08X}", band, (unsigned)hr);
				b.ReleaseResources();
				return false;
			}
			Util::SetResourceName(b.visibleBuf[band], "GrassOptimizations::VisibleBuf");

			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_UNKNOWN;
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.FirstElement = 0;
			uav.Buffer.NumElements = cap;
			uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
			hr = device->CreateUnorderedAccessView(b.visibleBuf[band], &uav, &b.visibleUAV[band]);
			if (FAILED(hr) || !b.visibleUAV[band]) {
				logger::error("[GRASS OPTIMIZATIONS] visible UAV {} create failed hr={:08X}", band, (unsigned)hr);
				b.ReleaseResources();
				return false;
			}
			Util::SetResourceName(b.visibleUAV[band], "GrassOptimizations::VisibleBuf UAV");

			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = DXGI_FORMAT_UNKNOWN;
			sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			sv.Buffer.NumElements = cap;
			hr = device->CreateShaderResourceView(b.visibleBuf[band], &sv, &b.visibleSRV[band]);
			if (FAILED(hr) || !b.visibleSRV[band]) {
				logger::error("[GRASS OPTIMIZATIONS] visible SRV {} create failed hr={:08X}", band, (unsigned)hr);
				b.ReleaseResources();
				return false;
			}
			Util::SetResourceName(b.visibleSRV[band], "GrassOptimizations::VisibleBuf SRV");
		}

		{
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = 5 * sizeof(uint32_t);
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = 0;
			bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			const uint32_t initArgs[5] = { 0, 0, 0, 0, 0 };
			D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };
			HRESULT hr = device->CreateBuffer(&bd, &init, &b.argsBuf[band]);
			if (FAILED(hr) || !b.argsBuf[band]) {
				logger::error("[GRASS OPTIMIZATIONS] args buffer {} create failed hr={:08X}", band, (unsigned)hr);
				b.ReleaseResources();
				return false;
			}
			Util::SetResourceName(b.argsBuf[band], "GrassOptimizations::ArgsBuf");
		}
	}

	// --- per-instance wind scalars: RW structured (CS write) + SRV (VS read) ---
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 2 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 2 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.windBuf)) || !b.windBuf) {
			logger::error("[GRASS OPTIMIZATIONS] wind buffer create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.windBuf, "GrassOptimizations::WindBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap;
		uav.Buffer.Flags = 0;  // plain RW, no counter
		if (FAILED(device->CreateUnorderedAccessView(b.windBuf, &uav, &b.windUAV)) || !b.windUAV) {
			logger::error("[GRASS OPTIMIZATIONS] wind UAV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.windUAV, "GrassOptimizations::WindBuf UAV");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap;
		if (FAILED(device->CreateShaderResourceView(b.windBuf, &sv, &b.windSRV)) || !b.windSRV) {
			logger::error("[GRASS OPTIMIZATIONS] wind SRV create failed");
			b.ReleaseResources();
			return false;
		}
		Util::SetResourceName(b.windSRV, "GrassOptimizations::WindBuf SRV");
	}

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
	if (!makeDynamicCB(&grassFrameCB, 16, "GrassOptimizations::GrassFrameCB"))
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
}

void GrassOptimizations::CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	float wavePeriod = 1.0f;
	if (!b.slices.empty() && b.slices[0].shape) {
		if (auto* prop = static_cast<RE::BSGrassShaderProperty*>(b.slices[0].shape->GetGeometryRuntimeData().shaderProperty.get()))
			wavePeriod = prop->wavePeriod;
	}

	{
		D3D11_MAPPED_SUBRESOURCE m{};
		if (!cullBucketCB || FAILED(ctx->Map(cullBucketCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
			return;
		auto* cb = static_cast<CullBucketCB*>(m.pData);
		cb->instanceCount = b.totalInstances;
		cb->wavePeriod = wavePeriod;
		cb->timeBase = timeBase;
		cb->prevTimeBase = prevTimeBase;
		ctx->Unmap(cullBucketCB, 0);
	}

	ID3D11UnorderedAccessView* uavs[GrassBucket::kBands + 1] = {
		b.visibleUAV[0], b.visibleUAV[1], b.visibleUAV[2], b.visibleUAV[3], b.windUAV
	};
	UINT initialCounts[GrassBucket::kBands + 1] = { 0, 0, 0, 0, (UINT)-1 };
	ctx->CSSetUnorderedAccessViews(0, GrassBucket::kBands + 1, uavs, initialCounts);

	ctx->CSSetShader(cullCS, nullptr, 0);
	ID3D11Buffer* cbs[2] = { cullParamsCB, cullBucketCB };
	ctx->CSSetConstantBuffers(0, 2, cbs);
	ID3D11ShaderResourceView* srvs[2] = { b.instanceSRV, b.originSRV };
	ctx->CSSetShaderResources(0, 2, srvs);

	ctx->Dispatch((b.totalInstances + 63) / 64, 1, 1);

	ID3D11UnorderedAccessView* nullUAVs[GrassBucket::kBands + 1] = {};
	ctx->CSSetUnorderedAccessViews(0, GrassBucket::kBands + 1, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[2] = {};
	ctx->CSSetShaderResources(0, 2, nullSRVs);

	for (uint32_t band = 0; band < GrassBucket::kBands; ++band)
		ctx->CopyStructureCount(b.argsBuf[band], sizeof(uint32_t), b.visibleUAV[band]);
}

void GrassOptimizations::UpdateCoarseBounds(GrassBucket& b)
{
	constexpr float kCell = 4096.0f;
	RE::NiPoint3 mn{ FLT_MAX, FLT_MAX, FLT_MAX };
	RE::NiPoint3 mx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto& s : b.slices) {
		// cell spans [origin, origin + cell] in XY; Z from actual data is unknown here,
		// so use a generous Z band around origin (grass height + terrain relief).
		mn.x = std::min(mn.x, s.origin.x);
		mn.y = std::min(mn.y, s.origin.y);
		mx.x = std::max(mx.x, s.origin.x + kCell);
		mx.y = std::max(mx.y, s.origin.y + kCell);
	}
	// Z: cells are XY-lattice (origin.z == 0 in your data); grass sits near terrain.
	// Use a wide Z to stay conservative — refine if you store per-slice Z extent.
	mn.z = -8192.0f;
	mx.z = 8192.0f;
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

	if (!b->cullVisible) {
		return;
	}

	const uint32_t meshStride = (uint32_t)((4 * descVal) & 0x3C);
	auto* meshVB = reinterpret_cast<ID3D11Buffer*>(geometry->GetGeometryRuntimeData().rendererData->vertexBuffer);
	auto* indexB = reinterpret_cast<ID3D11Buffer*>(geometry->GetGeometryRuntimeData().rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

	const uint32_t indexCount = 3u * geometry->GetTrishapeRuntimeData().triangleCount;
	const D3D11_BOX argBox{ 0, 0, 0, sizeof(uint32_t), 1, 1 };

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

	// IA: mesh VB + index only — NO instance stream (moved to SRV)
	ctx->IASetIndexBuffer(indexB, DXGI_FORMAT_R16_UINT, 0);
	ID3D11Buffer* buffers[1] = { meshVB };
	UINT strides[1] = { meshStride };
	UINT offsets[1] = { 0 };
	ctx->IASetVertexBuffers(0, 1, buffers, strides, offsets);
	ID3D11ShaderResourceView* vsSRVs[5] = {
		b->instanceSRV, b->originSRV, b->fadeSRV, nullptr, b->windSRV
	};
	ctx->VSSetConstantBuffers(7, 1, &self.grassFrameCB);

	// near → far: each band's survivors write depth for the next to early-Z against
	for (uint32_t band = 0; band < GrassBucket::kBands; ++band) {
		ctx->UpdateSubresource(b->argsBuf[band], 0, &argBox, &indexCount, 0, 0);
		vsSRVs[3] = b->visibleSRV[band];
		ctx->VSSetShaderResources(2, 5, vsSRVs);
		ctx->DrawIndexedInstancedIndirect(b->argsBuf[band], 0);
	}
}
