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
