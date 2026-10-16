#include "GrassMeshLibrary.h"

namespace
{
	RE::BSTriShape* FindFirstTriShape(RE::NiAVObject* obj)
	{
		if (!obj)
			return nullptr;
		if (auto* ts = obj->AsTriShape())
			return ts;
		if (auto* node = obj->AsNode()) {
			for (auto& child : node->GetChildren()) {
				if (auto* ts = FindFirstTriShape(child.get()))
					return ts;
			}
		}
		return nullptr;
	}

	std::string StemOf(const char* path)
	{
		const std::string p = path;
		const size_t slash = p.find_last_of("\\/");
		const size_t start = slash == std::string::npos ? 0 : slash + 1;
		const size_t dot = p.find_last_of('.');
		const size_t len = (dot == std::string::npos || dot < start) ? std::string::npos : dot - start;
		return p.substr(start, len);
	}
}

GrassMeshLibrary::~GrassMeshLibrary()
{
	loader.reset();
}

void GrassMeshLibrary::RecordModelPath(RE::BSMultiStreamInstanceTriShape* shape, const char* modelPath)
{
	if (!shape || !modelPath || !*modelPath)
		return;

	std::scoped_lock lk(stemMutex);
	const auto stem = StemOf(modelPath);
	if (auto it = stemByShape.find(shape); it == stemByShape.end() || it->second != stem)
		idByShape.erase(shape);
	stemByShape.insert_or_assign(shape, stem);
}

uint32_t GrassMeshLibrary::ResolveMeshId(RE::BSMultiStreamInstanceTriShape* shape)
{
	if (!shape)
		return 0;
	std::scoped_lock lk(stemMutex);
	if (auto it = idByShape.find(shape); it != idByShape.end())
		return it->second;

	std::string stem;
	if (auto it = stemByShape.find(shape); it != stemByShape.end())
		stem = it->second;

	uint32_t meshId = 0;
	if (!stem.empty()) {
		auto [it, inserted] = meshIdByStem.try_emplace(stem, static_cast<uint32_t>(stems.size()) + 1);
		meshId = it->second;  // ids remain stable after LOD resource eviction
		if (inserted) {
			stems.push_back(stem);
			activeBuckets.push_back(0);
		}
	}

	if (meshId)
		idByShape.emplace(shape, meshId);  // a not-yet-recorded model path may become available later
	return meshId;
}

void GrassMeshLibrary::EnsureLODMeshes(uint32_t meshId)
{
	if (meshId == 0 || meshId > stems.size())
		return;

	if (lodMeshes.size() < stems.size())
		lodMeshes.resize(stems.size());

	for (uint32_t tier = 0; tier < (uint32_t)LODTier::kCount; ++tier) {
		auto& entry = lodMeshes[meshId - 1][tier];
		if (entry.attemptedLoad || entry.queued)
			continue;

		entry.queued = true;
		if (!loader)
			loader = std::make_unique<BS::thread_pool<>>(1);

		loader->detach_task([this, meshId, stem = stems[meshId - 1], tier = (LODTier)tier] {
			LoadResult result{ meshId, tier, {} };
			LoadLODMesh(result.mesh, stem, tier);
			std::scoped_lock lock(resultMutex);
			results.push_back(std::move(result));
		});
	}
}

void GrassMeshLibrary::CommitLoadedLODMeshes()
{
	if (!loader)
		return;

	static thread_local std::vector<LoadResult> done;
	done.clear();
	{
		std::scoped_lock lk(resultMutex);
		done.swap(results);
	}

	for (auto& result : done) {
		auto& entry = lodMeshes[result.meshId - 1][(size_t)result.tier];
		if (!activeBuckets[result.meshId - 1] || !result.mesh.valid) {
			// Drop unused or rejected model trees, preserving the negative cache for failed loads.
			entry = {};
			entry.attemptedLoad = !result.mesh.valid;
			continue;
		}
		entry = std::move(result.mesh);
	}
	// Release discarded model trees on the render thread, like ReleaseMesh.
	done.clear();
}

void GrassMeshLibrary::LoadLODMesh(LODMesh& entry, const std::string& stem, LODTier tier)
{
	entry.attemptedLoad = true;

	const std::string modelPath = "LOD\\Grass\\" + stem + (tier == LODTier::kFar ? "_LOD1.nif" : "_LOD0.nif");

	RE::BSModelDB::DBTraits::ArgsType args{};
	args.unk8 = false;
	args.unkA = false;
	args.postProcess = false;
	RE::NiPointer<RE::NiNode> root;
	if (RE::BSModelDB::Demand(modelPath.c_str(), root, args) == RE::BSResource::ErrorCode::kNone && root) {
		if (auto* ts = FindFirstTriShape(root.get())) {
			auto& grd = ts->GetGeometryRuntimeData();
			auto* rd = grd.rendererData;
			if (rd && rd->vertexBuffer && rd->indexBuffer) {
				entry.keepAlive = RE::NiPointer<RE::NiAVObject>(root.get());
				entry.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(rd->vertexBuffer);
				entry.indexBuffer = reinterpret_cast<ID3D11Buffer*>(rd->indexBuffer);
				// Match the source grass instancing flags while retaining the LOD vertex format.
				entry.descVal = *reinterpret_cast<const uint64_t*>(&grd.vertexDesc) | 0x8000000000000080ull;
				entry.meshStride = VertexStrideFromDesc(entry.descVal);
				entry.indexCount = 3u * ts->GetTrishapeRuntimeData().triangleCount;
				entry.valid = entry.meshStride != 0 && entry.indexCount != 0;
				if (!entry.valid)
					logger::warn("[GRASS OPTIMIZATIONS] LOD mesh {} rejected: indices={} stride={} descVal={:016X}", modelPath, entry.indexCount, entry.meshStride, entry.descVal);
				else
					logger::info("[GRASS OPTIMIZATIONS] LOD mesh {} loaded: tris={} stride={} descVal={:016X}",
						modelPath, entry.indexCount / 3, entry.meshStride, entry.descVal);
			} else {
				logger::warn("[GRASS OPTIMIZATIONS] LOD mesh {} has no GPU buffers", modelPath);
			}
		} else {
			logger::warn("[GRASS OPTIMIZATIONS] LOD mesh {} has no TriShape", modelPath);
		}
	}
}

const GrassMeshLibrary::LODMesh* GrassMeshLibrary::GetLODMesh(uint32_t meshId, LODTier tier) const
{
	if (meshId == 0 || meshId > lodMeshes.size())
		return nullptr;

	const auto& entries = lodMeshes[meshId - 1];
	if (entries[(size_t)tier].valid)
		return &entries[(size_t)tier];

	const LODMesh& middle = entries[(size_t)LODTier::kMiddle];
	return (tier == LODTier::kFar && middle.valid) ? &middle : nullptr;
}

void GrassMeshLibrary::ForgetShape(RE::BSMultiStreamInstanceTriShape* shape)
{
	std::scoped_lock lk(stemMutex);
	idByShape.erase(shape);
	stemByShape.erase(shape);
}

void GrassMeshLibrary::RetainMesh(uint32_t meshId)
{
	if (meshId && meshId <= activeBuckets.size())
		++activeBuckets[meshId - 1];
}

void GrassMeshLibrary::ReleaseMesh(uint32_t meshId)
{
	if (!meshId || meshId > activeBuckets.size() || !activeBuckets[meshId - 1])
		return;
	if (--activeBuckets[meshId - 1] || meshId > lodMeshes.size())
		return;

	for (auto& entry : lodMeshes[meshId - 1]) {
		// Missing/invalid files stay cached; successfully loaded models may be demanded again.
		if (entry.valid)
			entry = {};
	}
}
