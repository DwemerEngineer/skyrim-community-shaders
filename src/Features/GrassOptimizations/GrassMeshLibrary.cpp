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

	/** @brief Returns the filename without directories or extension. */
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

void GrassMeshLibrary::RecordModelPath(RE::BSMultiStreamInstanceTriShape* shape, const char* modelPath)
{
	if (!shape || !modelPath || !*modelPath)
		return;

	std::scoped_lock lk(stemMutex);
	stemByShape.insert_or_assign(shape, StemOf(modelPath));
}

uint32_t GrassMeshLibrary::ResolveMeshId(RE::BSMultiStreamInstanceTriShape* shape)
{
	if (!shape)
		return 0;
	if (auto it = idByShape.find(shape); it != idByShape.end())
		return it->second;

	// The LoadGrassType hook records this shape's source .nif at grass-type creation, before any
	// instance of it is captured, so the entry exists by the time we get here.
	std::string stem;
	{
		std::scoped_lock lk(stemMutex);
		if (auto it = stemByShape.find(shape); it != stemByShape.end())
			stem = it->second;
	}

	uint32_t meshId = 0;
	if (!stem.empty()) {
		if (auto it = idByStem.find(stem); it != idByStem.end()) {
			meshId = it->second;
		} else {
			stems.push_back(stem);
			meshId = static_cast<uint32_t>(stems.size());  // ids are 1-based
			idByStem.emplace(stem, meshId);
		}
	}

	idByShape.emplace(shape, meshId);
	return meshId;
}

std::string GrassMeshLibrary::GetStem(uint32_t meshId) const
{
	return (meshId && meshId <= stems.size()) ? stems[meshId - 1] : std::string("?");
}

void GrassMeshLibrary::EnsureLODMesh(uint32_t meshId)
{
	if (meshId == 0 || meshId > stems.size() || lodMeshes.contains(meshId))
		return;

	LODMesh entry;  // valid == false: no LOD mesh → the full mesh is drawn

	// Convention: meshes\LOD\Grass\<source-mesh-stem>.nif, authored in the same local space and
	// vertex format as the source grass (BSModelDB::Demand prepends "meshes\").
	const std::string modelPath = "LOD\\Grass\\" + stems[meshId - 1] + ".nif";

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
				entry.descVal = *reinterpret_cast<const uint64_t*>(&grd.vertexDesc);
				entry.meshStride = (uint32_t)((4 * entry.descVal) & 0x3C);
				entry.indexCount = 3u * ts->GetTrishapeRuntimeData().triangleCount;
				entry.valid = entry.meshStride != 0;
				logger::info("[GRASS OPTIMIZATIONS] LOD mesh {} loaded: tris={} descVal={:016X}",
					modelPath, entry.indexCount / 3, entry.descVal);
			} else {
				logger::warn("[GRASS OPTIMIZATIONS] LOD mesh {} has no GPU buffers", modelPath);
			}
		} else {
			logger::warn("[GRASS OPTIMIZATIONS] LOD mesh {} has no TriShape", modelPath);
		}
	}
	// File absent → error != kNone → silent; that grass type keeps its full mesh.

	lodMeshes.emplace(meshId, std::move(entry));
}

const GrassMeshLibrary::LODMesh* GrassMeshLibrary::GetLODMesh(uint32_t meshId) const
{
	auto it = lodMeshes.find(meshId);
	return (it != lodMeshes.end() && it->second.valid) ? &it->second : nullptr;
}

void GrassMeshLibrary::ForgetShape(RE::BSMultiStreamInstanceTriShape* shape)
{
	idByShape.erase(shape);
	std::scoped_lock lk(stemMutex);
	stemByShape.erase(shape);
}
