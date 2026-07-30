#pragma once

/** @brief Maps grass shapes to their source .nif and caches the optional LOD mesh. Assigns integer IDs to each unique source .nif to avoid string lookups. */
class GrassMeshLibrary
{
public:
	/** @brief An LOD mesh, loaded from meshes\LOD\Grass\<source-mesh-stem>_LOD.nif. When `valid` is false, the full mesh is drawn then. */
	struct LODMesh
	{
		RE::NiPointer<RE::NiAVObject> keepAlive;  // owns the loaded model tree
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		uint32_t indexCount = 0;
		uint32_t meshStride = 0;
		uint64_t descVal = 0;
		bool attemptedLoad = false;
		bool valid = false;
	};

	/** @brief Records a shape's source model path, from the LoadGrassType hook. */
	void RecordModelPath(RE::BSMultiStreamInstanceTriShape* shape, const char* modelPath);

	/** @brief Resolves a shape to a source-mesh id (0 = unresolved) and caches the result. */
	uint32_t ResolveMeshId(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Loads the LOD .nif for this mesh id. */
	void EnsureLODMesh(uint32_t meshId);

	/** @brief Returns the cached LOD mesh, or nullptr when none is loaded or it is unusable. */
	const LODMesh* GetLODMesh(uint32_t meshId) const;

	/** @brief Removes no longer valid shapes from the lookup caches to prevent IDs and LOD meshes from being associated with the wrong shape. */
	void ForgetShape(RE::BSMultiStreamInstanceTriShape* shape);
		
private:
	// Resolves shapes by id to their source-mesh stems, with stems[id - 1] since zero is invalid.
	std::vector<std::string> stems;
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, uint32_t> idByShape;

	// Prevent race conditions between the resolving of meshIDs on the game thread and the loading on the game thread.
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, std::string> stemByShape;
	mutable std::mutex stemMutex;

	// Parallel to stems, indexed by meshId - 1. A deque to keep GetLODMesh's pointers valid after growth.
	std::deque<LODMesh> lodMeshes;
};
