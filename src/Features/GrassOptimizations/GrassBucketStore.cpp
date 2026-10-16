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
