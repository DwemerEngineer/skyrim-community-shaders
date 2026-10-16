#include "Features/GrassOptimizations.h"
#include "Features/GrassLighting.h"
#include "Profiler.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"

// The widest allocation is kGrassStride bytes per instance, so a larger capacity wraps its ByteWidth
// and silently under-allocates. Bounding capacityInstances here also bounds EnsureLODBin's cap.
constexpr uint32_t kMaxBucketInstances = UINT32_MAX / kGrassStride;

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

bool GrassBucketStore::CompactBucket(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	if (!b.instanceBuf || !b.capacityInstances)
		return false;

	const uint32_t surviving = (b.firstNewSlice == UINT32_MAX) ? (uint32_t)b.slices.size() : b.firstNewSlice;

	// Group contiguous survivors before closing removal gaps through the shared scratch buffer.
	struct Run
	{
		uint32_t oldFirst;
		uint32_t newFirst;
		uint32_t instances;
	};
	static thread_local std::vector<Run> runs;
	runs.clear();

	uint32_t newOffset = 0;
	for (uint32_t i = 0; i < surviving; ++i) {
		const BucketSlice& s = b.slices[i];
		if (s.bufferOffset == UINT32_MAX || !s.count)
			return false;  // never uploaded, so there is nothing resident to shift

		if (!runs.empty() && s.bufferOffset == runs.back().oldFirst + runs.back().instances)
			runs.back().instances += s.count;
		else
			runs.push_back({ s.bufferOffset, newOffset, s.count });

		newOffset += s.count;
	}

	if (runs.empty())
		return false;

	// Only trailing slices went, so everything still resident is already where it belongs.
	if (runs.size() == 1 && runs[0].oldFirst == 0) {
		b.residentInstances = newOffset;
		return true;
	}

	// Reuse a bounded scratch buffer to avoid allocating another full-capacity source buffer on cell unload.
	// Copy chunks toward lower offsets so later source chunks remain intact.
	constexpr uint32_t kChunkInstances = 16384;
	if (!compactionScratch) {
		try {
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = kChunkInstances * kGrassStride;
			bd.Usage = D3D11_USAGE_DEFAULT;
			compactionScratch = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::CompactionScratch");
		} catch (...) {
			return false;  // No offsets or resident data have changed.
		}
	}
	for (const Run& run : runs) {
		if (run.oldFirst == run.newFirst)
			continue;
		for (uint32_t offset = 0; offset < run.instances;) {
			const uint32_t count = std::min(kChunkInstances, run.instances - offset);
			const UINT recordBytes = count * kGrassStride;
			const uint32_t oldFirst = run.oldFirst + offset;
			const uint32_t newFirst = run.newFirst + offset;
			const D3D11_BOX records{ oldFirst * kGrassStride, 0, 0, oldFirst * kGrassStride + recordBytes, 1, 1 };
			ctx->CopySubresourceRegion(compactionScratch->resource.get(), 0, 0, 0, 0, b.instanceBuf, 0, &records);
			const D3D11_BOX scratchRecords{ 0, 0, 0, recordBytes, 1, 1 };
			ctx->CopySubresourceRegion(b.instanceBuf, 0, newFirst * kGrassStride, 0, 0, compactionScratch->resource.get(), 0, &scratchRecords);
			offset += count;
		}
	}

	newOffset = 0;
	for (uint32_t i = 0; i < surviving; ++i) {
		b.slices[i].bufferOffset = newOffset;
		newOffset += b.slices[i].count;
	}

	b.residentInstances = newOffset;
	return true;
}

void GrassBucketStore::UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	const auto canShrink = [](const GrassBucket& b) {
		return !b.dirty && !b.needsCompact && b.firstNewSlice == UINT32_MAX &&
		       b.capacityInstances > 4096 && b.totalInstances <= b.capacityInstances / 4;
	};
	for (auto it = pendingUploads.begin(); it != pendingUploads.end();) {
		auto& b = **it;
		if (frameParams.enableMeshLOD && b.firstNewSlice != UINT32_MAX)
			meshLibrary.EnsureLODMeshes(b.meshId);
		if (b.needsCompact) {
			b.needsCompact = false;
			if (CompactBucket(b, ctx)) {
				b.rebuildFromSlice = UINT32_MAX;  // survivors sit contiguously again
			} else {
				b.dirty = true;
			}
		}

		if (b.dirty) {
			RebuildBucket(b, device, ctx);
		} else if (b.firstNewSlice != UINT32_MAX) {
			if (b.totalInstances > b.capacityInstances)
				RebuildBucket(b, device, ctx);
			else
				AppendNewSlices(b, ctx);
		}

		if (canShrink(b)) {
			pendingShrinks.insert(&b);
		} else {
			pendingShrinks.erase(&b);
			b.underutilizedFrames = 0;
		}
		// Keep allocation/upload failures queued for the next grass frame.
		if (b.dirty || b.needsCompact || b.firstNewSlice != UINT32_MAX)
			++it;
		else
			it = pendingUploads.erase(it);
	}

	// Shrink after 600 grass updates, leaving at least twice the required capacity.
	// Shrink before culling and drawing; D3D11 retains resources referenced by queued commands.
	for (auto it = pendingShrinks.begin(); it != pendingShrinks.end();) {
		auto& b = **it;
		if (++b.underutilizedFrames >= 600) {
			uint32_t cap = 4096;
			while (cap < b.totalInstances * 2)
				cap *= 2;
			ResizeBucketBuffers(b, cap, device, ctx, b.residentInstances);
			b.underutilizedFrames = 0;
		}
		if (canShrink(b))
			++it;
		else
			it = pendingShrinks.erase(it);
	}
}

void GrassBucketStore::RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	// Preserve the resident prefix and upload only the stale or newly appended tail.
	uint32_t from = std::min(bucket.rebuildFromSlice, bucket.firstNewSlice);
	if (from == UINT32_MAX || from > (uint32_t)bucket.slices.size())
		from = 0;

	// Removals record the first stale slice; everything below it is still contiguous.
	// Ordinary growth preserves the resident prefix without walking it again.
	uint32_t startInstance = 0;
	if (from) {
		const auto& s = bucket.slices[from - 1];
		if (s.bufferOffset == UINT32_MAX) {
			from = 0;
		} else {
			startInstance = s.bufferOffset + s.count;
		}
	}

	// Copy from the old buffer starting with the given start instance to the new buffer on the GPU, so the rebuild below still only uploads the tail.
	if (!EnsureBucketCapacity(bucket, bucket.totalInstances, device, ctx, startInstance))
		return;

	if (!bucket.totalInstances) {
		bucket.dirty = false;
		bucket.rebuildFromSlice = UINT32_MAX;
		bucket.firstNewSlice = UINT32_MAX;
		bucket.residentInstances = 0;
		return;
	}

	UploadSliceTail(bucket, from, startInstance, ctx);

	bucket.dirty = false;
	bucket.residentInstances = bucket.totalInstances;
	bucket.rebuildFromSlice = UINT32_MAX;
	bucket.firstNewSlice = UINT32_MAX;
}

void GrassBucketStore::AppendNewSlices(GrassBucket& bucket, ID3D11DeviceContext* ctx)
{
	UploadSliceTail(bucket, bucket.firstNewSlice, bucket.residentInstances, ctx);
	bucket.firstNewSlice = UINT32_MAX;
}

void GrassBucketStore::UploadSliceTail(GrassBucket& bucket, uint32_t from, uint32_t prefix, ID3D11DeviceContext* ctx)
{
	const uint32_t tailInstances = bucket.totalInstances - prefix;
	if (!tailInstances)
		return;

	// Reuse storage across cell loads and upload the flattened tail in one call.
	static thread_local std::vector<uint8_t> records;
	records.clear();
	records.reserve((size_t)tailInstances * kGrassStride);

	uint32_t offset = prefix;
	for (uint32_t i = from; i < (uint32_t)bucket.slices.size(); ++i) {
		auto& slice = bucket.slices[i];
		slice.bufferOffset = offset;
		records.insert(records.end(), slice.data.begin(), slice.data.end());
		offset += slice.count;
	}

	const D3D11_BOX box{ prefix * kGrassStride, 0, 0, offset * kGrassStride, 1, 1 };
	ctx->UpdateSubresource(bucket.instanceBuf, 0, &box, records.data(), 0, 0);
	bucket.residentInstances = offset;
}

bool GrassBucketStore::EnsureBucketCapacity(GrassBucket& b, uint32_t needed, ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t preserveInstances)
{
	if (b.capacityInstances >= needed && b.instanceBuf)
		return true;

	if (needed > kMaxBucketInstances) {
		logger::error("[GRASS OPTIMIZATIONS] bucket capacity rejected: needed={} max={}", needed, kMaxBucketInstances);
		return false;
	}

	uint32_t cap = b.capacityInstances ? b.capacityInstances : 4096;
	while (cap < needed)
		cap = std::min(cap * 2, kMaxBucketInstances);

	return ResizeBucketBuffers(b, cap, device, ctx, preserveInstances);
}

bool GrassBucketStore::ResizeBucketBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t preserveInstances)
{
	// Allocate every replacement before releasing anything, so failed growth/shrink is retryable.
	GrassBucket replacement;
	if (!CreateBucketSourceBuffers(replacement, capacity, device) ||
		!CreateBucketCullScratch(replacement, capacity, device) ||
		!CreateBucketCounters(replacement, device))
		return false;

	const uint32_t preserve = std::min({ preserveInstances, b.residentInstances, capacity });
	if (preserve) {
		const D3D11_BOX ibox{ 0, 0, 0, preserve * kGrassStride, 1, 1 };
		ctx->CopySubresourceRegion(replacement.instanceBuf, 0, 0, 0, 0, b.instanceBuf, 0, &ibox);
	}

	b.ReleaseResources();  // This also retires LOD bins so their capacity and arguments are recreated.
	std::swap(b.instanceBuf, replacement.instanceBuf);
	std::swap(b.instanceSRV, replacement.instanceSRV);
	std::swap(b.compactedBuf, replacement.compactedBuf);
	std::swap(b.compactedUAV, replacement.compactedUAV);
	std::swap(b.extrasBuf, replacement.extrasBuf);
	std::swap(b.extrasUAV, replacement.extrasUAV);
	std::swap(b.extrasSRV, replacement.extrasSRV);
	std::swap(b.lodCounterBuf, replacement.lodCounterBuf);
	std::swap(b.lodCounterUAV, replacement.lodCounterUAV);
	std::swap(b.argsBuf, replacement.argsBuf);
	std::swap(b.argsUAV, replacement.argsUAV);
	b.capacityInstances = capacity;
	b.residentInstances = preserve;
	return true;
}

bool GrassBucketStore::CreateBucketSourceBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * kGrassStride;
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
	return true;
}

bool GrassBucketStore::CreateBucketCullScratch(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	// Compacted survivors, consumed as an instanced vertex stream by the draw.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * kGrassStride;
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

	return true;
}

bool GrassBucketStore::CreateBucketCounters(GrassBucket& b, ID3D11Device* device)
{
	{
		const uint32_t initArgs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(initArgs);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, &init, &b.argsBuf)) || !b.argsBuf) {
			logger::error("[GRASS OPTIMIZATIONS] args buffer create failed");
			return false;
		}
		Util::SetResourceName(b.argsBuf, "GrassOptimizations::ArgsBuf");

		// The shader's address 0 maps to the instance count, so clearing the view leaves the index count alone.
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = instanceCountOffset / sizeof(uint32_t);
		uav.Buffer.NumElements = 1;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.argsBuf, &uav, &b.argsUAV)) || !b.argsUAV) {
			logger::error("[GRASS OPTIMIZATIONS] args UAV create failed");
			return false;
		}
		Util::SetResourceName(b.argsUAV, "GrassOptimizations::ArgsBuf UAV");
	}

	const uint32_t initLODCounts[(size_t)GrassMeshLibrary::LODTier::kCount] = {};
	D3D11_SUBRESOURCE_DATA init{ initLODCounts, 0, 0 };
	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = sizeof(initLODCounts);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_TYPELESS;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = (UINT)std::size(initLODCounts);
	uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(device->CreateBuffer(&bd, &init, &b.lodCounterBuf)) || !b.lodCounterBuf) {
		logger::error("[GRASS OPTIMIZATIONS] LOD counter buffer create failed");
		return false;
	}
	Util::SetResourceName(b.lodCounterBuf, "GrassOptimizations::LODCounterBuf");

	if (FAILED(device->CreateUnorderedAccessView(b.lodCounterBuf, &uav, &b.lodCounterUAV)) || !b.lodCounterUAV) {
		logger::error("[GRASS OPTIMIZATIONS] LOD counter UAV create failed");
		return false;
	}
	Util::SetResourceName(b.lodCounterUAV, "GrassOptimizations::LODCounterBuf UAV");

	return true;
}

bool GrassBucketStore::UploadCullGroups(GrassBucket& b, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	const uint32_t count = (uint32_t)b.cullGroups.size();
	if (count > b.groupCapacity || !b.groupBuf) {
		uint32_t cap = b.groupCapacity ? b.groupCapacity : 256;
		while (cap < count)
			cap *= 2;

		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * sizeof(CullGroup);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(CullGroup);
		ID3D11Buffer* buf = nullptr;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &buf)) || !buf) {
			logger::error("[GRASS OPTIMIZATIONS] group buffer create failed groups={}", cap);
			return false;
		}
		Util::SetResourceName(buf, "GrassOptimizations::GroupBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap;
		ID3D11ShaderResourceView* srv = nullptr;
		if (FAILED(device->CreateShaderResourceView(buf, &sv, &srv)) || !srv) {
			logger::error("[GRASS OPTIMIZATIONS] group SRV create failed");
			buf->Release();
			return false;
		}
		Util::SetResourceName(srv, "GrassOptimizations::GroupBuf SRV");

		if (b.groupSRV)
			b.groupSRV->Release();
		if (b.groupBuf)
			b.groupBuf->Release();
		b.groupBuf = buf;
		b.groupSRV = srv;
		b.groupCapacity = cap;
		b.residentGroups = 0;
	}

	// Runs only grow at the tail until a removal rebuilds them, so only new groups are uploaded.
	if (count > b.residentGroups) {
		const D3D11_BOX box{ b.residentGroups * (UINT)sizeof(CullGroup), 0, 0, count * (UINT)sizeof(CullGroup), 1, 1 };
		ctx->UpdateSubresource(b.groupBuf, 0, &box, b.cullGroups.data() + b.residentGroups, 0, 0);
		b.residentGroups = count;
	}
	return true;
}

bool GrassBucketStore::EnsureLODBin(GrassBucket& b, GrassMeshLibrary::LODTier tier, ID3D11Device* device)
{
	GrassBucket::LODBin& bin = b.lodBins[(size_t)tier];
	const bool tierEnabled = (tier == GrassMeshLibrary::LODTier::kFar) ? frameParams.enableFarLOD : frameParams.enableMidLOD;

	if (!frameParams.enableMeshLOD || !tierEnabled || b.meshId == 0) {
		if (bin.capacityInstances)
			bin.Release();
		return false;
	}

	meshLibrary.EnsureLODMeshes(b.meshId);
	if (!meshLibrary.GetLODMesh(b.meshId, tier)) {
		if (bin.capacityInstances)
			bin.Release();
		return false;
	}

	const uint32_t cap = b.capacityInstances;
	if (!cap)
		return false;
	if (bin.capacityInstances >= cap && bin.compactedBuf)
		return true;

	// Worst case every survivor takes this tier's mesh, so the bin is sized like the full-detail one.
	bin.Release();

	const char* tierName = (tier == GrassMeshLibrary::LODTier::kFar) ? "far" : "middle";

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * kGrassStride;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &bin.compactedBuf)) || !bin.compactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD compacted buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.compactedBuf, "GrassOptimizations::LODCompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(bin.compactedBuf, &uav, &bin.compactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD compacted UAV create failed", tierName);
			bin.Release();
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
		if (FAILED(device->CreateBuffer(&bd, nullptr, &bin.extrasBuf)) || !bin.extrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.extrasBuf, "GrassOptimizations::LODExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 2;
		uav.Buffer.Flags = 0;
		if (FAILED(device->CreateUnorderedAccessView(bin.extrasBuf, &uav, &bin.extrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras UAV create failed", tierName);
			bin.Release();
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap * 2;
		if (FAILED(device->CreateShaderResourceView(bin.extrasBuf, &sv, &bin.extrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras SRV create failed", tierName);
			bin.Release();
			return false;
		}
	}

	{
		const uint32_t initArgs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 8 * sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;

		if (FAILED(device->CreateBuffer(&bd, &init, &bin.argsBuf)) || !bin.argsBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD args buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.argsBuf, "GrassOptimizations::LODArgsBuf");
	}

	bin.capacityInstances = cap;
	return true;
}
