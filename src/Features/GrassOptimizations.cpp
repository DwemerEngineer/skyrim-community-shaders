#include "GrassOptimizations.h"

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

static REL::Relocation<std::uint32_t*> GroupAttachLock{ REL::ID(524610) };

void GrassOptimizations::EnsureFadeCapacity(MergedGrassData& d, uint32_t groupCount, ID3D11Device* device)
{
	if (d.groupFadeBuffer && d.groupFadeCapacity >= groupCount)
		return;
	if (d.groupFadeBuffer) {
		d.groupFadeBuffer->Release();
		d.groupFadeBuffer = nullptr;
	}
	if (d.groupFadeSRV) {
		d.groupFadeSRV->Release();
		d.groupFadeSRV = nullptr;
	}

	const uint32_t cap = groupCount + 16;
	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = cap * sizeof(float);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = sizeof(float);
	device->CreateBuffer(&bd, nullptr, &d.groupFadeBuffer);

	D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format = DXGI_FORMAT_UNKNOWN;
	sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	sv.Buffer.NumElements = cap;
	device->CreateShaderResourceView(d.groupFadeBuffer, &sv, &d.groupFadeSRV);
	d.groupFadeCapacity = cap;
}

void GrassOptimizations::EnsureMergedCapacity(MergedGrassData& d, uint32_t totalInstances,
	uint32_t instanceStride, ID3D11Device* device)
{
	if (d.mergedInstanceVertBuf && d.mergedCapacity >= totalInstances)
		return;

	ID3D11Buffer* oldMapPtr = d.instanceGroupMapBuffer;  // capture before release
	// ... release + recreate ...
	if (d.mergedInstanceVertBuf) {
		d.mergedInstanceVertBuf->Release();
		d.mergedInstanceVertBuf = nullptr;
	}
	if (d.instanceGroupMapBuffer) {
		d.instanceGroupMapBuffer->Release();
		d.instanceGroupMapBuffer = nullptr;
	}
	if (d.instanceGroupMapSRV) {
		d.instanceGroupMapSRV->Release();
		d.instanceGroupMapSRV = nullptr;
	}

	const uint32_t cap = totalInstances + (totalInstances / 2) + 16;  // headroom to avoid churn

	D3D11_BUFFER_DESC ib{};
	ib.ByteWidth = cap * instanceStride;
	ib.Usage = D3D11_USAGE_DEFAULT;
	ib.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	HRESULT hr = device->CreateBuffer(&ib, nullptr, &d.mergedInstanceVertBuf);
	if (FAILED(hr) || !d.mergedInstanceVertBuf) {
		logger::error("[GRASS OPTIMIZATIONS] CreateBuffer FAILED hr={:08X} bytes={}", (unsigned)hr, ib.ByteWidth);
		d.totalInstances = 0;
		d.needsRebuild = true;
		return;
	}

	D3D11_BUFFER_DESC gb{};
	gb.ByteWidth = cap * sizeof(uint32_t);
	gb.Usage = D3D11_USAGE_DEFAULT;
	gb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	gb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	gb.StructureByteStride = sizeof(uint32_t);
	hr = device->CreateBuffer(&gb, nullptr, &d.instanceGroupMapBuffer);
	if (FAILED(hr) || !d.instanceGroupMapBuffer) {
		logger::error("[GRASS OPTIMIZATIONS] Map CreateBuffer FAILED hr={:08X}", (unsigned)hr);
		d.mergedCapacity = 0;
		d.totalInstances = 0;
		d.needsRebuild = true;
		return;
	}

	logger::debug("ENSURE relMap={:p} -> newMap={:p} cap={}",
		(void*)oldMapPtr, (void*)d.instanceGroupMapBuffer, cap);

	D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format = DXGI_FORMAT_UNKNOWN;
	sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	sv.Buffer.NumElements = cap;
	hr = device->CreateShaderResourceView(d.instanceGroupMapBuffer, &sv, &d.instanceGroupMapSRV);
	if (FAILED(hr) || !d.instanceGroupMapSRV) {
		logger::error("[GRASS OPTIMIZATIONS] Map SRV FAILED hr={:08X}", (unsigned)hr);
		d.mergedCapacity = 0;
		d.totalInstances = 0;
		d.needsRebuild = true;
		return;
	}

	d.mergedCapacity = cap;
}

void GrassOptimizations::BuildMergedBuffers(RE::BSMultiStreamInstanceTriShape* shape, MergedGrassData& d,
	uint32_t instanceStride, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	auto& groups = shape->GetMultiStreamTrishapeRuntimeData().unk160;

	static thread_local std::vector<uint32_t> groupMap;
	groupMap.clear();

	struct Span
	{
		ID3D11Buffer* src;
		uint32_t slot;
		uint32_t count;
	};
	static thread_local std::vector<Span> spans;
	spans.clear();

	uint32_t total = 0;
	for (uint32_t i = 0; i < groups.size(); ++i) {
		auto* g = groups[i];
		if (!g || !g->unk50)
			continue;
		uint32_t count = g->instanceCount;
		if (!count)
			continue;
		auto* src = *reinterpret_cast<ID3D11Buffer**>(g->buffer);
		if (!src)
			continue;
		spans.push_back({ src, i, count });
		total += count;
	}

	uint32_t passed = 0;
	for (uint32_t i = 0; i < groups.size(); ++i)
		if (auto* g = groups[i]; g && g->unk50 && g->instanceCount)
			passed++;

	EnsureMergedCapacity(d, total, instanceStride, device);
	if (!total) {
		d.totalInstances = 0;
		d.needsRebuild = false;
		return;
	}

	groupMap.reserve(total);
	uint32_t instOffset = 0;
	D3D11_BUFFER_DESC ib{};
	d.mergedInstanceVertBuf->GetDesc(&ib);
	for (auto& s : spans) {
		
		D3D11_BOX box{ 0, 0, 0, s.count * instanceStride, 1, 1 };
		ctx->CopySubresourceRegion(d.mergedInstanceVertBuf, 0,
			instOffset * instanceStride, 0, 0, s.src, 0, &box);
		for (uint32_t k = 0; k < s.count; ++k)
			groupMap.push_back(s.slot);
		instOffset += s.count;
	}
	
	logger::debug("preMapUpload buf={:p} writeBytes={}",
		(void*)d.instanceGroupMapBuffer, (UINT)(groupMap.size() * sizeof(uint32_t)));

	ctx->UpdateSubresource(d.instanceGroupMapBuffer, 0, nullptr, groupMap.data(), (UINT)(groupMap.size() * sizeof(uint32_t)), 0);
	d.totalInstances = instOffset;
	d.needsRebuild = false;
}

void GrassOptimizations::DrainPending()
{
	std::vector<RE::BSMultiStreamInstanceTriShape*> remove, dirty;
	{
		std::scoped_lock lk(pendingMutex);
		remove.swap(pendingRemove);
		dirty.swap(pendingDirty);
	}

	std::scoped_lock lk(mergedMutex);

	for (auto* s : remove) {
		auto it = mergedData.find(s);
		if (it != mergedData.end()) {
			mergedData.erase(it);
		}
	}
	for (auto* s : dirty) {
		if (std::find(remove.begin(), remove.end(), s) != remove.end())
			continue;
		mergedData[s].needsRebuild = true;
	}
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_dtor::thunk(RE::BSMultiStreamInstanceTriShape* This)
{
	{
		std::scoped_lock lk(globals::features::grassOptimizations.pendingMutex);
		globals::features::grassOptimizations.pendingRemove.push_back(This);
	}

	func(This);
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_DoneAddingInstances::thunk(RE::BSMultiStreamInstanceTriShape* This, RE::BSTArray<std::uint32_t>& a_instances)
{
	func(This, a_instances);

	if (auto shaderProp = This->shaderProperty) {
		if (auto rtti = shaderProp->GetRTTI()->GetName()) {
			const std::string rttiStr(rtti);
			if (rttiStr == "BSGrassShaderProperty") {
				auto& merged = globals::features::grassOptimizations.GetOrCreateMergedData(This);
				merged.needsRebuild = true;
			}
		}
	}
}

std::uint32_t GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_AddGroup::thunk(RE::BSMultiStreamInstanceTriShape* This, std::uint32_t a_numInstances, std::uint16_t* a_instanceData, std::uint32_t numShorts, float a_arg4)
{
	auto result = func(This, a_numInstances, a_instanceData, numShorts, a_arg4);

	if (auto shaderProp = This->shaderProperty) {
		if (auto rtti = shaderProp->GetRTTI()->GetName()) {
			const std::string rttiStr(rtti);
			if (rttiStr == "BSGrassShaderProperty") {
				auto& merged = globals::features::grassOptimizations.GetOrCreateMergedData(This);
				merged.needsRebuild = true;
			}
		}
	}

	return result;
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_RemoveGroup::thunk(RE::BSMultiStreamInstanceTriShape* This, std::uint32_t a_numInstances)
{
	func(This, a_numInstances);

	if (auto shaderProp = This->shaderProperty) {
		if (auto rtti = shaderProp->GetRTTI()->GetName()) {
			const std::string rttiStr(rtti);
			if (rttiStr == "BSGrassShaderProperty") {
				auto& merged = globals::features::grassOptimizations.GetOrCreateMergedData(This);
				merged.needsRebuild = true;
			}
		}
	}
}

void GrassOptimizations::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags)
{
	auto& self = globals::features::grassOptimizations;
	uint32_t frame = globals::game::graphicsState->frameCount;

	auto shaderProp = reinterpret_cast<RE::BSGrassShaderProperty*>(a2->shaderProperty);


	if (self.lastSetupFrame != frame) {
		self.DrainPending();
		self.lastSetupFrame = frame;
		self.rebuildsThisFrame = 0;
	}

	std::pair<uint32_t, float> key{ frame, shaderProp->wavePeriod };
	if (key == self.lastCB2Key)
		return;
	func(This, a2, flags);
	self.lastCB2Key = key;
}

/*
void GrassOptimizations::Hooks::BSGrassShaderProperty_dtor::thunk(RE::BSGrassShaderProperty* This)
{

uint32_t frame = globals::game::graphicsState->frameCount;

	{
		std::lock_guard lock(globals::features::grassOptimizations.mergedMutex);
		globals::features::grassOptimizations.fadeData.erase(This);
	}

	func(This);
}
*/

std::uint32_t GrassOptimizations::Hooks::AddQueuedGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4)
{
	auto result = func(a1, a2, a3, a4);
	auto& merged = globals::features::grassOptimizations.GetOrCreateMergedData(a1);
	merged.needsRebuild = true;

	return result;
}

std::uint32_t GrassOptimizations::Hooks::AddQueuedGroupDone::thunk(RE::BSMultiStreamInstanceTriShape* a1, uint32_t a_instanceCount, void* groupAlloc, uint32_t a4, RE::BSTArray<std::uint32_t>& groupIndicies, float a6)
{
	auto result = func(a1, a_instanceCount, groupAlloc, a4, groupIndicies, a6);

	auto& merged = globals::features::grassOptimizations.GetOrCreateMergedData(a1);
	merged.needsRebuild = true;

	return result;
}

std::uint32_t GrassOptimizations::Hooks::AddGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3)
{
	auto result = func(a1, a2, a3);

	auto& merged = globals::features::grassOptimizations.GetOrCreateMergedData(a1);
	merged.needsRebuild = true;

	return result;
}

thread_local RE::BSMultiStreamInstanceTriShape::GroupHeader tl_lastFileGroupHeader{};
thread_local std::vector<uint16_t> tl_lastFileInstanceData;

void GrassOptimizations::Hooks::ReadGroupHeaderStreamTraits::thunk(RE::BSStreamHeader* streamHeader, RE::BSMultiStreamInstanceTriShape::GroupHeader* groupHeader, uint32_t size)
{
	func(streamHeader, groupHeader, size);

	std::memcpy(&tl_lastFileGroupHeader, groupHeader, size);
}

void GrassOptimizations::Hooks::ReadInstanceGroupStreamTraits::thunk(RE::BSStreamHeader* streamHeader, uint16_t* instanceData, uint32_t size)
{
	func(streamHeader, instanceData, size);

	std::memcpy(&tl_lastFileInstanceData, instanceData, size);
}

uint32_t GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	auto result = func(a1, a2);
	auto& merged = globals::features::grassOptimizations.GetOrCreateMergedData(a1);
	merged.needsRebuild = true;

	return result;
}

void VanillaDrawInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto* ctx = globals::d3d::context;

	for (uint32_t i = 0; i < geometry->unk160.size(); ++i) {
		if (geometry->unk160.size()) {
			auto curInstanceGroup = geometry->unk160[i];

			if (curInstanceGroup && curInstanceGroup->unk50) {
				uint32_t indexCount = 0;
				uint32_t* indexCountPtr = &indexCount;
				static REL::Relocation<ID3D11Buffer** (*)(RE::BSGraphics::Renderer * a1, uint64_t a2, uint32_t** a3, uint32_t BufferIndex)> MapDynamicBuffer{ REL::RelocationID(75561, 0) };
				auto buffer = MapDynamicBuffer(globals::game::renderer, 1, &indexCountPtr, 7);
				*indexCountPtr = i;
				if (*buffer)
					ctx->Unmap(*buffer, 0);
				ctx->VSSetConstantBuffers(7u, 1u, buffer);

				static REL::Relocation<void (*)(RE::BSGraphics::Renderer* a1, RE::BSGraphics::TriShape* a2, uint32_t StartIndexLocation, uint32_t triCount, uint32_t instanceCount, RE::BSGraphics::VertexDesc desc, ID3D11Buffer** instanceGroupVertBuffer)> DrawInstancedTriShape{ REL::RelocationID(75479, 0) };
				DrawInstancedTriShape(globals::game::renderer, geometry->rendererData, 0, geometry->triangleCount, curInstanceGroup->instanceCount, geometry->vertexDesc, reinterpret_cast<ID3D11Buffer**>(curInstanceGroup->buffer));
			}
		}
	}
}

void GrassOptimizations::Hooks::DrawInstanceTriShape::thunk(RE::BSMultiStreamInstanceTriShape* geometry)
{
	if (auto rtti = geometry->shaderProperty->GetRTTI()->GetName()) {
		const std::string rttiStr(rtti);
		if (rttiStr != "BSGrassShaderProperty") {
			VanillaDrawInstanceTriShape(geometry);
			return;
		}
	}

	auto* device = globals::d3d::device;
	auto* ctx = globals::d3d::context;
	auto& self = globals::features::grassOptimizations;

	auto desc = geometry->vertexDesc;
	auto descPtr = reinterpret_cast<uint64_t*>(&desc);
	const uint32_t instanceStride = (uint32_t)((*descPtr >> 2) & 0x3C);
	const uint32_t meshStride = (uint32_t)((4 * *descPtr) & 0x3C);

	MergedGrassData* d = nullptr;
	{
		std::scoped_lock lk(self.mergedMutex);
		d = &self.mergedData[geometry];
	}

	uint32_t curGen = geometry->GetMultiStreamTrishapeRuntimeData().unk198;
	if (d->builtGroupGen != curGen) {
		d->builtGroupGen = curGen;
		d->rebuildFramesLeft = 8;
	}
	if (d->rebuildFramesLeft > 0) {
		self.BuildMergedBuffers(geometry, *d, instanceStride, device, ctx);
		d->rebuildFramesLeft--;
	}

	if (!d->totalInstances || !d->mergedInstanceVertBuf)
		return;

	auto grassProp = static_cast<RE::BSGrassShaderProperty*>(geometry->shaderProperty.get());
	auto& groups = geometry->GetMultiStreamTrishapeRuntimeData().unk160;
	const uint32_t groupCount = (uint32_t)groups.size();

	self.EnsureFadeCapacity(*d, groupCount, device);

	static thread_local std::vector<float> fades;
	fades.clear();
	fades.resize(groupCount, 1.0f);
	auto& fadeArr = grassProp->groupFadeAlphas;
	const uint32_t n = std::min(groupCount, fadeArr.size());
	for (uint32_t i = 0; i < n; ++i)
		fades[i] = fadeArr[i];

	ctx->UpdateSubresource(d->groupFadeBuffer, 0, nullptr, fades.data(), (UINT)(fades.size() * sizeof(float)), 0);

	if (globals::game::shadowState->vertexDesc != *descPtr) {
		globals::game::shadowState->vertexDesc = *descPtr;
		globals::game::shadowState->stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	}
	if (globals::game::shadowState->topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		globals::game::shadowState->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		globals::game::shadowState->stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}

	static REL::Relocation<void (*)(uint32_t)> SetDirtyStates{ REL::RelocationID(75580, 77386) };
	SetDirtyStates(0);

	ctx->IASetIndexBuffer(reinterpret_cast<ID3D11Buffer*>(geometry->rendererData->indexBuffer),
		DXGI_FORMAT_R16_UINT, 0);

	ID3D11Buffer* buffers[2] = {
		reinterpret_cast<ID3D11Buffer*>(geometry->rendererData->vertexBuffer),
		d->mergedInstanceVertBuf
	};
	UINT strides[2] = { meshStride, instanceStride };
	UINT offsets[2] = { 0, 0 };
	ctx->IASetVertexBuffers(0, 2, buffers, strides, offsets);

	ctx->VSSetShaderResources(2, 1, &d->groupFadeSRV);
	ctx->VSSetShaderResources(3, 1, &d->instanceGroupMapSRV);

	ctx->DrawIndexedInstanced(3 * geometry->triangleCount, d->totalInstances, 0, 0, 0);
}

void GrassOptimizations::Hooks::ProcessAttachQueue::thunk(RE::BGSGrassManager* a1)
{
	static thread_local std::vector<RE::BSMultiStreamInstanceTriShape*> touched;
	touched.clear();

	static REL::Relocation<RE::BSTArray<RE::BSMultiStreamInstanceTriShape::GroupAttachTask>*>
		queue{ REL::RelocationID(524607, 411247) };
	for (uint32_t i = 0; i < queue->size(); ++i)
		if (auto* s = (*queue)[i].trishape)
			touched.push_back(s);

	func(a1); 

	auto& self = globals::features::grassOptimizations;
	std::scoped_lock lk(self.pendingMutex);
	for (auto* s : touched)
		self.pendingDirty.push_back(s);
}
