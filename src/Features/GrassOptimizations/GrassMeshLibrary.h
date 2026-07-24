#pragma once

/** @brief Maps grass shapes to their source .nif and caches the optional lower-poly LOD mesh.
    Bucket keys and the LOD cache work on interned integer ids rather than strings, so the per-draw
    bucket lookup stays an integer hash. */
class GrassMeshLibrary
{
public:
	/** @brief A lower-poly LOD mesh, loaded from meshes\LOD\Grass\<source-mesh-stem>.nif.
	    `valid` is false when no LOD mesh exists — the full mesh is drawn then. */
	struct LODMesh
	{
		RE::NiPointer<RE::NiAVObject> keepAlive;  // owns the loaded model tree
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		uint32_t indexCount = 0;
		uint32_t meshStride = 0;
		uint64_t descVal = 0;
		bool valid = false;
	};

	/**
	 * @brief Records a shape's source model path, from the LoadGrassType hook.
	 *
	 * Runs on the grass loader thread, so it takes its own lock rather than the feature's bucket
	 * mutex, which UpdateGrass holds across its GPU work.
	 */
	void RecordModelPath(RE::BSMultiStreamInstanceTriShape* shape, const char* modelPath);

	/** @brief Resolves a shape to an interned source-mesh id (0 = unresolved), caching the result. */
	uint32_t ResolveMeshId(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Returns the interned .nif filename stem for a mesh id, or "?" when unknown. */
	std::string GetStem(uint32_t meshId) const;

	/** @brief Loads (once per source mesh) the lower-poly LOD .nif for this mesh id. */
	void EnsureLODMesh(uint32_t meshId);

	/** @brief Returns the cached LOD mesh, or nullptr when none is loaded or it is unusable. */
	const LODMesh* GetLODMesh(uint32_t meshId) const;

	/**
	 * @brief Drops a dead shape from the lookup caches.
	 *
	 * Both are keyed by shape pointer and must not outlive the shape: the allocator reuses
	 * addresses, and a future grass shape would inherit this one's mesh id and LOD mesh.
	 */
	void ForgetShape(RE::BSMultiStreamInstanceTriShape* shape);

private:
	// Interned source-mesh stems: id 1..N (0 = unresolved), so stems[id - 1].
	std::unordered_map<std::string, uint32_t> idByStem;
	std::vector<std::string> stems;
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, uint32_t> idByShape;

	// Written by the loader thread, read while resolving; hence its own mutex.
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, std::string> stemByShape;
	mutable std::mutex stemMutex;

	std::unordered_map<uint32_t, LODMesh> lodMeshes;
};
