#include "GrassBucketStore.h"

#include "Features/GrassLighting.h"

void GrassBucketStore::SetupResources()
{
	detectParamsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc(16), "GrassOptimizations::DetectParamsCB");

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(uint32_t);
		detectResult = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::DetectResult");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = 1;
		detectResult->CreateUAV(uav);
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_STAGING;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		detectStaging = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::DetectStaging");
	}
}

void GrassBucketStore::ClearShaderCache()
{
	if (detectCS)
		detectCS->Release();
	detectCS = nullptr;
}

ID3D11ComputeShader* GrassBucketStore::GetDetectCS()
{
	if (!detectCS) {
		detectCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\DetectComplexCS.hlsl", {}, "cs_5_0"));
		if (!detectCS)
			logger::error("[GRASS OPTIMIZATIONS] detect CS load failed — complex detection disabled");
	}
	return detectCS;
}

void GrassBucketStore::ApplyPending(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	std::vector<PendingCapture> caps;
	std::vector<RE::BSMultiStreamInstanceTriShape*> rems;
	{
		std::scoped_lock lk(pendingMutex);
		caps.swap(pendingCaptures);
		rems.swap(pendingRemoves);
	}

	ApplyRemovals(rems);
	ApplyCaptures(caps);
	UploadDirtyBuckets(device, ctx);
}

void GrassBucketStore::RefreshComplexGrass(float threshold, ID3D11DeviceContext* ctx)
{
	if (threshold == cachedComplexThreshold)
		return;

	cachedComplexThreshold = threshold;
	complexCache.clear();
	for (auto& [key, b] : buckets) {
		b.isComplex = DetectComplexGrass(key.tex, ctx);
		b.dirty = true;
	}
}

void GrassBucketStore::StageRemoval(RE::BSMultiStreamInstanceTriShape* shape)
{
	std::scoped_lock lk(pendingMutex);
	pendingRemoves.push_back(shape);
}

GrassBucket* GrassBucketStore::FindBucketForShape(RE::BSMultiStreamInstanceTriShape* shape) const
{
	std::shared_lock lk(shapeBucketMutex);
	auto it = shapeBucketId.find(shape);
	return it != shapeBucketId.end() ? it->second : nullptr;
}

void GrassBucketStore::ApplyRemovals(const std::vector<RE::BSMultiStreamInstanceTriShape*>& removes)
{
	if (removes.empty())
		return;

	// A sorted, reused buffer rather than a fresh unordered_set: the game's loader threads keep
	// the heap busy during a cell load, so an allocation here can block the render thread for a
	// long time. This path allocates nothing after warmup.
	static thread_local std::vector<RE::BSMultiStreamInstanceTriShape*> dead;
	dead.assign(removes.begin(), removes.end());
	std::sort(dead.begin(), dead.end());
	const auto isDead = [](RE::BSMultiStreamInstanceTriShape* p) {
		return std::binary_search(dead.begin(), dead.end(), p);
	};

	// One batched lock rather than one per shape.
	{
		std::unique_lock lk(shapeBucketMutex);
		for (auto* s : removes)
			shapeBucketId.erase(s);
	}

	// Keyed by shape pointer, so they must not outlive the shape: the allocator reuses addresses
	// and a future grass shape would inherit this one's mesh id and LOD mesh.
	for (auto* s : removes)
		meshLibrary.ForgetShape(s);

	for (auto it = buckets.begin(); it != buckets.end();) {
		auto& b = it->second;
		const size_t before = b.slices.size();

		// Compact in place, noting the first slice that goes: everything ahead of it keeps its
		// buffer contents and offset, so the rebuild only re-uploads from there.
		uint32_t firstRemoved = UINT32_MAX;
		size_t write = 0;
		uint32_t removedInstances = 0;
		for (size_t read = 0; read < before; ++read) {
			if (isDead(b.slices[read].shape)) {
				if (firstRemoved == UINT32_MAX)
					firstRemoved = (uint32_t)write;
				removedInstances += b.slices[read].count;
				continue;
			}
			if (write != read) {
				b.slices[write] = std::move(b.slices[read]);
				b.sliceBounds[write] = b.sliceBounds[read];
			}
			++write;
		}
		b.slices.resize(write);
		b.sliceBounds.resize(write);
		if (write != before) {
			b.clustersValid = false;
			b.totalInstances -= std::min(removedInstances, b.totalInstances);
		}

		if (b.slices.empty()) {
			b.Release();
			it = buckets.erase(it);

			// Clear now, not at end of frame: the map holds a GrassBucket* to the bucket being
			// erased and the culling thread dereferences those. Costs one frame of un-deduped
			// queueing (absent shapes queue normally), which beats a use-after-free window.
			std::unique_lock lk(shapeBucketMutex);
			shapeBucketId.clear();
			continue;
		}
		if (b.slices.size() != before) {
			b.dirty = true;
			b.coarseValid = false;
			b.rebuildFromSlice = (b.rebuildFromSlice == UINT32_MAX) ?
			                         firstRemoved :
			                         std::min(b.rebuildFromSlice, firstRemoved);
		}
		++it;
	}
}

void GrassBucketStore::ApplyCaptures(std::vector<PendingCapture>& captures)
{
	// Collected then applied under one lock. Inserting per shape would take the writer lock
	// hundreds of times on a cell load, each one blocking the culling threads' readers.
	static thread_local std::vector<std::pair<RE::BSMultiStreamInstanceTriShape*, GrassBucket*>> pendingMapAdds;
	pendingMapAdds.clear();

	auto* ctx = globals::d3d::context;

	for (auto& pc : captures) {
		const uint32_t meshId = meshLibrary.ResolveMeshId(pc.shape);
		const BucketKey bk{ meshId, meshId ? nullptr : pc.diffuseTexture, pc.descVal };
		auto& b = buckets[bk];
		b.meshId = meshId;

		if (b.slices.empty() && b.totalInstances == 0)
			b.isComplex = DetectComplexGrass(pc.diffuseTexture, ctx);

		if (b.firstNewSlice == UINT32_MAX)
			b.firstNewSlice = (uint32_t)b.slices.size();

		if (!b.typeParamsValid) {
			CacheBucketTypeParams(b, pc.shape);
			b.isComplex = DetectComplexGrass(pc.diffuseTexture, ctx);
		}
		if (frameParams.enableMeshLOD)
			meshLibrary.EnsureLODMesh(meshId);

		BucketSlice s;
		s.shape = pc.shape;
		s.count = pc.count;
		s.fadeStart = frameParams.fadeStart;
		s.origin = pc.origin;
		s.localMin = pc.localMin;
		s.localMax = pc.localMax;
		s.data = std::move(pc.bytes);

		SliceBounds sb;
		sb.lo[0] = pc.origin.x + pc.localMin.x;
		sb.lo[1] = pc.origin.y + pc.localMin.y;
		sb.lo[2] = pc.origin.z + pc.localMin.z;
		sb.hi[0] = pc.origin.x + pc.localMax.x;
		sb.hi[1] = pc.origin.y + pc.localMax.y;
		sb.hi[2] = pc.origin.z + pc.localMax.z;

		b.totalInstances += pc.count;
		b.slices.push_back(std::move(s));
		if (pc.shape)
			pendingMapAdds.emplace_back(pc.shape, &b);
		b.sliceBounds.push_back(sb);
		b.clustersValid = false;
		b.coarseValid = false;
	}

	if (!pendingMapAdds.empty()) {
		std::unique_lock lk(shapeBucketMutex);
		for (const auto& [shape, bucket] : pendingMapAdds)
			shapeBucketId[shape] = bucket;
	}
}

void GrassBucketStore::UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	for (auto& [key, b] : buckets) {
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

void GrassBucketStore::CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape,
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

bool GrassBucketStore::StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src,
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

void GrassBucketStore::CacheBucketTypeParams(GrassBucket& b, RE::BSMultiStreamInstanceTriShape* shape)
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

void GrassBucketStore::RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	// Slices below the first changed one keep their contents and offsets, so only the tail is
	// re-assembled. The tail can be stale from removals (rebuildFromSlice) or appends
	// (firstNewSlice); both are UINT32_MAX-sentinelled, so min() picks whichever reaches further
	// back and "neither" falls through to a full rebuild.
	uint32_t from = std::min(bucket.rebuildFromSlice, bucket.firstNewSlice);
	if (from == UINT32_MAX || from > (uint32_t)bucket.slices.size())
		from = 0;

	// A partial rebuild is only valid if everything below `from` sits contiguously.
	uint32_t startInstance = 0;
	for (uint32_t i = 0; i < from; ++i) {
		const auto& s = bucket.slices[i];
		if (s.bufferOffset == UINT32_MAX || s.bufferOffset != startInstance) {
			from = 0;
			startInstance = 0;
			break;
		}
		startInstance += s.count;
	}

	// Computed before allocating: on growth this is the range carried across device-side, so a
	// reallocation does not force a full CPU re-upload.
	if (!EnsureBucketCapacity(bucket, bucket.totalInstances, device, ctx, startInstance))
		return;
	if (!bucket.totalInstances) {
		bucket.dirty = false;
		bucket.rebuildFromSlice = UINT32_MAX;
		bucket.firstNewSlice = UINT32_MAX;
		return;
	}

	uint32_t tailInstances = 0;
	for (uint32_t i = from; i < (uint32_t)bucket.slices.size(); ++i)
		tailInstances += bucket.slices[i].count;

	if (tailInstances) {
		static thread_local std::vector<uint8_t> records;
		static thread_local std::vector<float> origins;
		records.clear();
		records.reserve((size_t)tailInstances * 32);
		origins.resize((size_t)tailInstances * 4);
		float* op = origins.data();

		uint32_t off = startInstance;
		for (uint32_t i = from; i < (uint32_t)bucket.slices.size(); ++i) {
			auto& s = bucket.slices[i];
			s.bufferOffset = off;
			records.insert(records.end(), s.data.begin(), s.data.end());
			for (uint32_t j = 0; j < s.count; ++j) {
				*op++ = s.origin.x;
				*op++ = s.origin.y;
				*op++ = s.origin.z;
				*op++ = s.fadeStart;
			}
			off += s.count;
		}

		const D3D11_BOX ibox{ startInstance * 32, 0, 0, (startInstance + tailInstances) * 32, 1, 1 };
		ctx->UpdateSubresource(bucket.instanceBuf, 0, &ibox, records.data(), 0, 0);

		const D3D11_BOX obox{ startInstance * 4 * (UINT)sizeof(float), 0, 0,
			(startInstance + tailInstances) * 4 * (UINT)sizeof(float), 1, 1 };
		ctx->UpdateSubresource(bucket.originBuf, 0, &obox, origins.data(), 0, 0);
	}

	bucket.dirty = false;
	bucket.rebuildFromSlice = UINT32_MAX;
	bucket.firstNewSlice = UINT32_MAX;
}

void GrassBucketStore::AppendNewSlices(GrassBucket& bucket, ID3D11DeviceContext* ctx)
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
			bucket.dirty = true;
			RebuildBucket(bucket, globals::d3d::device, ctx);
			return;
		}
		prefix += s.count;
	}

	uint32_t tailInstances = 0;
	for (uint32_t i = bucket.firstNewSlice; i < (uint32_t)bucket.slices.size(); ++i)
		tailInstances += bucket.slices[i].count;

	if (!tailInstances) {
		bucket.firstNewSlice = UINT32_MAX;
		return;
	}

	static thread_local std::vector<uint8_t> recTail;
	static thread_local std::vector<float> originTail;
	recTail.clear();
	recTail.reserve((size_t)tailInstances * 32);
	originTail.resize((size_t)tailInstances * 4);
	float* op = originTail.data();

	uint32_t off = prefix;
	for (uint32_t i = bucket.firstNewSlice; i < (uint32_t)bucket.slices.size(); ++i) {
		auto& s = bucket.slices[i];
		s.bufferOffset = off;
		recTail.insert(recTail.end(), s.data.begin(), s.data.end());
		for (uint32_t j = 0; j < s.count; ++j) {
			*op++ = s.origin.x;
			*op++ = s.origin.y;
			*op++ = s.origin.z;
			*op++ = s.fadeStart;
		}
		off += s.count;
	}

	const D3D11_BOX ibox{ prefix * 32, 0, 0, off * 32, 1, 1 };
	ctx->UpdateSubresource(bucket.instanceBuf, 0, &ibox, recTail.data(), 0, 0);

	const D3D11_BOX obox{ prefix * 4 * (UINT)sizeof(float), 0, 0,
		off * 4 * (UINT)sizeof(float), 1, 1 };
	ctx->UpdateSubresource(bucket.originBuf, 0, &obox, originTail.data(), 0, 0);

	bucket.firstNewSlice = UINT32_MAX;
}

bool GrassBucketStore::EnsureBucketCapacity(GrassBucket& b, uint32_t needed, ID3D11Device* device,
	ID3D11DeviceContext* ctx, uint32_t preserveInstances)
{
	if (b.capacityInstances >= needed && b.instanceBuf)
		return true;

	uint32_t cap = b.capacityInstances ? b.capacityInstances : 4096;
	while (cap < needed)
		cap *= 2;

	// Hold the old instance/origin buffers across the reallocation and copy [0, preserve)
	// device-to-device, sparing the caller a several-MB CPU re-assembly. Everything else is
	// per-frame scratch and needs no preservation.
	const uint32_t preserve = std::min(preserveInstances, b.capacityInstances);
	struct OldBuffers
	{
		ID3D11Buffer* instance;
		ID3D11Buffer* origin;
		~OldBuffers()
		{
			if (instance)
				instance->Release();
			if (origin)
				origin->Release();
		}
	} old{ b.instanceBuf, b.originBuf };

	// Withheld from ReleaseResources so the references survive; ~OldBuffers frees them on exit.
	b.instanceBuf = nullptr;
	b.originBuf = nullptr;

	b.ReleaseResources();

	if (!CreateBucketSourceBuffers(b, cap, device) ||
		!CreateBucketCullScratch(b, cap, device) ||
		!CreateBucketArgsBuffer(b, device)) {
		b.ReleaseResources();
		b.capacityInstances = 0;
		return false;
	}

	// Carry the unchanged head across on the GPU. The caller's partial rebuild then only has to
	// upload the tail, exactly as it does when no reallocation happened.
	if (preserve && ctx) {
		if (old.instance && b.instanceBuf) {
			const D3D11_BOX box{ 0, 0, 0, preserve * 32, 1, 1 };
			ctx->CopySubresourceRegion(b.instanceBuf, 0, 0, 0, 0, old.instance, 0, &box);
		}
		if (old.origin && b.originBuf) {
			const D3D11_BOX box{ 0, 0, 0, preserve * 4 * (UINT)sizeof(float), 1, 1 };
			ctx->CopySubresourceRegion(b.originBuf, 0, 0, 0, 0, old.origin, 0, &box);
		}
	}

	b.capacityInstances = cap;
	b.dirty = true;
	return true;
}

bool GrassBucketStore::CreateBucketSourceBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	// Source instance data: raw, half-packed, local positions.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * 32;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.instanceBuf)) || !b.instanceBuf) {
			logger::error("[GRASS OPTIMIZATIONS] instance buffer create failed bytes={}", bd.ByteWidth);
			return false;
		}
		Util::SetResourceName(b.instanceBuf, "GrassOptimizations::InstanceBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_R32_TYPELESS;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		sv.BufferEx.FirstElement = 0;
		sv.BufferEx.NumElements = capacity * 8;
		sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		if (FAILED(device->CreateShaderResourceView(b.instanceBuf, &sv, &b.instanceSRV)) || !b.instanceSRV) {
			logger::error("[GRASS OPTIMIZATIONS] instance raw SRV create failed");
			return false;
		}
		Util::SetResourceName(b.instanceSRV, "GrassOptimizations::InstanceBuf SRV");
	}

	// Origin + spawn time.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.originBuf)) || !b.originBuf) {
			logger::error("[GRASS OPTIMIZATIONS] origin buffer create failed");
			return false;
		}
		Util::SetResourceName(b.originBuf, "GrassOptimizations::OriginBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = capacity;
		if (FAILED(device->CreateShaderResourceView(b.originBuf, &sv, &b.originSRV)) || !b.originSRV) {
			logger::error("[GRASS OPTIMIZATIONS] origin SRV create failed");
			return false;
		}
		Util::SetResourceName(b.originSRV, "GrassOptimizations::OriginBuf SRV");
	}

	return true;
}

bool GrassBucketStore::CreateBucketCullScratch(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	// Compacted survivors, consumed as an instanced vertex stream by the draw.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * 32;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.compactedBuf)) || !b.compactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] compacted buffer create failed");
			return false;
		}
		Util::SetResourceName(b.compactedBuf, "GrassOptimizations::CompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = capacity * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.compactedBuf, &uav, &b.compactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] compacted UAV create failed");
			return false;
		}
		Util::SetResourceName(b.compactedUAV, "GrassOptimizations::CompactedBuf UAV");
	}

	// Extras: [i*2+0] = {origin.xyz, isComplex}, [i*2+1] = {windCur, windPrev, fade, collision}.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * 2 * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.extrasBuf)) || !b.extrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] extras buffer create failed");
			return false;
		}
		Util::SetResourceName(b.extrasBuf, "GrassOptimizations::ExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = capacity * 2;
		if (FAILED(device->CreateUnorderedAccessView(b.extrasBuf, &uav, &b.extrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras UAV create failed");
			return false;
		}
		Util::SetResourceName(b.extrasUAV, "GrassOptimizations::ExtrasBuf UAV");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = capacity * 2;
		if (FAILED(device->CreateShaderResourceView(b.extrasBuf, &sv, &b.extrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras SRV create failed");
			return false;
		}
		Util::SetResourceName(b.extrasSRV, "GrassOptimizations::ExtrasBuf SRV");
	}

	// Counter — CopyStructureCount does not work on a raw UAV, so the survivor count is an
	// InterlockedAdd target copied into the args with CopySubresourceRegion.
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
			return false;
		}
		Util::SetResourceName(b.counterUAV, "GrassOptimizations::CounterBuf UAV");
	}

	return true;
}

bool GrassBucketStore::CreateBucketArgsBuffer(GrassBucket& b, ID3D11Device* device)
{
	// Preferably UAV-writable, so the cull CS drops its survivor count straight in and we avoid
	// the per-bucket counter reset + CopySubresourceRegion hop (that dependency chain is what
	// stalls the indirect draws).
	//
	// 8 uints: 3 of leading pad, then the 5-uint args block at kArgsByteOffset.
	const uint32_t initArgs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = 8 * sizeof(uint32_t);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

	if (SUCCEEDED(device->CreateBuffer(&bd, &init, &b.argsBuf)) && b.argsBuf) {
		// Window the view onto instanceCount alone: the shader's address 0 maps to it, and
		// ClearUnorderedAccessViewUint resets the count without disturbing indexCount, written
		// once at first draw. FirstElement 4 == byte 16, the required 16-byte raw-UAV alignment.
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

	return true;
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

bool GrassBucketStore::DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11DeviceContext* ctx)
{
	if (auto it = complexCache.find(tex); it != complexCache.end())
		return it->second;

	bool complex = false;

	auto* rt = tex ? tex->rendererTexture : nullptr;
	if (GetDetectCS() && detectResult && detectStaging && rt && rt->resourceView && rt->height > 0) {
		struct DetectParams
		{
			uint32_t texHeight;
			float threshold;
			uint32_t pad[2];
		};
		DetectParams dp{};
		dp.texHeight = rt->height;
		dp.threshold = globals::features::grassLighting.settings.ComplexGrassThreshold;
		detectParamsCB->Update(dp);

		UINT initialCount = 0;
		ID3D11UnorderedAccessView* resultUAV = detectResult->uav.get();
		ctx->CSSetUnorderedAccessViews(0, 1, &resultUAV, &initialCount);
		ctx->CSSetShader(detectCS, nullptr, 0);
		ID3D11Buffer* paramsCB = detectParamsCB->CB();
		ctx->CSSetConstantBuffers(0, 1, &paramsCB);
		ctx->CSSetShaderResources(0, 1, &rt->resourceView);
		ctx->Dispatch(1, 1, 1);

		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ctx->CSSetShaderResources(0, 1, &nullSRV);

		// One stall per unique texture, at cell load — never per frame.
		ctx->CopyResource(detectStaging->resource.get(), detectResult->resource.get());
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(detectStaging->resource.get(), 0, D3D11_MAP_READ, 0, &m))) {
			complex = (*static_cast<const uint32_t*>(m.pData)) != 0;
			ctx->Unmap(detectStaging->resource.get(), 0);
		}
	}

	complexCache.emplace(tex, complex);
	return complex;
}

bool GrassBucketStore::EnsureLODBin(GrassBucket& b, ID3D11Device* device)
{
	if (!frameParams.enableMeshLOD || b.meshId == 0) {
		if (b.lodCapacityInstances)
			b.ReleaseLODBin();
		return false;
	}

	// Cheap after the first call — a missing .nif caches an invalid entry rather than re-Demanding.
	meshLibrary.EnsureLODMesh(b.meshId);
	if (!meshLibrary.GetLODMesh(b.meshId)) {
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
