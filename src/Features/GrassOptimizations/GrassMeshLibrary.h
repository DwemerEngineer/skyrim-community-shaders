#pragma once

#include <BS_thread_pool.hpp>

/** @brief Byte stride of the per-instance records. */
constexpr uint32_t kGrassStride = 32;

/** @brief Mesh vertex stride in bytes, from nibble 0 of a packed vertexDesc (stored as bytes/4). */
constexpr uint32_t VertexStrideFromDesc(uint64_t descVal) { return static_cast<uint32_t>((4 * descVal) & 0x3C); }

/** @brief Maps grass shapes to their source .nif and caches the optional LOD meshes. Assigns integer IDs to each unique source .nif to avoid string lookups. */
class GrassMeshLibrary
{
public:
	~GrassMeshLibrary();

	/** @brief Which of the two mesh-swap distance bands an LOD mesh serves. */
	enum class LODTier : uint32_t
	{
		kMiddle = 0,
		kFar = 1,
		kCount = 2
	};

	/** @brief An LOD mesh, loaded from meshes\LOD\Grass\<source-mesh-stem>_LOD0.nif (middle) or _LOD1.nif (far). The far tier falls back to the middle mesh, then to the full mesh. */
	struct LODMesh
	{
		RE::NiPointer<RE::NiAVObject> keepAlive;  // owns the loaded model tree
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		uint32_t indexCount = 0;
		float boundExtent = 0.0f;
		// The LOD .nif is authored separately, so its vertex format need not match the source mesh's.
		uint32_t meshStride = 0;
		uint64_t descVal = 0;
		bool attemptedLoad = false;
		bool queued = false;
		bool valid = false;
	};

	/** @brief Records a shape's source model path, from the LoadGrassType hook. */
	void RecordModelPath(RE::BSMultiStreamInstanceTriShape* shape, const char* modelPath);

	/** @brief Resolves a shape to a source-mesh id (0 = unresolved) and caches the result. */
	uint32_t ResolveMeshId(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Queues both LOD .nifs for this mesh id on the background loader. */
	void EnsureLODMeshes(uint32_t meshId);
	/** @brief Publishes LOD meshes the background loader finished, on the render thread. */
	void CommitLoadedLODMeshes();

	/** @brief Returns the cached LOD mesh for a tier, or nullptr when none is loaded or it is unusable. The far tier falls back to the middle mesh. */
	const LODMesh* GetLODMesh(uint32_t meshId, LODTier tier) const;

	/** @brief Removes no longer valid shapes from the lookup caches to prevent IDs and LOD meshes from being associated with the wrong shape. */
	void ForgetShape(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Registers a bucket's use of a stable mesh ID. */
	void RetainMesh(uint32_t meshId);
	/** @brief Drops loaded LOD model trees when their last bucket is removed. */
	void ReleaseMesh(uint32_t meshId);

private:
	static bool FindLODShape(RE::NiAVObject* object, RE::BSTriShape*& shape);

	/** @brief Loads one tier's .nif into an entry. Runs on the loader thread. */
	static void LoadLODMesh(LODMesh& entry, const std::string& stem, LODTier tier);

	// Resolves shapes by id to their source-mesh stems, with stems[id - 1] since zero is invalid.
	std::vector<std::string> stems;
	std::unordered_map<std::string, uint32_t> meshIdByStem;
	std::vector<uint32_t> activeBuckets;
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, uint32_t> idByShape;

	// Model paths and per-shape IDs share stemMutex; destruction forgets them before address reuse.
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, std::string> stemByShape;
	mutable std::mutex stemMutex;

	// Parallel to stems, indexed by meshId - 1 then by LODTier. A deque to keep GetLODMesh's pointers valid after growth.
	std::deque<std::array<LODMesh, (size_t)LODTier::kCount>> lodMeshes;

	struct LoadResult
	{
		uint32_t meshId;
		LODTier tier;
		LODMesh mesh;
	};
	std::mutex resultMutex;
	std::vector<LoadResult> results;
	std::unique_ptr<BS::thread_pool<>> loader;
};
