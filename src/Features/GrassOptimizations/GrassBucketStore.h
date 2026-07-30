#pragma once

#include "Buffer.h"
#include "GrassMeshLibrary.h"

struct BucketKey
{
	uint32_t meshId = 0;
	RE::NiSourceTexture* tex = nullptr;  // Used to key the bucket when meshId == 0
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
	// Origin-relative extent decoded at capture; world AABB = origin + [localMin, localMax].
	RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
};

// Optimized 32-byte boxes for the cull compute shader to read.
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
	RE::NiSourceTexture* diffuseTexture = nullptr;
	std::vector<uint8_t> bytes;
	uint32_t count = 0;
	uint64_t descVal = 0;
	RE::NiPoint3 origin;
	RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
};

// Offset of the indirect args block. Uses an offset of 12 so the instance count lands on byte 16, as required for the raw UAV to have 16 byte alignment.
// Since instance count is the second uint32_t in the args, leading padding is needed so the instance count is at offset 16 as required by the raw UAV.
inline constexpr uint32_t argsByteOffset = 12;
inline constexpr uint32_t instanceCountOffset = argsByteOffset + sizeof(uint32_t);  // 16

/** @brief Contains the instance data, GPU buffers and per-frame cull results for each grass type. */
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
	ID3D11Buffer* argsBuf = nullptr;
	// Windows onto args[1] alone, so the cull CS adds survivors straight into the indirect args.
	ID3D11UnorderedAccessView* argsUAV = nullptr;

	// Second bin for LOD instances, allocated only when an LOD .nif loaded. When null all instances are drawn in the main bin.
	ID3D11Buffer* lodCompactedBuf = nullptr;
	ID3D11UnorderedAccessView* lodCompactedUAV = nullptr;
	ID3D11Buffer* lodExtrasBuf = nullptr;
	ID3D11UnorderedAccessView* lodExtrasUAV = nullptr;
	ID3D11ShaderResourceView* lodExtrasSRV = nullptr;
	ID3D11Buffer* lodArgsBuf = nullptr;
	ID3D11UnorderedAccessView* lodArgsUAV = nullptr;
	bool lodArgsIndexCountWritten = false;
	uint32_t lodCapacityInstances = 0;
	bool lodActive = false;

	// Source mesh id, for easy lookup of the LOD mesh.
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

	// Adjacent same-cell slices merged so each run is one slice-table entry.
	struct alignas(16) SliceRun
	{
		SliceBounds bounds{};
		uint32_t firstSliceOffset = 0;
		uint32_t instanceCount = 0;
		uint32_t pad[2]{};
	};
	STATIC_ASSERT_ALIGNAS_16(SliceRun);

	std::vector<SliceRun> sliceRuns;
	bool clustersValid = false;
	bool dirty = false;
	// Removals leaves gaps, so compact on the gpu to avoid a full rebuild from the CPU.
	bool needsCompact = false;
	uint32_t firstNewSlice = UINT32_MAX;
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

	// This frame's window into the shared slice table and the instances the dispatch must cover.
	uint32_t sliceTableOffset = 0;
	uint32_t sliceTableCount = 0;
	uint32_t visibleInstances = 0;

	/** @brief Releases the GPU buffers and views, while keeping the instance data and slices. */
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

/** @brief Container for the grass instance data, staged captures, the buckets they fold into, and their GPU buffers. */
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

	/** @brief Applies staged removals and captures, then uploads dirty buckets. */
	void ApplyPending(ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Re-runs complex-grass detection for every bucket when the threshold changes. Marks buckets dirty, so must be called before ApplyPending. */
	void RefreshComplexGrass(float threshold, ID3D11DeviceContext* ctx);

	/** @brief Captures one GID group's instance records from the cell-load hooks. */
	void CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData, size_t dataBytes);

	/** @brief Stages a raw instance-record capture. Returns false when the stride is not the expected 32 bytes, defaulting to vanilla rendering. */
	bool StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex);

	/** @brief Stages a dead shape for removal on the next grass frame. */
	void StageRemoval(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Marks a bucket's represtative shape for this frame as having been queued for setup. Returns true when there is no instanced bucket or first of its bucket this frame. */
	bool ClaimQueueSlot(RE::BSMultiStreamInstanceTriShape* shape, uint32_t frame);

	/** @brief Drops staged captures and removals without applying them. */
	void DiscardPending();

	/** @brief Recomputes a bucket's padded union AABB over all of its slices. */
	void UpdateCoarseBounds(GrassBucket& b);

	/** @brief Allocates/frees a bucket's LOD compaction bin. Returns true if the bin is successfully allocated. */
	bool EnsureLODBin(GrassBucket& b, ID3D11Device* device);

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

	/** @brief Closes the gaps left by removals with device-side copies, reassigning slice offsets.
	    Returns false when the survivors cannot be shifted and a full rebuild is needed instead. */
	bool CompactBucket(GrassBucket& b, ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Re-assembles and re-uploads a bucket's instance data from the first stale slice. */
	void RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Uploads only the newly appended slices to a bucket's existing buffers. */
	void AppendNewSlices(GrassBucket& bucket, ID3D11DeviceContext* ctx);

	/** @brief Grows a bucket's buffers, preserving the first preserveInstances with a GPU copy. */
	bool EnsureBucketCapacity(GrassBucket& b, uint32_t neededInstances, ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t preserveInstances);

	/** @brief Creates a bucket's instance and origin source buffers plus their SRVs. */
	bool CreateBucketSourceBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device);

	/** @brief Creates a bucket's per-frame cull scratch: compacted and extras buffers. */
	bool CreateBucketCullScratch(GrassBucket& b, uint32_t capacity, ID3D11Device* device);

	/** @brief Creates a bucket's UAV-writable indirect args buffer. */
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

	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, GrassBucket*> shapeBucketId;
	mutable std::shared_mutex shapeBucketMutex;

	std::unordered_map<RE::NiSourceTexture*, bool> complexCache;
	float cachedComplexThreshold = -1.0f;

	ID3D11ComputeShader* detectCS = nullptr;
	std::unique_ptr<ConstantBuffer> detectParamsCB;
	std::unique_ptr<Buffer> detectResult;
	std::unique_ptr<Buffer> detectStaging;
};
