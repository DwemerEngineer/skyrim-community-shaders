#pragma once

#include "Buffer.h"
#include "GrassMeshLibrary.h"

// Keyed by SOURCE MESH (interned .nif filename stem) so one bucket == one mesh. Required for the
// mesh-swap LOD, and it also stops two grass types that merely share a diffuse texture + vertex
// format from landing in one bucket and being drawn with each other's mesh.
// meshId == 0 means the mesh could not be resolved — fall back to the texture identity so that
// grass still gets the instancing optimization.
struct BucketKey
{
	uint32_t meshId = 0;
	RE::NiSourceTexture* tex = nullptr;  // part of the identity only when meshId == 0
	uint64_t descVal = 0;
	bool operator==(const BucketKey&) const = default;
};

struct BucketKeyHash
{
	size_t operator()(const BucketKey& k) const
	{
		return (std::hash<uint32_t>{}(k.meshId) * 31) ^
		       std::hash<void*>{}(k.tex) ^
		       (std::hash<uint64_t>{}(k.descVal) << 1);
	}
};

/** @brief One captured group of grass instances: raw half-packed records plus placement data. */
struct BucketSlice
{
	RE::BSMultiStreamInstanceTriShape* shape = nullptr;
	std::vector<uint8_t> data;  // raw 32-byte half-packed instance records
	uint32_t count = 0;
	float fadeStart = 0.0f;
	RE::NiPoint3 origin;
	uint32_t bufferOffset = UINT32_MAX;
	// Instance-local position extent (origin-relative), decoded from the half-packed records
	// at capture. World AABB of this slice = origin + [localMin, localMax].
	RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
};

// Slice bounds, held parallel to `slices` rather than inside BucketSlice: the cull loop needs only
// the box, and BucketSlice is ~128 bytes because it owns the instance data. 32 bytes exactly, so
// two pack per cache line.
struct SliceBounds
{
	alignas(16) float lo[4]{};
	alignas(16) float hi[4]{};
};
static_assert(sizeof(SliceBounds) == 32);

/** @brief A capture staged by the loader hooks, applied to a bucket on the next grass frame. */
struct PendingCapture
{
	RE::BSMultiStreamInstanceTriShape* shape = nullptr;
	RE::NiSourceTexture* diffuseTexture = nullptr;
	std::vector<uint8_t> bytes;
	uint32_t count = 0;
	uint64_t descVal = 0;
	RE::NiPoint3 origin;
	RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
};

// Byte offset of the DrawIndexedInstancedIndirect args block inside its buffer. The 5-uint block is
// deliberately NOT at 0: placing it at 12 puts instanceCount (block + 4) on byte 16, and a D3D11 raw
// UAV must start 16-byte aligned — FirstElement a multiple of 4. Windowing onto instanceCount at
// byte 4 is illegal, which is why an earlier FirstElement=1 attempt was rejected by the runtime and
// silently fell back to the counter + copy path.
inline constexpr uint32_t kArgsByteOffset = 12;
inline constexpr uint32_t kArgsInstanceCountOffset = kArgsByteOffset + sizeof(uint32_t);  // 16

/** @brief All state for one grass type: instance data, GPU buffers and per-frame cull results.
    The D3D resources are raw pointers by design — buckets are created and destroyed constantly as
    cells load, which the feature-lifetime wrapper types in Buffer.h are not meant for. */
struct GrassBucket
{
	ID3D11Buffer* instanceBuf = nullptr;
	ID3D11ShaderResourceView* instanceSRV = nullptr;
	ID3D11Buffer* originBuf = nullptr;
	ID3D11ShaderResourceView* originSRV = nullptr;

	ID3D11Buffer* compactedBuf = nullptr;
	ID3D11UnorderedAccessView* compactedUAV = nullptr;
	ID3D11Buffer* extrasBuf = nullptr;
	ID3D11UnorderedAccessView* extrasUAV = nullptr;
	ID3D11ShaderResourceView* extrasSRV = nullptr;
	ID3D11Buffer* counterBuf = nullptr;
	ID3D11UnorderedAccessView* counterUAV = nullptr;
	ID3D11Buffer* argsBuf = nullptr;
	// View over args[1] (instanceCount) only, so the cull CS can InterlockedAdd the survivor
	// count straight into the indirect args. Null => runtime rejected a UAV-capable args
	// buffer, fall back to the counter + CopySubresourceRegion path.
	ID3D11UnorderedAccessView* argsUAV = nullptr;

	// Second compaction bin, for the mesh-swap LOD. Allocated only for buckets whose
	// LOD\Grass\<stem>.nif actually loaded, so grass types without an authored LOD cost
	// nothing. While these are null the cull CS routes every survivor to the full-detail bin.
	ID3D11Buffer* lodCompactedBuf = nullptr;
	ID3D11UnorderedAccessView* lodCompactedUAV = nullptr;
	ID3D11Buffer* lodExtrasBuf = nullptr;
	ID3D11UnorderedAccessView* lodExtrasUAV = nullptr;
	ID3D11ShaderResourceView* lodExtrasSRV = nullptr;
	ID3D11Buffer* lodCounterBuf = nullptr;
	ID3D11UnorderedAccessView* lodCounterUAV = nullptr;
	ID3D11Buffer* lodArgsBuf = nullptr;
	ID3D11UnorderedAccessView* lodArgsUAV = nullptr;
	bool lodArgsIndexCountWritten = false;
	uint32_t lodCapacityInstances = 0;
	// Set per frame: LOD bin allocated and the setting is on -> the CS may use bin 1.
	bool lodActive = false;

	// Source mesh id, kept so the draw path finds this bucket's LOD mesh without re-resolving.
	uint32_t meshId = 0;

	uint32_t cullSlot = UINT32_MAX;
	bool typeParamsValid = false;
	float wavePeriod = 1.0f;
	RE::NiPoint3 boundCenter{};
	float clumpRadius = 128.0f;
	float distScale = 1.0f;
	float minPixelScale = 1.0f;
	bool isComplex = false;
	bool argsIndexCountWritten = false;

	uint32_t capacityInstances = 0;
	uint32_t totalInstances = 0;
	std::vector<BucketSlice> slices;
	std::vector<SliceBounds> sliceBounds;  // parallel to slices; same size, same order

	// Cull granularity: maximal runs of slices that share a cell AND occupy a contiguous range
	// of the instance buffer. Slices are ~1254 units wide in a 4096-unit cell and ~105 pile
	// into each one, overlapping heavily, so a whole cell passes or fails the frustum together.
	// One run emits one slice-table entry, which also shortens the CS binary search.
	struct SliceRun
	{
		alignas(16) float lo[4]{};
		alignas(16) float hi[4]{};
		uint32_t firstOffset = 0;  // bufferOffset of the run's first slice
		uint32_t instanceCount = 0;
		uint32_t pad[2]{};
	};
	static_assert(sizeof(SliceRun) == 48);
	std::vector<SliceRun> sliceRuns;
	bool clustersValid = false;  // gates sliceRuns rebuild
	bool dirty = false;
	uint32_t firstNewSlice = UINT32_MAX;
	// Lowest slice index whose buffer contents are stale. Slices below it keep their offsets,
	// so a rebuild only re-uploads from here. UINT32_MAX with dirty set means rebuild all.
	uint32_t rebuildFromSlice = UINT32_MAX;

	// Last frame this bucket was queued. Atomic rather than a set behind a mutex: OnVisible
	// runs for every grass shape across several culling threads, and a shared lock there
	// serialises the game's parallel culling. The common path is a relaxed load that matches
	// and returns; only the first shape of a bucket does the CAS.
	std::atomic<uint32_t> lastQueuedFrame{ UINT32_MAX };

	uint32_t drawnFrame = UINT32_MAX;
	// Identifies the PASS, not the geometry. A BSRenderPass* would not do: the engine
	// allocates one per geometry, and a bucket holds one slice per cell, so every shape
	// arrived with a different pointer and re-issued the whole bucket's draw. passEnum +
	// pixel-shader descriptor is stable across the shapes of one pass and distinct between
	// passes.
	uint64_t drawnPassKey = UINT64_MAX;

	RE::NiPoint3 coarseMin{};
	RE::NiPoint3 coarseMax{};
	bool coarseValid = false;
	bool cullVisible = false;

	// This frame's visible-slice window into the shared slice table, and the instance count
	// the dispatch actually needs to cover.
	uint32_t sliceTableOffset = 0;
	uint32_t sliceTableCount = 0;
	uint32_t visibleInstances = 0;

	/** @brief Releases the GPU buffers and views; instance data and slices are kept. */
	void ReleaseResources()
	{
		auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		rel(instanceBuf);
		rel(instanceSRV);
		rel(originBuf);
		rel(originSRV);
		rel(compactedBuf);
		rel(compactedUAV);
		rel(extrasBuf);
		rel(extrasUAV);
		rel(extrasSRV);
		rel(counterBuf);
		rel(counterUAV);
		rel(argsUAV);
		rel(argsBuf);
		ReleaseLODBin();
		capacityInstances = 0;
		argsIndexCountWritten = false;
	}

	/** @brief Releases the LOD compaction bin's buffers and views. */
	void ReleaseLODBin()
	{
		auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		rel(lodCompactedUAV);
		rel(lodCompactedBuf);
		rel(lodExtrasSRV);
		rel(lodExtrasUAV);
		rel(lodExtrasBuf);
		rel(lodCounterUAV);
		rel(lodCounterBuf);
		rel(lodArgsUAV);
		rel(lodArgsBuf);
		lodCapacityInstances = 0;
		lodArgsIndexCountWritten = false;
		lodActive = false;
	}

	/** @brief Releases everything including instance data and slices. */
	void Release()
	{
		ReleaseResources();
		totalInstances = 0;
		slices.clear();
		sliceBounds.clear();
		sliceRuns.clear();
		clustersValid = false;
	}

	~GrassBucket() { Release(); }
};

/** @brief Owns the grass instance data: captures staged by the loader hooks, the buckets they fold
    into, and those buckets' GPU buffers. The feature drives culling and drawing on top of it. */
class GrassBucketStore
{
public:
	/** @brief Feature settings the store needs, refreshed once per grass frame. */
	struct FrameParams
	{
		bool enableMeshLOD = false;
		float fadeStart = 0.0f;  // stamped on newly captured slices as their fade-in start
	};

	/** @brief Creates the complex-grass detection buffers. */
	void SetupResources();

	/** @brief Releases the cached detection shader so it recompiles on next use. */
	void ClearShaderCache();

	void BeginFrame(const FrameParams& params) { frameParams = params; }

	/** @brief Applies staged removals and captures, then uploads every bucket whose slices changed.
	    Caller must hold bucketMutex. */
	void ApplyPending(ID3D11Device* device, ID3D11DeviceContext* ctx);

	/**
	 * @brief Re-runs complex-grass detection for every bucket when the threshold changes.
	 *
	 * Caller must hold bucketMutex. Marks affected buckets dirty, so call before ApplyPending.
	 */
	void RefreshComplexGrass(float threshold, ID3D11DeviceContext* ctx);

	/** @brief Captures one GID group's instance records from the loader hooks. */
	void CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData);

	/** @brief Stages a raw instance-record capture for the next grass frame. Returns false and
	    leaves vanilla rendering untouched when the record layout is not the expected 32 bytes. */
	bool StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex);

	/** @brief Stages a dead shape for removal on the next grass frame. */
	void StageRemoval(RE::BSMultiStreamInstanceTriShape* shape);

	/**
	 * @brief Claims this frame's single queue slot for the bucket owning `shape`.
	 *
	 * Returns true when the caller should queue the shape: either it has no instanced bucket (so
	 * vanilla per-shape drawing still needs it) or it is the first of its bucket this frame.
	 * Runs on the game's culling threads, and holds shapeBucketMutex across the whole claim — the
	 * bucket it points at can be erased by ApplyRemovals on the render thread.
	 */
	bool ClaimQueueSlot(RE::BSMultiStreamInstanceTriShape* shape, uint32_t frame);

	/** @brief Drops staged captures and removals without applying them. */
	void DiscardPending();

	/** @brief Recomputes a bucket's padded union AABB over all of its slices. */
	void UpdateCoarseBounds(GrassBucket& b);

	/** @brief Allocates/frees a bucket's second (LOD) compaction bin to match the current setting
	    and the bucket's capacity. Returns true when the bucket has a usable LOD bin this frame. */
	bool EnsureLODBin(GrassBucket& b, ID3D11Device* device);

	// Mesh identity is bucket identity, so the library lives here; the LoadGrassType and draw hooks
	// reach it through the store.
	GrassMeshLibrary meshLibrary;

	std::unordered_map<BucketKey, GrassBucket, BucketKeyHash> buckets;
	std::mutex bucketMutex;

private:
	/** @brief Removes dead shapes' slices from their buckets and drops them from the lookup maps. */
	void ApplyRemovals(const std::vector<RE::BSMultiStreamInstanceTriShape*>& removes);

	/** @brief Folds staged captures into buckets, creating buckets and slices as needed. */
	void ApplyCaptures(std::vector<PendingCapture>& captures);

	/** @brief Rebuilds or appends to the GPU buffers of buckets whose slices changed. */
	void UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Re-assembles and re-uploads a bucket's instance data from the first stale slice. */
	void RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Uploads only the newly appended slices to a bucket's existing buffers. */
	void AppendNewSlices(GrassBucket& bucket, ID3D11DeviceContext* ctx);

	/** @brief Grows a bucket's buffers, preserving the first preserveInstances instances with a
	    device-side copy so the caller does not have to re-upload data that has not changed. */
	bool EnsureBucketCapacity(GrassBucket& b, uint32_t neededInstances, ID3D11Device* device,
		ID3D11DeviceContext* ctx, uint32_t preserveInstances);

	/** @brief Creates a bucket's instance and origin source buffers plus their SRVs. */
	bool CreateBucketSourceBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device);

	/** @brief Creates a bucket's per-frame cull scratch: compacted, extras and counter buffers. */
	bool CreateBucketCullScratch(GrassBucket& b, uint32_t capacity, ID3D11Device* device);

	/** @brief Creates a bucket's indirect args buffer, preferring a UAV-writable one. */
	bool CreateBucketArgsBuffer(GrassBucket& b, ID3D11Device* device);

	/** @brief Caches per-type parameters (wave period, bound, mesh cost) from a source shape. */
	void CacheBucketTypeParams(GrassBucket& b, RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Samples a grass diffuse to decide whether it uses the complex-grass layout. */
	bool DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11DeviceContext* ctx);

	/** @brief Returns the complex-grass detection compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetDetectCS();

	FrameParams frameParams;

	std::vector<PendingCapture> pendingCaptures;
	std::vector<RE::BSMultiStreamInstanceTriShape*> pendingRemoves;
	std::mutex pendingMutex;

	// shape -> owning bucket. A bucket holds every cell's instances and draws identically
	// whichever shape triggered it, so only one shape per bucket needs queueing; the rest would
	// cost a full BSGrassShader::SetupGeometry for a draw the pass-key dedup discards.
	//
	// Maintained incrementally by ApplyCaptures / ApplyRemovals — rebuilding it wholesale would
	// either cost every frame or spike on the frames where cells load. GrassBucket* survives
	// rehash (unordered_map is node-based); erasing a bucket invalidates one, and that path
	// clears the map immediately under the lock.
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, GrassBucket*> shapeBucketId;
	mutable std::shared_mutex shapeBucketMutex;

	std::unordered_map<RE::NiSourceTexture*, bool> complexCache;
	float cachedComplexThreshold = -1.0f;

	ID3D11ComputeShader* detectCS = nullptr;
	std::unique_ptr<ConstantBuffer> detectParamsCB;
	std::unique_ptr<Buffer> detectResult;
	std::unique_ptr<Buffer> detectStaging;
};
