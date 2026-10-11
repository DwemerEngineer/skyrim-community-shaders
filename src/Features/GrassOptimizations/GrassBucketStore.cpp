#include "GrassBucketStore.h"

#include "TruePBR/BSLightingShaderMaterialPBR.h"

void GrassBucketStore::SetupResources()
{
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
}

void GrassBucketStore::ClearShaderCache()
{
	if (detectCS)
		detectCS->Release();
	detectCS = nullptr;
	detectCSFailed = false;
}

ID3D11ComputeShader* GrassBucketStore::GetDetectCS()
{
	if (!detectCS && !detectCSFailed) {
		detectCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassOptimizations\\DetectComplexCS.hlsl", {}, "cs_5_0"));
		if (!detectCS) {
			detectCSFailed = true;
			logger::error("[GRASS OPTIMIZATIONS] detect CS load failed — complex detection disabled");
		}
	}
	return detectCS;
}

void GrassBucketStore::ProcessRemovals()
{
	std::vector<ShapeIdentity> rems;
	{
		std::scoped_lock lk(pendingMutex);
		rems.swap(pendingRemoves);
		std::erase_if(pendingCaptures, [this](const PendingCapture& pc) {
			auto it = liveShapes.find(pc.shape);
			return it == liveShapes.end() || it->second != pc.generation;
		});
	}
	std::sort(rems.begin(), rems.end());
	rems.erase(std::unique(rems.begin(), rems.end()), rems.end());
	ApplyRemovals(rems);
	if (buckets.empty())
		compactionScratch.reset();
}

void GrassBucketStore::ApplyPending(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	std::vector<PendingCapture> caps;
	std::vector<ShapeIdentity> rems;
	{
		std::scoped_lock lk(pendingMutex);
		caps.swap(pendingCaptures);
		rems.swap(pendingRemoves);
	}
	std::sort(rems.begin(), rems.end());
	rems.erase(std::unique(rems.begin(), rems.end()), rems.end());
	ApplyRemovals(rems);
	static thread_local std::vector<BucketKey> emptied;
	emptied.clear();
	{
		// Keep engine loading and GPU work outside the lock shared by capture commits and destruction.
		std::scoped_lock lk(pendingMutex);
		std::erase_if(caps, [this](const PendingCapture& pc) {
			auto it = liveShapes.find(pc.shape);
			return it == liveShapes.end() || it->second != pc.generation;
		});
		ApplyCaptures(caps, emptied);
	}
	// Release LOD trees outside pendingMutex because their destruction can reenter the removal hook.
	// A later capture in the batch may have refilled a bucket, so only erase ones still empty.
	for (const auto& key : emptied) {
		if (auto it = buckets.find(key); it != buckets.end() && it->second.slices.empty())
			EraseBucket(it->second);
	}
	UploadDirtyBuckets(device, ctx);
}

void GrassBucketStore::StageRemoval(RE::BSMultiStreamInstanceTriShape* shape)
{
	std::scoped_lock lk(pendingMutex);
	if (auto it = liveShapes.find(shape); it != liveShapes.end()) {
		pendingRemoves.push_back({ shape, it->second });
		std::unique_lock shapeLock(shapeBucketMutex);
		if (auto bucket = shapeBucketId.find(shape); bucket != shapeBucketId.end())
			bucket->second.current->optimizedReady.store(false, std::memory_order_relaxed);
		liveShapes.erase(it);
	}
	// Forget the shape before the engine frees its address so a later model-path registration survives.
	meshLibrary.ForgetShape(shape);
}

GrassBucket* GrassBucketStore::FindBucket(RE::BSMultiStreamInstanceTriShape* shape)
{
	std::shared_lock lk(shapeBucketMutex);
	auto it = shapeBucketId.find(shape);
	return it == shapeBucketId.end() ? nullptr : it->second.current;
}

bool GrassBucketStore::ClaimQueueSlot(RE::BSMultiStreamInstanceTriShape* shape, uint32_t frame)
{
	std::shared_lock lk(shapeBucketMutex);

	auto it = shapeBucketId.find(shape);
	// A bucket not ready for optimized drawing falls back to vanilla per-shape drawing, so all shapes must still be queued.
	// Deliberately read without bucketMutex, which UpdateGrass holds whole and would serialise every culling job.
	if (it == shapeBucketId.end() || !it->second.current->optimizedReady.load(std::memory_order_relaxed))
		return true;

	auto& lastQueued = it->second.current->lastQueuedFrame;
	uint32_t prev = lastQueued.load(std::memory_order_relaxed);
	if (prev == frame)
		return false;
	return lastQueued.compare_exchange_strong(prev, frame, std::memory_order_relaxed);
}

void GrassBucketStore::ApplyRemovals(const std::vector<ShapeIdentity>& removes)
{
	if (removes.empty())
		return;

	// ApplyPending supplies sorted, unique shape keys. Gather before erasing the lookup entries.
	static thread_local std::vector<GrassBucket*> affected;
	affected.clear();
	{
		std::unique_lock lk(shapeBucketMutex);
		for (const auto& dead : removes) {
			if (auto it = shapeBucketId.find(dead.shape); it != shapeBucketId.end() && it->second.generation == dead.generation) {
				affected.push_back(it->second.current);
				shapeBucketId.erase(it);
			}
		}
	}
	std::sort(affected.begin(), affected.end(), std::less<GrassBucket*>{});
	affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

	const auto isDead = [&removes](const BucketSlice& slice) {
		return std::binary_search(removes.begin(), removes.end(), ShapeIdentity{ slice.shape, slice.generation });
	};
	// The exclusive lock above retired every reference to an emptied bucket and waited for culling readers.
	for (auto* bucket : affected) {
		if (ExtractSlices(*bucket, isDead, nullptr))
			EraseBucket(*bucket);
	}
}

void GrassBucketStore::ApplyCaptures(std::vector<PendingCapture>& captures, std::vector<BucketKey>& emptied)
{
	// Queue pending adds so additions can be processed under one lock
	static thread_local std::vector<std::pair<ShapeIdentity, GrassBucket*>> pendingMapAdds;
	pendingMapAdds.clear();

	for (auto& pc : captures) {
		const uint32_t meshId = meshLibrary.ResolveMeshId(pc.shape);
		BucketKey bk{ meshId, pc.material, meshId ? nullptr : pc.diffuseTexture.get(), pc.triangleCount, meshId ? 0u : pc.descVal };
		bk.material = CanonicalMaterial(bk, pc.material, pc.pbrMaterial);
		auto [it, inserted] = buckets.try_emplace(bk);
		auto& b = it->second;
		if (inserted) {
			b.key = bk;
			b.meshId = meshId;
			b.material.reset(bk.material);
			b.pbrMaterial = pc.pbrMaterial;
			keysByMesh[meshId].push_back(bk);
			meshLibrary.RetainMesh(meshId);
		}
		if (b.diffuseTexture != pc.diffuseTexture) {
			ReleaseComplexTexture(b.diffuseTexture.get());
			b.diffuseTexture = pc.diffuseTexture;
			++complexCache[b.diffuseTexture.get()].activeUsers;
			b.complexSRV = nullptr;
		}

		CacheBucketTypeParams(b, pc);
		BucketSlice s;
		s.shape = pc.shape;
		s.generation = pc.generation;
		s.count = pc.count;
		s.fadeStart = frameParams.fadeStart;
		s.origin = pc.origin;
		s.localMin = pc.localMin;
		s.localMax = pc.localMax;
		s.data = std::move(pc.bytes);
		AppendSlice(b, std::move(s));
		if (pc.shape)
			pendingMapAdds.emplace_back(ShapeIdentity{ pc.shape, pc.generation }, &b);
	}

	if (pendingMapAdds.empty())
		return;

	// Move a shape's earlier slices when its model path or material changes so every bucket keeps a draw representative.
	struct Migration
	{
		GrassBucket* from;
		GrassBucket* to;
		ShapeIdentity previous;
		bool sameShape;
	};
	static thread_local std::vector<Migration> migrations;
	migrations.clear();
	{
		std::unique_lock lk(shapeBucketMutex);
		for (const auto& [identity, bucket] : pendingMapAdds) {
			auto& entry = shapeBucketId[identity.shape];
			if (entry.current && entry.current != bucket)
				migrations.push_back({ entry.current, bucket, ShapeIdentity{ identity.shape, entry.generation }, entry.generation == identity.generation });
			entry.generation = identity.generation;
			entry.current = bucket;
		}
	}
	for (const auto& m : migrations) {
		if (m.sameShape) {
			MigrateShapeSlices(*m.from, *m.to, m.previous, emptied);
			continue;
		}
		// The address was reused before the old shape's removal arrived; its slices are dead.
		const auto isPrevious = [&m](const BucketSlice& slice) {
			return slice.shape == m.previous.shape && slice.generation == m.previous.generation;
		};
		if (ExtractSlices(*m.from, isPrevious, nullptr))
			emptied.push_back(m.from->key);
	}
}

void GrassBucketStore::AppendSlice(GrassBucket& b, BucketSlice&& slice)
{
	pendingUploads.insert(&b);
	if (b.firstNewSlice == UINT32_MAX)
		b.firstNewSlice = (uint32_t)b.slices.size();

	slice.bufferOffset = UINT32_MAX;

	SliceBounds sb;
	sb.lo[0] = slice.origin.x + slice.localMin.x;
	sb.lo[1] = slice.origin.y + slice.localMin.y;
	sb.lo[2] = slice.origin.z + slice.localMin.z;
	sb.hi[0] = slice.origin.x + slice.localMax.x;
	sb.hi[1] = slice.origin.y + slice.localMax.y;
	sb.hi[2] = slice.origin.z + slice.localMax.z;

	// Extend valid padded bounds without revisiting existing slices.
	// Recompute invalidated bounds in UpdateCoarseBounds after removals.
	if (b.coarseValid || b.slices.empty()) {
		const float pad = b.modelRadius + 128.0f;
		const RE::NiPoint3 lo{ sb.lo[0] - pad, sb.lo[1] - pad, sb.lo[2] - pad };
		const RE::NiPoint3 hi{ sb.hi[0] + pad, sb.hi[1] + pad, sb.hi[2] + pad };
		if (b.slices.empty()) {
			b.coarseMin = lo;
			b.coarseMax = hi;
		} else {
			b.coarseMin.x = std::min(b.coarseMin.x, lo.x);
			b.coarseMin.y = std::min(b.coarseMin.y, lo.y);
			b.coarseMin.z = std::min(b.coarseMin.z, lo.z);
			b.coarseMax.x = std::max(b.coarseMax.x, hi.x);
			b.coarseMax.y = std::max(b.coarseMax.y, hi.y);
			b.coarseMax.z = std::max(b.coarseMax.z, hi.z);
		}
		b.coarseValid = true;
	}
	b.totalInstances += slice.count;
	b.slices.push_back(std::move(slice));
	b.sliceBounds.push_back(sb);
}

template <class Pred>
bool GrassBucketStore::ExtractSlices(GrassBucket& b, Pred isTarget, std::vector<BucketSlice>* moved)
{
	const size_t before = b.slices.size();

	// Record the first removed slice so rebuilding only uploads the affected tail.
	uint32_t firstRemoved = UINT32_MAX;
	size_t write = 0;
	uint32_t removedInstances = 0;
	// Remap firstNewSlice after compaction so pending slices remain in the upload range.
	uint32_t remappedFirstNew = UINT32_MAX;

	for (size_t read = 0; read < before; ++read) {
		if ((uint32_t)read == b.firstNewSlice)
			remappedFirstNew = (uint32_t)write;
		if (isTarget(b.slices[read])) {
			if (firstRemoved == UINT32_MAX)
				firstRemoved = (uint32_t)write;
			removedInstances += b.slices[read].count;
			if (moved)
				moved->push_back(std::move(b.slices[read]));
			continue;
		}
		if (write != read) {
			b.slices[write] = std::move(b.slices[read]);
			b.sliceBounds[write] = b.sliceBounds[read];
		}
		++write;
	}

	if (write == before)
		return false;

	b.slices.resize(write);
	b.sliceBounds.resize(write);

	if (b.firstNewSlice != UINT32_MAX)
		b.firstNewSlice = (remappedFirstNew != UINT32_MAX) ? remappedFirstNew : (uint32_t)write;

	b.clustersValid = false;
	b.totalInstances -= std::min(removedInstances, b.totalInstances);

	if (b.slices.empty())
		return true;

	b.needsCompact = true;
	pendingUploads.insert(&b);
	b.coarseValid = false;
	b.rebuildFromSlice = (b.rebuildFromSlice == UINT32_MAX) ? firstRemoved : std::min(b.rebuildFromSlice, firstRemoved);
	return false;
}

void GrassBucketStore::MigrateShapeSlices(GrassBucket& from, GrassBucket& to, const ShapeIdentity& identity, std::vector<BucketKey>& emptied)
{
	static thread_local std::vector<BucketSlice> moved;
	moved.clear();
	const auto isShape = [&identity](const BucketSlice& slice) {
		return slice.shape == identity.shape && slice.generation == identity.generation;
	};
	const bool fromEmpty = ExtractSlices(from, isShape, &moved);
	for (auto& slice : moved)
		AppendSlice(to, std::move(slice));
	if (fromEmpty)
		emptied.push_back(from.key);
}

void GrassBucketStore::EraseBucket(GrassBucket& b)
{
	pendingUploads.erase(&b);
	pendingShrinks.erase(&b);
	ReleaseComplexTexture(b.diffuseTexture.get());
	meshLibrary.ReleaseMesh(b.meshId);
	if (auto keys = keysByMesh.find(b.meshId); keys != keysByMesh.end()) {
		std::erase(keys->second, b.key);
		if (keys->second.empty())
			keysByMesh.erase(keys);
	}
	buckets.erase(b.key);
}

RE::BSShaderMaterial* GrassBucketStore::CanonicalMaterial(const BucketKey& key, RE::BSShaderMaterial* material, bool pbr) const
{
	const auto keys = keysByMesh.find(key.meshId);
	if (!material || keys == keysByMesh.end())
		return material;
	for (const BucketKey& existing : keys->second) {
		if (existing.tex != key.tex || existing.triCount != key.triCount || existing.descVal != key.descVal)
			continue;
		const auto bucket = buckets.find(existing);
		if (bucket != buckets.end() && bucket->second.pbrMaterial == pbr && SameDrawMaterial(existing.material, material, pbr))
			return existing.material;
	}
	return material;
}

bool GrassBucketStore::SameDrawMaterial(RE::BSShaderMaterial* a, RE::BSShaderMaterial* b, bool pbr)
{
	if (a == b)
		return true;
	// Grass materials are lighting materials; the same vtable means the same concrete class.
	if (!a || !b || *reinterpret_cast<void**>(a) != *reinterpret_cast<void**>(b) || a->GetType() != RE::BSShaderMaterial::Type::kLighting)
		return false;

	auto* la = static_cast<RE::BSLightingShaderMaterialBase*>(a);
	auto* lb = static_cast<RE::BSLightingShaderMaterialBase*>(b);
	for (int i = 0; i < 2; ++i) {
		if (la->texCoordOffset[i] != lb->texCoordOffset[i] || la->texCoordScale[i] != lb->texCoordScale[i])
			return false;
	}
	if (la->specularColor != lb->specularColor || la->diffuseTexture != lb->diffuseTexture ||
		la->diffuseRenderTargetSourceIndex != lb->diffuseRenderTargetSourceIndex || la->normalTexture != lb->normalTexture ||
		la->rimSoftLightingTexture != lb->rimSoftLightingTexture || la->specularBackLightingTexture != lb->specularBackLightingTexture ||
		la->textureClampMode != lb->textureClampMode || la->materialAlpha != lb->materialAlpha ||
		la->refractionPower != lb->refractionPower || la->specularPower != lb->specularPower ||
		la->specularColorScale != lb->specularColorScale || la->subSurfaceLightRolloff != lb->subSurfaceLightRolloff ||
		la->rimLightPower != lb->rimLightPower)
		return false;
	if (!pbr)
		return true;

	// Compare the additional material state bound by TruePBR's grass setup.
	auto* pa = static_cast<BSLightingShaderMaterialPBR*>(a);
	auto* pb = static_cast<BSLightingShaderMaterialPBR*>(b);
	return pa->rmaosTexture == pb->rmaosTexture && pa->featuresTexture0 == pb->featuresTexture0 &&
	       pa->pbrFlags == pb->pbrFlags && pa->GetRoughnessScale() == pb->GetRoughnessScale() &&
	       pa->GetSpecularLevel() == pb->GetSpecularLevel() && pa->GetSubsurfaceColor() == pb->GetSubsurfaceColor() &&
	       pa->GetSubsurfaceOpacity() == pb->GetSubsurfaceOpacity();
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

void GrassBucketStore::CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData, size_t dataBytes)
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
	const uint32_t stride = kGrassStride;

	uint32_t count = header->groupInstanceCount;
	if (dataBytes / stride < count)
		count = (uint32_t)(dataBytes / stride);

	StageCapture(shape, instanceData, count, stride, descVal, tex);
}

bool GrassBucketStore::StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex)
{
	if (!shape || !src || !tex || !count || stride != kGrassStride) {
		logger::debug("[GRASS OPTIMIZATIONS] capture rejected: count={} stride={} desc={:016X} shape={:p}", count, stride, descVal, (void*)shape);
		return false;
	}
	auto shaderProperty = shape->GetGeometryRuntimeData().shaderProperty;
	if (!shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get())
		return false;
	auto* grassProperty = static_cast<RE::BSGrassShaderProperty*>(shaderProperty.get());
	auto* material = grassProperty->material;
	if (!material)
		return false;

	const auto& bound = shape->GetModelData().modelBound;

	PendingCapture pc;
	pc.shape = shape;
	pc.material = material;
	// TruePBR marks converted grass properties with kMenuScreen.
	pc.pbrMaterial = grassProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMenuScreen);
	pc.descVal = descVal;
	pc.diffuseTexture = RE::NiPointer<RE::NiSourceTexture>(tex);
	pc.count = count;
	pc.triangleCount = shape->GetTrishapeRuntimeData().triangleCount;
	pc.boundCenter = bound.center;
	pc.modelRadius = bound.radius;
	pc.wavePeriod = grassProperty->wavePeriod;
	pc.origin = shape->world.translate;
	pc.bytes.resize((size_t)count * kGrassStride);
	std::memcpy(pc.bytes.data(), src, pc.bytes.size());

	// Read the instance coordinates from the source allocation. Store the local min/max in the slice so the coarse bounds can be computed without re-reading the instance data.
	{
		using DirectX::PackedVector::XMConvertHalfToFloat;
		RE::NiPoint3 lmn{ FLT_MAX, FLT_MAX, FLT_MAX };
		RE::NiPoint3 lmx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
		const uint8_t* data = pc.bytes.data();
		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t* cord = data + (size_t)i * kGrassStride;
			const float lx = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 0));
			const float ly = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 2));
			const float lz = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 4));
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
	auto [identity, inserted] = liveShapes.try_emplace(shape, 0);
	if (inserted)
		identity->second = ++nextGeneration;
	pc.generation = identity->second;
	pendingCaptures.push_back(std::move(pc));
	return true;
}

void GrassBucketStore::CacheBucketTypeParams(GrassBucket& b, const PendingCapture& pc)
{
	b.indexCount = 3u * pc.triangleCount;
	if (b.typeParamsValid)
		return;

	b.wavePeriod = pc.wavePeriod;
	b.boundCenter = pc.boundCenter;
	b.modelRadius = pc.modelRadius;

	const float tris = (float)pc.triangleCount;
	const float cost = std::max(1.0f, tris / 8.0f);
	const float w = std::sqrt(cost);
	b.distScale = 1.0f / w;
	b.minPixelScale = w;
	b.typeParamsValid = true;
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

// The widest allocation is kGrassStride bytes per instance, so a larger capacity wraps its ByteWidth
// and silently under-allocates. Bounding capacityInstances here also bounds EnsureLODBin's cap.
constexpr uint32_t kMaxBucketInstances = UINT32_MAX / kGrassStride;

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

void GrassBucketStore::RefreshComplexGrass(GrassBucket& b, ID3D11DeviceContext* ctx)
{
	b.complexSRV = DetectComplexGrass(b.diffuseTexture.get(), ctx);
}

ID3D11ShaderResourceView* GrassBucketStore::DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11DeviceContext* ctx)
{
	const auto found = complexCache.find(tex);
	if (found == complexCache.end())
		return nullptr;  // The pixel shader classifies the texture instead.
	auto& entry = found->second;
	auto* rt = tex ? tex->rendererTexture : nullptr;
	auto* resourceView = rt ? rt->resourceView : nullptr;
	if (entry.normalLength && entry.resourceView == resourceView)
		return entry.normalLength->srv.get();
	if (!resourceView || !GetDetectCS() || !detectResult)
		return nullptr;  // Retry textures whose renderer resource is not ready yet.

	if (!entry.normalLength) {
		try {
			const float unknown = 0.0f;
			D3D11_SUBRESOURCE_DATA init{ &unknown, 0, 0 };
			entry.normalLength = std::make_unique<Buffer>(StructuredBufferDesc<float>(uint64_t{ 1 }, false, false), &init, "GrassOptimizations::NormalLength");
			D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
			sv.Format = DXGI_FORMAT_UNKNOWN;
			sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			sv.Buffer.NumElements = 1;
			entry.normalLength->CreateSRV(sv);
		} catch (...) {
			entry.normalLength.reset();
			return nullptr;
		}
	}
	ID3D11UnorderedAccessView* resultUAV = detectResult->uav.get();
	ctx->CSSetUnorderedAccessViews(0, 1, &resultUAV, nullptr);
	ctx->CSSetShader(detectCS, nullptr, 0);
	ctx->CSSetShaderResources(0, 1, &resourceView);
	ctx->Dispatch(1, 1, 1);
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ctx->CSSetShaderResources(0, 1, &nullSRV);
	// Copy the classification for the cull shader without allocating staging memory or waiting on the CPU.
	ctx->CopyResource(entry.normalLength->resource.get(), detectResult->resource.get());
	entry.resourceView = resourceView;
	return entry.normalLength->srv.get();
}

void GrassBucketStore::ReleaseComplexTexture(RE::NiSourceTexture* tex)
{
	if (auto it = complexCache.find(tex); it != complexCache.end()) {
		if (--it->second.activeUsers == 0)
			complexCache.erase(it);
	}
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
