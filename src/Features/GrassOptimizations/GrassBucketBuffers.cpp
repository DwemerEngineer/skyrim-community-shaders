#include "Features/GrassLighting.h"
#include "Features/GrassOptimizations.h"

void GrassOptimizations::SetupResources()
{
	cullParamsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CullParamsCB>(), "GrassOptimizations::CullParamsCB");
	{
		// The marker selects the engine instance stream in the optimized shader permutation.
		std::array<uint32_t, sizeof(CullBucketCB) / sizeof(uint32_t)> marker{};
		marker[0] = 0xFFFFFFFFu;
		vanillaDrawCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc(sizeof(marker), false), "GrassOptimizations::VanillaDrawCB");
		vanillaDrawCB->Update(marker);
	}

	hiZ.SetupResources();

	if (FAILED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), ctx1.put_void())) || !ctx1) {
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

bool GrassOptimizations::UploadCullBucketConstants(ID3D11DeviceContext* ctx)
{
	if (!EnsureCullBucketCapacity((uint32_t)visibleBuckets.size()))
		return false;

	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(ctx->Map(cullBucketCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return false;

	// SetupTechnique has already populated the fog color used at c16 in PerGeometry.
	static const auto* fogNearColor = reinterpret_cast<const float*>(REL::RelocationID(527805, 414756).address() + 0x100);
	static const auto* grassWindAmount = reinterpret_cast<const float*>(REL::RelocationID(527802, 414753).address());
	static const auto* grassWindAmountMax = reinterpret_cast<const float*>(REL::RelocationID(504665, 373550).address());
	static const auto* windGrassMultiplier = reinterpret_cast<const float*>(REL::RelocationID(513337, 391112).address());

	GrassPerGeometry geometry{};
	std::memcpy(geometry.fogNearColor, fogNearColor, sizeof(geometry.fogNearColor));
	geometry.windVector[2] = std::min(*grassWindAmount, *grassWindAmountMax) * *windGrassMultiplier;
	geometry.scaleMask[2] = 1.0f;

	if (auto* sunLight = GetSunLight()) {
		const auto& sunDirection = sunLight->GetWorldDirection();
		const auto& sun = sunLight->GetLightRuntimeData();

		geometry.dirLightDirection[0] = -sunDirection.x;
		geometry.dirLightDirection[1] = -sunDirection.y;
		geometry.dirLightDirection[2] = -sunDirection.z;
		geometry.dirLightColor[0] = sun.diffuse.red;
		geometry.dirLightColor[1] = sun.diffuse.green;
		geometry.dirLightColor[2] = sun.diffuse.blue;
		geometry.ambientColor[0] = sun.ambient.red;
		geometry.ambientColor[1] = sun.ambient.green;
		geometry.ambientColor[2] = sun.ambient.blue;
	}

	auto* slots = static_cast<CullBucketSlot*>(m.pData);
	uint32_t slot = 0;
	for (GrassBucket* b : visibleBuckets) {
		b->cullSlot = slot;
		auto& data = slots[slot];
		const auto& draw = b->asset.key.draw;
		data.geometry = geometry;
		data.geometry.windVector[0] = draw.windDirection[0];
		data.geometry.windVector[1] = draw.windDirection[1];
		data.geometry.scaleMask[0] = data.geometry.scaleMask[1] = draw.uniformScale ? 1.0f : 0.0f;
		data.material = b->asset.key.material.constants;
		data.cull = {
			.groupCount = b->groupCount,
			.sliceTableOffset = b->sliceTableOffset,
			.sliceCount = b->sliceTableCount,
			.dispatchWidth = DispatchWidth(b->groupCount),
			.tierStride = b->output->capacity,
			.outputOffset = b->outputOffset,
			.argsOffset = slot * kTierCount * kArgsStride,
			.boundExtent = b->boundExtent,
			.modelRadius = draw.modelRadius,
			.distScale = b->distScale,
			.minPixelScale = b->minPixelScale,
			.midLODEnabled = b->lodMeshes[0] ? 1.0f : 0.0f,
			.farLODEnabled = b->lodMeshes[1] ? 1.0f : 0.0f,
			.complexThreshold = globals::features::grassLighting.settings.ComplexGrassThreshold,
			.wavePeriod = draw.wavePeriod,
			.timeBase = timeBase,
			.prevTimeBase = prevTimeBase
		};
		++slot;
	}

	ctx->Unmap(cullBucketCB->CB(), 0);

	return true;
}

bool GrassOptimizations::UploadSliceTable(ID3D11DeviceContext* ctx)
{
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
			return false;
		}
	}

	D3D11_MAPPED_SUBRESOURCE m{};
	if (!sliceTable || !sliceTable->srv || FAILED(ctx->Map(sliceTable->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return false;

	std::memcpy(m.pData, sliceTableCPU.data(), sliceTableCPU.size() * 2 * sizeof(uint32_t));
	ctx->Unmap(sliceTable->resource.get(), 0);

	return true;
}

uint32_t GrassBucketStore::ReservedInstanceCapacity(uint32_t needed)
{
	// Keep 25% headroom, rounded to 4096 records, without overflowing the D3D11 byte width.
	const uint64_t reserved = (uint64_t)needed + ((uint64_t)needed + 3) / 4;
	return (uint32_t)std::min<uint64_t>(kMaxBucketInstances, std::max<uint64_t>(4096, (reserved + 4095) & ~uint64_t{ 4095 }));
}

uint32_t GrassBucketStore::GrownInstanceCapacity(uint32_t current, uint32_t needed)
{
	return (uint32_t)std::min<uint64_t>(kMaxBucketInstances, std::max<uint64_t>(ReservedInstanceCapacity(needed), 2ull * current));
}

bool GrassBucketStore::CanReduceInstanceCapacity(uint32_t capacity, uint32_t needed)
{
	// Require at least a 12.5% reduction before replacing an otherwise usable buffer.
	return capacity > 4096 && ReservedInstanceCapacity(needed) <= capacity - capacity / 8;
}

void GrassBucketStore::AdoptBucketBuffers(GrassBucket& b, GrassSourceBuffers& replacement, ID3D11DeviceContext* ctx)
{
	const uint32_t preserve = b.residentInstances;
	if (preserve) {
		const D3D11_BOX ibox{ 0, 0, 0, preserve * kSourceStride, 1, 1 };
		ctx->CopySubresourceRegion(replacement.instances->resource.get(), 0, 0, 0, 0, b.source.instances->resource.get(), 0, &ibox);
	}

	// Source growth leaves the per-view outputs, arguments and cached cull groups intact.
	std::swap(b.source, replacement);
}

bool GrassBucketStore::ResizeBucketAsync(GrassBucket& b, uint32_t capacity, ID3D11DeviceContext* ctx)
{
	if (b.allocation) {
		if (!b.allocation->done.load(std::memory_order_acquire))
			return false;

		const auto finished = std::exchange(b.allocation, nullptr);
		// Adopt replacements that preserve resident data and request further growth next frame if needed.
		if (finished->ok && finished->capacity >= b.residentInstances) {
			AdoptBucketBuffers(b, finished->source, ctx);
			return true;
		}
	}

	auto job = std::make_shared<BucketAllocation>();
	job->capacity = capacity;
	b.allocation = job;
	QueueAllocation(std::move(job));

	return false;
}

void GrassBucketStore::QueueAllocation(std::shared_ptr<BucketAllocation> job)
{
	if (!allocator)
		allocator = std::make_unique<BS::thread_pool<>>(1);

	allocator->detach_task([job = std::move(job)] {
		if (job.use_count() > 1)
			job->ok = CreateBucketSourceBuffers(job->source, job->capacity);
		job->done.store(true, std::memory_order_release);
	});
}

void GrassBucketStore::RetireSlices(std::vector<std::unique_ptr<GrassChunk>>& slices)
{
	if (!slices.empty()) {
		if (!allocator)
			allocator = std::make_unique<BS::thread_pool<>>(1);
		allocator->detach_task([retired = std::move(slices)] {});
	}
}

std::unique_ptr<Buffer> GrassBucketStore::CreateRawBuffer(uint64_t bytes, uint32_t bindings, uint32_t misc, const char* name)
{
	if (bytes > UINT32_MAX)
		throw std::length_error("Grass buffer exceeds D3D11 ByteWidth");

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = (UINT)bytes;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bindings;
	desc.MiscFlags = misc | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

	auto buffer = std::make_unique<Buffer>(desc, nullptr, name);
	if (bindings & D3D11_BIND_UNORDERED_ACCESS) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = desc.ByteWidth / 4;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		buffer->CreateUAV(uav);
	}

	return buffer;
}

bool GrassBucketStore::CreateBucketSourceBuffers(GrassSourceBuffers& b, uint32_t capacity)
{
	try {
		auto buffer = CreateRawBuffer((uint64_t)capacity * kSourceStride, D3D11_BIND_SHADER_RESOURCE, 0, "GrassOptimizations::InstanceBuf");
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R32_TYPELESS;
		srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		srv.BufferEx.NumElements = capacity * kSourceStride / 4;
		srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;

		buffer->CreateSRV(srv);
		b.instances = std::move(buffer);
		b.capacity = capacity;

		return true;
	} catch (...) {
		logger::error("[GRASS OPTIMIZATIONS] source buffer create failed instances={}", capacity);
		return false;
	}
}

bool GrassBucketStore::CreateBucketCullScratch(GrassCullBuffers& b, uint32_t capacity, uint32_t tiers)
{
	try {
		b.records = CreateRawBuffer((uint64_t)capacity * tiers * kCullRecordStride, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, "GrassOptimizations::CompactedBuf");
		for (uint32_t tier = 0; tier < tiers; ++tier) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
			srv.BufferEx.FirstElement = tier * capacity * kCullRecordStride / 4;
			srv.BufferEx.NumElements = capacity * kCullRecordStride / 4;
			srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;

			b.records->CreateSRV(srv);
			b.compactedSRVs[tier] = std::move(b.records->srv);
			Util::SetResourceName(b.compactedSRVs[tier].get(), "GrassOptimizations::CompactedBuf Tier%u SRV", tier);
		}

		b.capacity = capacity;
		b.tiers = tiers;

		return true;
	} catch (...) {
		logger::error("[GRASS OPTIMIZATIONS] cull output create failed instances={} tiers={}", capacity, tiers);
		return false;
	}
}

bool GrassBucketStore::EnsureArgsCapacity(uint32_t count)
{
	if (count <= argsCapacity)
		return true;

	const uint64_t capacity = std::max<uint64_t>(count, 2ull * argsCapacity);

	try {
		auto buffer = CreateRawBuffer(capacity * kTierCount * kArgsStride, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, "GrassOptimizations::ArgsBuf");
		arguments = std::move(buffer);
		argsCapacity = (uint32_t)capacity;

		return true;
	} catch (...) {
		logger::error("[GRASS OPTIMIZATIONS] args buffer create failed buckets={}", capacity);
		return false;
	}
}

bool GrassBucketStore::PrepareCullOutputs(std::span<GrassBucket*> visible)
{
	if (!EnsureArgsCapacity((uint32_t)visible.size()))
		return false;

	GrassCullBuffers* pageToShrink = nullptr;
	for (auto& page : outputPages) {
		if (!pageToShrink && page->underutilizedFrames >= 600)
			pageToShrink = page.get();
		page->used = 0;
	}

	// Place large buckets first to reduce unused space in shared output pages.
	const auto requiredInstances = [](const GrassBucket* bucket) {
		return (uint32_t)std::min({ (uint64_t)bucket->totalInstances, (uint64_t)bucket->source.capacity, (uint64_t)bucket->groupCount * kCullGroupSize });
	};

	std::sort(visible.begin(), visible.end(), [&](const GrassBucket* a, const GrassBucket* b) {
		const uint32_t aCount = requiredInstances(a), bCount = requiredInstances(b);
		return aCount != bCount ? aCount > bCount : std::less<const GrassBucket*>{}(a, b);
	});

	for (auto* bucket : visible) {
		const uint32_t needed = requiredInstances(bucket);
		const uint32_t tiers = bucket->lodMeshes[0] || bucket->lodMeshes[1] ? kTierCount : 1;
		GrassCullBuffers* selected = nullptr;

		for (auto& page : outputPages) {
			if (page.get() == pageToShrink || page->tiers != tiers || page->capacity - page->used < needed)
				continue;
			if (!selected || page->capacity - page->used < selected->capacity - selected->used)
				selected = page.get();
		}

		if (!selected) {
			const uint32_t limit = UINT32_MAX / (tiers * kCullRecordStride);
			if (needed > limit)
				return false;

			auto page = std::make_unique<GrassCullBuffers>();
			const uint32_t capacity = std::min(limit, std::max(65536u, ReservedInstanceCapacity(needed)));
			if (!CreateBucketCullScratch(*page, capacity, tiers))
				return false;

			selected = page.get();
			outputPages.push_back(std::move(page));
		}
		bucket->output = selected;
		bucket->outputOffset = selected->used;
		selected->used += needed;
	}

	for (auto& page : outputPages) {
		page->idleFrames = page->used ? 0 : page->idleFrames + 1;
		if (CanReduceInstanceCapacity(page->capacity, page->used))
			++page->underutilizedFrames;
		else
			page->underutilizedFrames = 0;
	}

	uint64_t retained = 0, activeBytes = 0;
	for (const auto& page : outputPages) {
		if (page->used)
			activeBytes += (uint64_t)page->capacity * page->tiers * kCullRecordStride;
	}

	// Retain the recent working set through camera turns, then trim it after 600 smaller frames.
	if (activeBytes >= outputReserveBytes || ++outputReserveFrames >= 600) {
		outputReserveBytes = activeBytes;
		outputReserveFrames = 0;
	}

	const uint64_t spareBudget = std::max(activeBytes / 4 + (1ull << 20), outputReserveBytes - activeBytes);
	std::stable_sort(outputPages.begin(), outputPages.end(), [](const auto& a, const auto& b) { return a->idleFrames < b->idleFrames; });
	std::erase_if(outputPages, [&](const auto& page) {
		if (page->used)
			return false;
		const uint64_t bytes = (uint64_t)page->capacity * page->tiers * kCullRecordStride;

		if (page.get() == pageToShrink || page->idleFrames >= 600 || retained + bytes > spareBudget)
			return true;
		retained += bytes;

		return false;
	});

	return true;
}

void GrassBucketStore::UploadPending(ID3D11DeviceContext* ctx)
{
	for (auto* bucket : maintenance) {
		if (!bucket->HasPendingSlices())
			continue;

		bucket->underutilizedFrames = 0;
		uint64_t needed = bucket->residentInstances;
		for (uint32_t i = bucket->uploadedSlices; i < bucket->slices.size(); ++i)
			needed += bucket->slices[i]->count;

		if (bucket->allocation && bucket->allocation->capacity < bucket->source.capacity)
			bucket->allocation.reset();

		if (bucket->source.instances && bucket->source.capacity >= needed) {
			if (bucket->allocation && bucket->allocation->capacity >= bucket->source.capacity)
				bucket->allocation.reset();
		} else if (needed <= kMaxBucketInstances) {
			ResizeBucketAsync(*bucket, GrownInstanceCapacity(bucket->source.capacity, (uint32_t)needed), ctx);
		}

	}
	uint64_t uploadBudget = kUploadBudgetBytes;
	uint64_t copyBudget = kDefragBudgetBytes;
	bool shrinkAttempted = false;

	for (auto it = maintenance.begin(); it != maintenance.end();) {
		auto& bucket = **it;

		if (frameParams.enableMeshLOD && bucket.HasPendingSlices())
			meshLibrary.EnsureLODMeshes(*bucket.asset.key.mesh);

		if (bucket.HasPendingSlices() && bucket.source.instances && uploadBudget)
			AppendNewSlices(bucket, uploadBudget, ctx);

		if (bucket.fragmented && copyBudget)
			bucket.fragmented = !DefragBucket(bucket, copyBudget, ctx);

		const bool canShrink = !bucket.HasPendingSlices() && !bucket.fragmented && CanReduceInstanceCapacity(bucket.source.capacity, bucket.totalInstances);
		if (canShrink) {
			if (++bucket.underutilizedFrames >= 600 && !shrinkAttempted) {
				shrinkAttempted = true;
				if (ResizeBucketAsync(bucket, ReservedInstanceCapacity(bucket.totalInstances), ctx))
					bucket.underutilizedFrames = 0;
			}
		} else {
			bucket.underutilizedFrames = 0;
			if (bucket.allocation && bucket.allocation->capacity < bucket.source.capacity)
				bucket.allocation.reset();
		}

		if (bucket.HasPendingSlices() || bucket.fragmented || canShrink)
			++it;
		else
			it = maintenance.erase(it);
	}
}

void GrassBucketStore::AppendNewSlices(GrassBucket& bucket, uint64_t& budgetBytes, ID3D11DeviceContext* ctx)
{
	// Allow one oversized slice per frame so uploads always make progress.
	const uint64_t roomBytes = (uint64_t)(bucket.source.capacity - bucket.residentInstances) * kSourceStride;
	uint32_t to = bucket.uploadedSlices;
	uint64_t bytes = 0;
	bool outOfBudget = false;
	for (; to < (uint32_t)bucket.slices.size(); ++to) {
		const uint64_t sliceBytes = (uint64_t)bucket.slices[to]->count * kSourceStride;
		if (bytes + sliceBytes > roomBytes)
			break;
		if (bytes + sliceBytes > budgetBytes && (bytes || budgetBytes < kUploadBudgetBytes)) {
			outOfBudget = true;
			break;
		}
		bytes += sliceBytes;
	}

	if (to == bucket.uploadedSlices) {
		// A full buffer waits for its replacement without taking the budget from other buckets.
		if (outOfBudget)
			budgetBytes = 0;
		return;
	}

	UploadSlices(bucket, bucket.uploadedSlices, to, bucket.residentInstances, ctx);
	budgetBytes -= std::min(bytes, budgetBytes);
	bucket.uploadedSlices = to;
}

void GrassBucketStore::UploadSlices(GrassBucket& bucket, uint32_t from, uint32_t to, uint32_t prefix, ID3D11DeviceContext* ctx)
{
	// Sum the slice counts because resident records may contain removal gaps.
	uint32_t instances = 0;
	for (uint32_t i = from; i < to; ++i)
		instances += bucket.slices[i]->count;
	if (!instances)
		return;

	// Reuse storage across cell loads and upload the flattened range in one call.
	static thread_local std::vector<uint8_t> records;
	records.clear();
	records.reserve((size_t)instances * kSourceStride);

	uint32_t offset = prefix;
	for (uint32_t i = from; i < to; ++i) {
		auto& slice = *bucket.slices[i];
		slice.bufferOffset = offset;
		records.insert(records.end(), slice.data.begin(), slice.data.end());
		offset += slice.count;
	}

	const D3D11_BOX box{ prefix * kSourceStride, 0, 0, offset * kSourceStride, 1, 1 };
	ctx->UpdateSubresource(bucket.source.instances->resource.get(), 0, &box, records.data(), 0, 0);
	bucket.residentInstances = offset;

	for (uint32_t i = from; i < to; ++i)
		UpdateShapeResidency(*bucket.slices[i], true);
}

bool GrassBucketStore::DefragBucket(GrassBucket& b, uint64_t& budgetBytes, ID3D11DeviceContext* ctx)
{
	if (!b.source.instances)
		return true;

	uint32_t next = 0;
	for (uint32_t cursor = 0; cursor < b.uploadedSlices;) {
		auto& slice = *b.slices[cursor];
		uint32_t end = cursor + 1;

		if (slice.bufferOffset > next) {
			uint32_t count = slice.count;
			uint64_t bytes = (uint64_t)count * kSourceStride * 2;

			// Always allow one move per frame so a slice larger than the budget still makes progress.
			if (bytes > budgetBytes && budgetBytes < kDefragBudgetBytes) {
				budgetBytes = 0;
				return false;
			}

			// Move adjacent slices together to reduce GPU copy calls.
			while (end < b.uploadedSlices) {
				const auto& following = *b.slices[end];
				const uint64_t combinedBytes = bytes + (uint64_t)following.count * kSourceStride * 2;
				if (following.bufferOffset != slice.bufferOffset + count || combinedBytes > budgetBytes)
					break;
				count += following.count;
				bytes = combinedBytes;
				++end;
			}

			if (!MoveInstances(b, slice.bufferOffset, next, count, ctx)) {
				// Preserve resident records and retry next frame if scratch allocation fails.
				budgetBytes = 0;
				return false;
			}

			budgetBytes -= std::min(bytes, budgetBytes);

			for (uint32_t moved = cursor; moved < end; ++moved) {
				auto& movedSlice = *b.slices[moved];
				movedSlice.bufferOffset = next;

				if (b.clustersValid && movedSlice.firstCullGroup != UINT32_MAX && moved < b.clusteredSlices) {
					const uint32_t groups = (movedSlice.count + kCullGroupSize - 1) / kCullGroupSize;
					for (uint32_t g = 0; g < groups; ++g)
						b.cullGroups[movedSlice.firstCullGroup + g].sourceStart = next + g * kCullGroupSize;
					b.dirtyGroupRanges.emplace_back(movedSlice.firstCullGroup, movedSlice.firstCullGroup + groups);
				}

				next += movedSlice.count;
			}
		} else {
			next = slice.bufferOffset + slice.count;
		}
		cursor = end;
	}

	b.residentInstances = next;
	return true;
}

bool GrassBucketStore::MoveInstances(GrassBucket& b, uint32_t oldFirst, uint32_t newFirst, uint32_t count, ID3D11DeviceContext* ctx)
{
	// Copy toward lower offsets through bounded scratch storage so unread source records remain intact.
	constexpr uint32_t kChunkInstances = 16384;
	if (!compactionScratch) {
		try {
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = kChunkInstances * kSourceStride;
			bd.Usage = D3D11_USAGE_DEFAULT;
			compactionScratch = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::CompactionScratch");
		} catch (...) {
			return false;
		}
	}
	for (uint32_t offset = 0; offset < count;) {
		const uint32_t chunk = std::min(kChunkInstances, count - offset);
		const UINT recordBytes = chunk * kSourceStride;
		const uint32_t from = oldFirst + offset;
		const uint32_t to = newFirst + offset;

		const D3D11_BOX records{ from * kSourceStride, 0, 0, from * kSourceStride + recordBytes, 1, 1 };
		ctx->CopySubresourceRegion(compactionScratch->resource.get(), 0, 0, 0, 0, b.source.instances->resource.get(), 0, &records);
		const D3D11_BOX scratchRecords{ 0, 0, 0, recordBytes, 1, 1 };
		ctx->CopySubresourceRegion(b.source.instances->resource.get(), 0, to * kSourceStride, 0, 0, compactionScratch->resource.get(), 0, &scratchRecords);
		offset += chunk;
	}

	return true;
}

bool GrassBucketStore::UploadCullGroups(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	const uint32_t count = (uint32_t)b.cullGroups.size();
	if (count > b.groupCapacity || !b.groups) {

		uint32_t cap = b.groupCapacity ? b.groupCapacity : 256;
		while (cap < count)
			cap *= 2;

		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * sizeof(CullGroup);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(CullGroup);

		try {
			auto groups = std::make_unique<Buffer>(bd, nullptr, "GrassOptimizations::GroupBuf");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.NumElements = cap;
			groups->CreateSRV(srv);
			b.groups = std::move(groups);
		} catch (...) {
			logger::error("[GRASS OPTIMIZATIONS] group buffer create failed groups={}", cap);
			return false;
		}

		b.groupCapacity = cap;
		b.residentGroups = 0;
	}

	// Upload groups appended at the tail plus each resident range emptied or moved since the last upload.
	auto& ranges = b.dirtyGroupRanges;
	if (count > b.residentGroups)
		ranges.emplace_back(b.residentGroups, count);

	std::sort(ranges.begin(), ranges.end());
	constexpr uint32_t kMergeGap = 64;  // Neighbouring ranges share one update rather than many tiny ones.

	for (size_t i = 0; i < ranges.size();) {
		uint32_t from = ranges[i].first;
		uint32_t to = ranges[i].second;

		for (++i; i < ranges.size() && ranges[i].first <= to + kMergeGap; ++i)
			to = std::max(to, ranges[i].second);
		to = std::min(to, count);

		if (to > from) {
			const D3D11_BOX box{ from * (UINT)sizeof(CullGroup), 0, 0, to * (UINT)sizeof(CullGroup), 1, 1 };
			ctx->UpdateSubresource(b.groups->resource.get(), 0, &box, b.cullGroups.data() + from, 0, 0);
		}
	}

	ranges.clear();
	b.residentGroups = count;

	return true;
}
