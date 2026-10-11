#pragma once

#include "Buffer.h"
#include "GrassMeshLibrary.h"

struct BucketKey
{
	// Mesh ids come from the model's file name, so the triangle count keeps same-named models in other folders apart.
	uint32_t meshId = 0;
	// An optimized draw shares the representative shape's material state.
	RE::BSShaderMaterial* material = nullptr;
	// Unresolved meshes also require texture and vertex format identity.
	RE::NiSourceTexture* tex = nullptr;
	uint32_t triCount = 0;
	uint64_t descVal = 0;
	bool operator==(const BucketKey&) const = default;
};

struct BucketKeyHash
{
	size_t operator()(const BucketKey& k) const
	{
		return (std::hash<uint32_t>{}(k.meshId) * 31) ^
		       std::hash<void*>{}(k.material) ^
		       std::hash<void*>{}(k.tex) ^
		       (std::hash<uint32_t>{}(k.triCount) * 131) ^
		       (std::hash<uint64_t>{}(k.descVal) << 1);
	}
};

/** @brief One captured group of grass instances: raw half-packed records plus placement data. */
struct BucketSlice
{
	RE::BSMultiStreamInstanceTriShape* shape = nullptr;
	uint64_t generation = 0;
	std::vector<uint8_t> data;  // raw 32-byte half-packed instance records
	uint32_t count = 0;
	float fadeStart = 0.0f;
	RE::NiPoint3 origin;
	uint32_t bufferOffset = UINT32_MAX;
	// Origin-relative extent decoded at capture; world AABB = origin + [localMin, localMax].
	RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
};

// Use world-space slice bounds to build cached CPU culling runs.
struct SliceBounds
{
	alignas(16) float lo[4]{};
	alignas(16) float hi[4]{};
};
static_assert(sizeof(SliceBounds) == 32);

/** @brief A capture queued by the cell-load hooks, applied to a bucket on the next grass frame. */
struct PendingCapture
{
	RE::BSMultiStreamInstanceTriShape* shape = nullptr;
	uint64_t generation = 0;
	RE::BSShaderMaterial* material = nullptr;
	bool pbrMaterial = false;
	RE::NiPointer<RE::NiSourceTexture> diffuseTexture;
	std::vector<uint8_t> bytes;
	uint32_t count = 0;
	uint32_t triangleCount = 0;
	uint64_t descVal = 0;
	RE::NiPoint3 origin;
	RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint3 boundCenter;
	float modelRadius = 0.0f;
	float wavePeriod = 1.0f;
};

// Pad indirect args so their second DWORD, the instance count, starts at a 16-byte-aligned UAV window.
inline constexpr uint32_t argsByteOffset = 12;
inline constexpr uint32_t instanceCountOffset = argsByteOffset + sizeof(uint32_t);  // 16
static_assert(instanceCountOffset % 16 == 0);

inline constexpr uint32_t kCullGroupSize = 64;

/** @brief Match the HLSL CullGroup layout for one slice's contiguous source instances. */
struct CullGroup
{
	float origin[3];
	float fadeStart;
	uint32_t sourceStart;
	uint32_t count;
};
static_assert(sizeof(CullGroup) == 24);

/** @brief Contains the instance data, GPU buffers and per-frame cull results for each grass type. */
struct GrassBucket
{
	BucketKey key;  // Erase this bucket without scanning the map.
	ID3D11Buffer* instanceBuf = nullptr;
	ID3D11ShaderResourceView* instanceSRV = nullptr;
	// Upload cached cull groups only when the runs change.
	ID3D11Buffer* groupBuf = nullptr;
	ID3D11ShaderResourceView* groupSRV = nullptr;

	ID3D11Buffer* compactedBuf = nullptr;
	ID3D11UnorderedAccessView* compactedUAV = nullptr;
	ID3D11Buffer* extrasBuf = nullptr;
	ID3D11UnorderedAccessView* extrasUAV = nullptr;
	ID3D11ShaderResourceView* extrasSRV = nullptr;
	// Holds the middle and far LOD counts in one UAV; copied into their indirect args after culling.
	ID3D11Buffer* lodCounterBuf = nullptr;
	ID3D11UnorderedAccessView* lodCounterUAV = nullptr;
	// Give each bucket its own indirect arguments so the driver can overlap cull dispatches that write to different resources.
	ID3D11Buffer* argsBuf = nullptr;
	ID3D11UnorderedAccessView* argsUAV = nullptr;  // Expose only the instance count to the cull shader.
	uint32_t argsIndexCount = 0;                   // Cache the index count currently stored in argsBuf.

	/** @brief A compaction bin and indirect draw for one LOD tier, allocated only when that tier's mesh loaded. */
	struct LODBin
	{
		ID3D11Buffer* compactedBuf = nullptr;
		ID3D11UnorderedAccessView* compactedUAV = nullptr;
		ID3D11Buffer* extrasBuf = nullptr;
		ID3D11UnorderedAccessView* extrasUAV = nullptr;
		ID3D11ShaderResourceView* extrasSRV = nullptr;
		ID3D11Buffer* argsBuf = nullptr;
		uint32_t argsIndexCount = 0;
		uint32_t capacityInstances = 0;
		bool active = false;

		/** @brief Releases this bin's buffers and views. */
		void Release()
		{
			auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
			rel(compactedUAV);
			rel(compactedBuf);
			rel(extrasSRV);
			rel(extrasUAV);
			rel(extrasBuf);
			rel(argsBuf);
			capacityInstances = 0;
			argsIndexCount = 0;
			active = false;
		}
	};

	// Indexed by GrassMeshLibrary::LODTier. Instances land in the main bin unless a tier claims them.
	std::array<LODBin, (size_t)GrassMeshLibrary::LODTier::kCount> lodBins;

	// Source mesh id, for easy lookup of the LOD mesh.
	uint32_t meshId = 0;
	// Retain the key's material for comparisons with later captures.
	RE::BSTSmartPointer<RE::BSShaderMaterial> material;
	bool pbrMaterial = false;
	// BucketKey only carries the texture for unresolved meshes, so keep it here for re-detection. Owned,
	// since re-detection dereferences it long after the shader property that supplied it may have gone.
	RE::NiPointer<RE::NiSourceTexture> diffuseTexture;

	uint32_t cullSlot = UINT32_MAX;
	bool typeParamsValid = false;
	float wavePeriod = 1.0f;
	RE::NiPoint3 boundCenter{};
	float modelRadius = 128.0f;
	float distScale = 1.0f;
	float minPixelScale = 1.0f;
	ID3D11ShaderResourceView* complexSRV = nullptr;  // The active texture cache owns this view.
	std::atomic<bool> optimizedReady{ false };
	uint32_t indexCount = 0;  // Copy this mesh's index count into argsBuf when it changes.

	uint32_t capacityInstances = 0;  // Source and cull buffers share this capacity.
	uint32_t totalInstances = 0;     // This count includes the pending upload tail.
	uint32_t residentInstances = 0;  // This count covers the contiguous uploaded or compacted prefix.
	uint32_t underutilizedFrames = 0;
	std::vector<BucketSlice> slices;
	std::vector<SliceBounds> sliceBounds;  // parallel to slices; same size, same order

	// Each SSE lane tests one of four adjacent runs.
	// Cache bounds with modelRadius + 64 padding until the slices change.
	struct alignas(16) SliceRunBatch
	{
		float loX[4]{}, loY[4]{}, loZ[4]{};
		float hiX[4]{}, hiY[4]{}, hiZ[4]{};
		uint32_t firstGroup[4]{};  // These windows preserve the run order in cullGroups.
		uint32_t groupCount[4]{};
	};
	static_assert(sizeof(SliceRunBatch) == 128);
	STATIC_ASSERT_ALIGNAS_16(SliceRunBatch);

	std::vector<SliceRunBatch> sliceRunBatches;
	uint32_t sliceRunCount = 0;  // Exclude unused lanes in the final batch.
	// One parent bounds a contiguous block of cell runs; four parents share an SSE batch.
	static constexpr uint32_t kRunsPerCullBlock = 16;
	static_assert(kRunsPerCullBlock % 4 == 0);
	std::vector<SliceRunBatch> sliceBlockBatches;
	bool sliceBlocksUseful = false;  // Scattered run order can make parent bounds too broad.
	// Keep thread groups in their original run order.
	std::vector<CullGroup> cullGroups;
	uint32_t groupCapacity = 0;
	uint32_t residentGroups = 0;   // These leading groups are already resident in groupBuf.
	uint32_t clusteredSlices = 0;  // Cached runs cover [0, clusteredSlices); additions extend only the tail.
	bool clustersValid = false;
	bool dirty = false;
	// Compact removal gaps on the GPU instead of uploading the surviving instances again.
	bool needsCompact = false;
	uint32_t firstNewSlice = UINT32_MAX;  // UINT32_MAX means there is no pending upload tail.
	// The lowest stale slice that the rebuild will start from. When set to UINT32_MAX and dirty is true, all slices are rebuilt.
	uint32_t rebuildFromSlice = UINT32_MAX;

	// Atomic to make sure only one culling job queues the bucket per frame, ensuring setup is only done once per type per frame.
	std::atomic<uint32_t> lastQueuedFrame{ UINT32_MAX };

	uint32_t drawnFrame = UINT32_MAX;
	// Identifies the pass by the passEnum + pixel-shader descriptor to prevent buckets from being drawn more than once per frame.
	uint64_t drawnPassKey = UINT64_MAX;

	RE::NiPoint3 coarseMin{};
	RE::NiPoint3 coarseMax{};
	bool coarseValid = false;
	bool cullVisible = false;

	// Select this bucket's visible slice ranges and dispatch groups from the shared table.
	uint32_t sliceTableOffset = 0;
	uint32_t sliceTableCount = 0;
	uint32_t groupCount = 0;

	/** @brief Drops this frame's cull verdict, so a skipped or failed cull cannot be reissued from stale args. */
	void ResetCullState()
	{
		cullSlot = UINT32_MAX;
		cullVisible = false;
		sliceTableCount = 0;
		groupCount = 0;
	}

	GrassBucket() = default;
	// Owns raw COM pointers released in the destructor, so a copy would double-release them.
	GrassBucket(const GrassBucket&) = delete;
	GrassBucket& operator=(const GrassBucket&) = delete;

	/** @brief Releases the GPU buffers and views, while keeping the instance data and slices. */
	void ReleaseResources()
	{
		auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		rel(instanceBuf);
		rel(instanceSRV);
		rel(groupBuf);
		rel(groupSRV);
		rel(compactedBuf);
		rel(compactedUAV);
		rel(extrasBuf);
		rel(extrasUAV);
		rel(extrasSRV);
		rel(lodCounterUAV);
		rel(lodCounterBuf);
		rel(argsUAV);
		rel(argsBuf);
		argsIndexCount = 0;
		for (LODBin& bin : lodBins)
			bin.Release();
		capacityInstances = 0;
		residentInstances = 0;
		groupCapacity = 0;
		residentGroups = 0;
	}

	/** @brief Releases everything including instance data and slices. */
	void Release()
	{
		ReleaseResources();
		totalInstances = 0;
		slices.clear();
		sliceBounds.clear();
		sliceRunBatches.clear();
		sliceRunCount = 0;
		sliceBlockBatches.clear();
		sliceBlocksUseful = false;
		cullGroups.clear();
		clusteredSlices = 0;
		clustersValid = false;
		coarseValid = false;
		firstNewSlice = UINT32_MAX;
		rebuildFromSlice = UINT32_MAX;
		underutilizedFrames = 0;
	}

	~GrassBucket() { Release(); }
};

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
