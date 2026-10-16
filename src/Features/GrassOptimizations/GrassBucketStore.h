#pragma once

#include "GrassBucket.h"

/** @brief Container for the grass instance data, staged captures, the buckets they fold into, and their GPU buffers. */
class GrassBucketStore
{
public:
	/** @brief Feature settings the store needs, refreshed once per grass frame. */
	struct FrameParams
	{
		bool enableMeshLOD = false;
		bool enableMidLOD = false;
		bool enableFarLOD = false;
		float fadeStart = 0.0f;  // stamped on newly captured slices as their fade-in start
	};

	/** @brief Creates the complex-grass detection buffers. */
	void SetupResources();

	/** @brief Releases the cached detection shader so it recompiles on next use. */
	void ClearShaderCache();

	void BeginFrame(const FrameParams& params) { frameParams = params; }

	/** @brief Applies staged removals and captures, then uploads dirty buckets. Caller holds bucketMutex. */
	void ApplyPending(ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Process queued destruction even without grass draws; the caller must hold bucketMutex. */
	void ProcessRemovals();

	/** @brief Captures one GID group's instance records from the cell-load hooks. */
	void CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData, size_t dataBytes);

	/** @brief Stages a raw instance-record capture. Returns false when the stride is not the expected 32 bytes, defaulting to vanilla rendering. */
	bool StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex);

	/** @brief Stages a dead shape for removal on the next grass frame. */
	void StageRemoval(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Marks a bucket's representative shape for this frame as having been queued for setup. Returns true when there is no instanced bucket or first of its bucket this frame. */
	bool ClaimQueueSlot(RE::BSMultiStreamInstanceTriShape* shape, uint32_t frame);

	/** @brief Find a shape's committed bucket, which remains valid while the caller holds bucketMutex. */
	GrassBucket* FindBucket(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Recomputes a bucket's padded union AABB over all of its slices. */
	void UpdateCoarseBounds(GrassBucket& b);

	/** @brief Extend cached culling runs with uploaded slices, rebuilding all runs after removals. */
	void MergeSlicesIntoRuns(GrassBucket& b);

	/** @brief Upload new cull groups and return false if buffer allocation fails. */
	bool UploadCullGroups(GrassBucket& b, ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Allocates/frees one of a bucket's LOD compaction bins. Returns true if the bin is successfully allocated. */
	bool EnsureLODBin(GrassBucket& b, GrassMeshLibrary::LODTier tier, ID3D11Device* device);

	/** @brief Refresh texture classification for a visible bucket as its texture becomes resident. */
	void RefreshComplexGrass(GrassBucket& b, ID3D11DeviceContext* ctx);

	std::unordered_map<BucketKey, GrassBucket, BucketKeyHash> buckets;
	// Limit material matching to buckets with the same source mesh.
	std::unordered_map<uint32_t, std::vector<BucketKey>> keysByMesh;
	// Hold this mutex throughout bucket traversal and pending updates.
	// UpdateGrass holds it while calling helpers, so those helpers must not lock it again.
	std::mutex bucketMutex;

private:
	struct ShapeIdentity
	{
		RE::BSMultiStreamInstanceTriShape* shape = nullptr;
		uint64_t generation = 0;
		bool operator==(const ShapeIdentity&) const = default;
		bool operator<(const ShapeIdentity& other) const
		{
			return shape == other.shape ? generation < other.generation : std::less<RE::BSMultiStreamInstanceTriShape*>{}(shape, other.shape);
		}
	};
	/** @brief Removes dead shapes' slices from their buckets and drops them from the lookup maps. */
	void ApplyRemovals(const std::vector<ShapeIdentity>& removes);

	/** @brief Fold captures into buckets and collect the keys of buckets emptied by migration. */
	void ApplyCaptures(std::vector<PendingCapture>& captures, std::vector<BucketKey>& emptied);

	/** @brief Reuse an existing material when it produces the same grass draw. */
	RE::BSShaderMaterial* CanonicalMaterial(const BucketKey& key, RE::BSShaderMaterial* material, bool pbr) const;

	/** @brief Compare the material state bound by the grass shader, ignoring bookkeeping such as texture set pointers. */
	static bool SameDrawMaterial(RE::BSShaderMaterial* a, RE::BSShaderMaterial* b, bool pbr);

	/** @brief Append a slice to a bucket's pending tail and extend its coarse bounds. */
	void AppendSlice(GrassBucket& b, BucketSlice&& slice);

	/** @brief Extract matching slices into `moved` when provided and return true if the bucket becomes empty. */
	template <class Pred>
	bool ExtractSlices(GrassBucket& b, Pred isTarget, std::vector<BucketSlice>* moved);

	/** @brief Move a shape's earlier slices into the bucket identified by its latest capture. */
	void MigrateShapeSlices(GrassBucket& from, GrassBucket& to, const ShapeIdentity& identity, std::vector<BucketKey>& emptied);

	/** @brief Release an empty bucket whose shape entries have already been retired. */
	void EraseBucket(GrassBucket& b);

	/** @brief Rebuilds or appends to the GPU buffers of buckets whose slices changed. */
	void UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Closes the gaps left by removals with device-side copies, reassigning slice offsets.
	    Returns false when the survivors cannot be shifted and a full rebuild is needed instead. */
	bool CompactBucket(GrassBucket& b, ID3D11DeviceContext* ctx);

	/** @brief Re-assembles and re-uploads a bucket's instance data from the first stale slice. */
	void RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Uploads only the newly appended slices to a bucket's existing buffers. */
	void AppendNewSlices(GrassBucket& bucket, ID3D11DeviceContext* ctx);
	void UploadSliceTail(GrassBucket& bucket, uint32_t from, uint32_t prefix, ID3D11DeviceContext* ctx);

	/** @brief Grows a bucket's buffers, preserving the first preserveInstances with a GPU copy. */
	bool EnsureBucketCapacity(GrassBucket& b, uint32_t neededInstances, ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t preserveInstances);

	/** @brief Replace capacity-dependent buffers transactionally while preserving a contiguous source prefix. */
	bool ResizeBucketBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t preserveInstances);

	/** @brief Drop a texture classification when its last bucket stops using it. */
	void ReleaseComplexTexture(RE::NiSourceTexture* tex);

	/** @brief Create a bucket's instance source buffer and its SRV. */
	bool CreateBucketSourceBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device);

	/** @brief Creates a bucket's per-frame cull scratch: compacted and extras buffers. */
	bool CreateBucketCullScratch(GrassBucket& b, uint32_t capacity, ID3D11Device* device);

	/** @brief Create a bucket's full-detail indirect arguments and shared middle/far LOD counter. */
	bool CreateBucketCounters(GrassBucket& b, ID3D11Device* device);

	/** @brief Cache per-type wave, bounding-sphere and mesh-cost parameters from a capture. */
	void CacheBucketTypeParams(GrassBucket& b, const PendingCapture& pc);

	/** @brief Samples a grass diffuse to decide whether it uses the complex-grass layout. */
	ID3D11ShaderResourceView* DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11DeviceContext* ctx);

	/** @brief Returns the complex-grass detection compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetDetectCS();

	FrameParams frameParams;
	// Only changed buckets need upload work; only underutilized buckets need shrink checks.
	// These pointers remain stable across unordered_map rehashes and are removed before erasure.
	std::unordered_set<GrassBucket*> pendingUploads;
	std::unordered_set<GrassBucket*> pendingShrinks;

	std::vector<PendingCapture> pendingCaptures;
	std::vector<ShapeIdentity> pendingRemoves;
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, uint64_t> liveShapes;
	uint64_t nextGeneration = 0;  // Assign a new identity whenever an allocator reuses a shape address.
	std::mutex pendingMutex;

	struct ShapeBuckets
	{
		uint64_t generation = 0;
		// Move earlier slices when a later capture resolves to a different bucket.
		GrassBucket* current = nullptr;
	};
	// Read by culling jobs, so this guards the pointed-to buckets' lifetime as well as the map itself.
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, ShapeBuckets> shapeBucketId;
	mutable std::shared_mutex shapeBucketMutex;

	// Buckets own texture references, so cache entries remain valid until their last bucket is removed.
	struct ComplexEntry
	{
		uint32_t activeUsers = 0;
		ID3D11ShaderResourceView* resourceView = nullptr;
		std::unique_ptr<Buffer> normalLength;
	};
	std::unordered_map<RE::NiSourceTexture*, ComplexEntry> complexCache;

	ID3D11ComputeShader* detectCS = nullptr;
	bool detectCSFailed = false;  // Latches a failed compile until ClearShaderCache(), like cullCSFailed.
	std::unique_ptr<Buffer> detectResult;
	std::unique_ptr<Buffer> compactionScratch;

public:
	// Destroy the mesh library before the destruction-hook queues and locks that its callbacks can access.
	GrassMeshLibrary meshLibrary;
};
