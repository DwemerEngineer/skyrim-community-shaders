#pragma once

#include "Buffer.h"
#include "Upscaling.h"
struct GrassOptimizations : Feature
{
public:
	virtual inline std::string GetName() override { return "Grass Optimizations"; }
	virtual std::string GetDisplayName() override { return T("feature.grass_optimizations.name", "Grass Optimizations"); }
	virtual inline std::string GetShortName() override { return "GrassOptimizations"; }
	virtual inline std::string_view GetShaderDefineName() override { return "GRASS_OPTIMIZATIONS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kGrass; }

	/** @brief Returns true only for Grass shader type. */
	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.grass_optimizations.description", "Improves grass rendering performance."), { T("feature.grass_optimizations.key_feature_1", "Improves vanilla grass instancing improving performance") }
		};
	};

	struct Settings
	{
		// Projected-size LOD, lowest level: clumps whose on-screen radius is below this many pixels
		// are culled entirely.
		float MinPixelSize = 4.0f;
		// Projected-size LOD, full-detail level: clumps larger than this (px) render at full
		// density; between this and MinPixelSize they are stochastically thinned toward MinDensity.
		float FullDetailPixelSize = 16.0f;
		float MinDensity = 0.03f;
		// Blends in a per-mesh cost weighting (sqrt(triangles/6)) that culls heavier grass meshes
		// sooner. 0 = the pixel/distance settings are literal and identical for every grass type;
		// 1 = full weighting (the old hidden behaviour).
		float MeshCostBias = 0.0f;
		// Cull instances whose fade is at/below this before they are ever drawn. The alpha test
		// discards every fragment of such instances anyway (fade * baseAlpha < ref), so raising
		// this toward the game's alpha-test ref removes invisible-but-rasterized grass. 0 = only
		// provably-invisible (zero-fade) instances.
		float InvisibleFadeCull = 0.0f;
		// Max grass render distance override, in units. 0 = use the vanilla INI
		// (fGrassStartFadeDistance + fGrassFadeRange).
		float MaxDistanceOverride = 0.0f;

		// Discard instances hidden behind already-drawn geometry, tested against a 1/16-res
		// max-depth reduction of the scene depth copy. Early-Z already kills their fragments, but
		// only after the vertex shader has run; this removes the vertex work too.
		bool EnableOcclusionCulling = true;

		// Mesh-swap LOD: clumps whose on-screen radius is below MeshLODPixelSize (but still above
		// the MinPixelSize cull) are drawn with a lower-poly LOD .nif instead of the full mesh.
		bool EnableMeshLOD = false;
		float MeshLODPixelSize = 8.0f;
		// Width in pixels of the dithered swap band centred on MeshLODPixelSize. Instances inside
		// it are hash-assigned to full/LOD so the transition scatters across a clump rather than
		// every instance flipping at once. 0 = hard swap.
		float MeshLODBandPixels = 3.0f;
	};

	Settings settings;

	// Keyed by SOURCE MESH (interned .nif filename stem) so one bucket == one mesh. Required for the
	// mesh-swap LOD, and it also stops two grass types that merely share a diffuse texture + vertex
	// format from landing in one bucket and being drawn with each other's mesh.
	// meshId == 0 means the mesh could not be resolved — fall back to the old texture identity so
	// that grass still gets the instancing optimization.
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

	struct BucketSlice
	{
		RE::BSMultiStreamInstanceTriShape* shape;
		std::vector<uint8_t> data;  // REVERTED to raw 32B half records
		uint32_t count;
		float fadeStart;
		RE::NiPoint3 origin;
		uint32_t bufferOffset = UINT32_MAX;
		// Actual instance-local position extent (origin-relative), decoded from the half-packed
		// records at capture. World AABB of this slice = origin + [localMin, localMax].
		RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
		RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
	};

	struct PendingCapture
	{
		RE::BSMultiStreamInstanceTriShape* shape = nullptr;
		RE::NiSourceTexture* diffuseTexture = nullptr;
		std::vector<uint8_t> bytes;  // REVERTED
		uint32_t count = 0;
		uint64_t descVal = 0;
		RE::NiPoint3 origin;
		RE::NiPoint3 localMin{ 0.0f, 0.0f, 0.0f };
		RE::NiPoint3 localMax{ 0.0f, 0.0f, 0.0f };
	};

	struct VisibleRun
	{
		uint32_t base;
		uint32_t count;
		uint32_t cbFirstConst = UINT32_MAX;
		float sortKeySq = 0.0f;
		RE::NiPoint3 origin;
		uint32_t drawCount;
	};

	// Byte offset of the DrawIndexedInstancedIndirect args block inside its buffer. The 5-uint block
	// is deliberately NOT at 0: placing it at 12 puts instanceCount (block + 4) on byte 16, and a
	// D3D11 raw UAV must start 16-byte aligned — FirstElement a multiple of 4. Windowing onto
	// instanceCount at byte 4 is illegal, which is why the earlier FirstElement=1 attempt was
	// rejected by the runtime and silently fell back to the counter + copy path.
	static constexpr uint32_t kArgsByteOffset = 12;
	static constexpr uint32_t kArgsInstanceCountOffset = kArgsByteOffset + sizeof(uint32_t);  // 16

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
		// LOD\Grass\<stem>.nif actually loaded, so grass types without an authored LOD cost nothing.
		// While these are null the cull CS routes every survivor to the full-detail bin.
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
		// capacity the LOD bin was sized for; recreated when the bucket grows past it
		uint32_t lodCapacityInstances = 0;
		// set per frame: LOD bin allocated and the setting is on -> the CS may use bin 1
		bool lodActive = false;

		// source mesh id, kept so the draw path can find this bucket's LOD mesh without re-resolving
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
		bool dirty = false;
		uint64_t descVal = 0;
		uint32_t firstNewSlice = UINT32_MAX;

		uint32_t drawnFrame = UINT32_MAX;
		RE::BSRenderPass* drawnPass = nullptr;

		RE::NiPoint3 coarseMin{};
		RE::NiPoint3 coarseMax{};
		bool coarseValid = false;
		bool cullVisible = false;

		// kept alongside the key so removal bookkeeping still works when the key is mesh-based
		RE::NiSourceTexture* diffuseTex = nullptr;

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

		void Release()
		{
			ReleaseResources();
			totalInstances = 0;
			slices.clear();
		}
	};

	struct RunSlot
	{
		uint32_t base;
		float fadeNow;
		float fadeInTimeRcp;
		uint32_t debugFlags;
		float origin[3];
		float pad1;
		uint32_t slotIndex;
		uint32_t isFar;
		uint32_t pad2[2];
	};

	struct CullParamsCB
	{
		float frustumPlanes[6][4];  // 96
		float maxDistSq;
		float fullDetailPixelSize;
		float meshCostBias;
		float lodMinKeep;  // 112
		float lodFadeBand;
		float projScale;
		float minPixelSize;
		float edgeFadeStart;    // 128 — fraction of max distance where the cull fade begins
		float collisionDistSq;  // 132
		float alphaParam1;
		float alphaParam2;
		float fadeNow;  // 144
		float fadeInTimeRcp;
		float invisibleFadeCull;
		// Mesh-swap LOD: instances below this projected size take the LOD mesh, hash-dithered
		// across a band of meshLODBandPx so a clump scatters into the swap instead of popping.
		float meshLODPixelSize;
		float meshLODBandPx;  // 160
		float hiZEnabled;
		float hiZSizeX;
		float hiZSizeY;
		float hiZTexelPixels;  // 176
		float hiZMipCount;
		float hiZPad[3];  // 192
	};
	static_assert(sizeof(CullParamsCB) % 16 == 0);

	struct CullBucketCB
	{
		uint32_t instanceCount;
		float wavePeriod;
		float timeBase;
		float prevTimeBase;
		float boundCenter[3];
		float clumpRadius;
		float distScale;
		float minPixelScale;
		float isComplex;
		// 0 = this bucket has no LOD bin this frame, every survivor goes to full detail
		float lodEnabled;
	};
	static_assert(sizeof(CullBucketCB) == 48);

	std::unordered_map<BucketKey, GrassBucket, BucketKeyHash> buckets;
	std::mutex bucketMutex;

	std::unordered_set<RE::NiSourceTexture*> bucketKeys;
	mutable std::shared_mutex bucketKeysMutex;

	std::vector<PendingCapture> pendingCaptures;
	std::vector<RE::BSMultiStreamInstanceTriShape*> pendingRemoves;
	std::mutex pendingMutex;

	std::unordered_map<RE::NiSourceTexture*, bool> complexCache;

	// Lower-poly LOD mesh for a grass type, loaded from meshes\LOD\Grass\<source-mesh-stem>.nif.
	// `valid` is false when no LOD mesh exists (or it is incompatible) — draw the full mesh then.
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
	std::unordered_map<uint32_t, LODMesh> lodMeshCache;  // keyed by meshId

	// Interned source-mesh stems: id 1..N (0 = unresolved). Interning keeps the per-draw bucket
	// lookup on integers instead of hashing a string every draw.
	std::unordered_map<std::string, uint32_t> meshIdMap;
	std::vector<std::string> meshStems;  // meshStems[id - 1]
	// resolved meshId per grass shape (cached so resolution happens once per shape)
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, uint32_t> shapeMeshId;

	// shape -> source .nif stem, recorded by the LoadGrassType hook at grass-type creation. Its
	// own mutex: the hook runs on the grass loader thread and must not block on bucketMutex,
	// which UpdateGrass holds across its GPU work.
	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, std::string> shapeModelStem;
	std::mutex meshPathMutex;

	uint32_t lastFrame = UINT32_MAX;

	ID3D11DeviceContext1* ctx1 = nullptr;
	bool triedCtx1Init = false;

	ID3D11ComputeShader* cullCS = nullptr;
	ID3D11Buffer* cullParamsCB = nullptr;  // per-frame frustum + params
	ID3D11Buffer* cullBucketCB = nullptr;
	uint32_t cullBucketCBSlots = 0;
	static constexpr uint32_t kSlotBytes = 256;
	bool cullInit = false;
	ID3D11Buffer* detectStaging = nullptr;

	ID3D11ComputeShader* detectCS = nullptr;
	ID3D11Buffer* detectResultBuf = nullptr;
	ID3D11UnorderedAccessView* detectResultUAV = nullptr;
	ID3D11Buffer* detectParamsCB = nullptr;

	float windTimer = 0.0f;
	float prevWindTimer = 0.0f;
	float timeAccum = 0.0f;
	float fadeInTimeRcp = 0.0f;
	float timeBase = 0.0f;
	float prevTimeBase = 0.0f;
	float cachedComplexThreshold = -1.0f;
	float grassStartFadeDistance = 0.0f;
	float vanillaMaxDistance = 0.0f;  // INI fGrassStartFadeDistance + fGrassFadeRange, cached once
	float maxGrassDistance = 0.0f;    // effective max this frame (vanilla or thinning-override)
	float maxDistSq = 0.0f;

	void InitCullResources();                                   // once
	void CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx);  // per bucket per frame

	void ComputeFrustumPlanes(RE::NiFrustumPlanes& out, const RE::NiFrustum& viewFrustum, const RE::NiTransform& transform);

	void UpdateGrass();
	void ApplyRemovals(const std::vector<RE::BSMultiStreamInstanceTriShape*>& removes);
	void ApplyCaptures(std::vector<PendingCapture>& captures);
	void UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx);
	void RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx);
	void AppendNewSlices(GrassBucket& bucket, ID3D11DeviceContext* ctx);
	bool EnsureBucketCapacity(GrassBucket& b, uint32_t neededInstances, ID3D11Device* device);
	bool AabbVisible(const RE::NiFrustumPlanes& f, const RE::NiPoint3& mn, const RE::NiPoint3& mx);
	void CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData);
	void UpdateCoarseBounds(GrassBucket& b);
	bool EnsureCullBucketCapacity(uint32_t slots, ID3D11Device* device);
	void CacheBucketTypeParams(GrassBucket& b, RE::BSMultiStreamInstanceTriShape* shape);
	bool StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex);

	bool DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11Device* device, ID3D11DeviceContext* ctx);

	/** @brief Resolves a grass shape to an interned source-mesh id (0 = unresolved). */
	uint32_t ResolveMeshId(RE::BSMultiStreamInstanceTriShape* shape);

	/** @brief Loads (once per source mesh) the lower-poly LOD .nif for this mesh id. */
	/** @brief Reduces the scene depth copy to the 1/16-res max-depth buffer the occlusion cull
	    samples. Returns true when hiZSRV is valid for this frame. */
	bool BuildHiZ(ID3D11Device* device, ID3D11DeviceContext* ctx);

	// 1/16-res max-depth reduction, rebuilt once per frame from POST_ZPREPASS_COPY
	ID3D11Texture2D* hiZTex = nullptr;
	ID3D11UnorderedAccessView* hiZUAV = nullptr;
	ID3D11ShaderResourceView* hiZSRV = nullptr;
	ID3D11ComputeShader* hiZCS = nullptr;
	ID3D11ComputeShader* hiZMipCS = nullptr;
	// TerrainBlending state the current hiZCS was compiled against; a change re-compiles it
	bool hiZCSTerrainBlending = false;
	// Per-level views for the max-depth pyramid. A coarse level is the exact max of its children,
	// which is what lets a large clump be tested with a fixed number of samples.
	std::vector<ID3D11UnorderedAccessView*> hiZMipUAVs;
	std::vector<ID3D11ShaderResourceView*> hiZMipSRVs;
	uint32_t hiZMipCount = 1;
	ID3D11Buffer* hiZParamsCB = nullptr;
	uint32_t hiZWidth = 0;
	uint32_t hiZHeight = 0;
	bool hiZValid = false;
	static constexpr uint32_t kHiZTileSize = 16;

	void EnsureLODMesh(uint32_t meshId);

	/** @brief Allocates/frees a bucket's second (LOD) compaction bin to match the current setting
	    and the bucket's capacity. Returns true when the bucket has a usable LOD bin this frame. */
	bool EnsureLODBin(GrassBucket& b, ID3D11Device* device);

	/** @brief Draws the ImGui settings panel for grass optimizations configuration. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Installs the hooks after all plugins have loaded. */
	virtual void PostPostLoad() override;

	struct Hooks
	{
		struct BSMultiStreamInstanceTriShape_dtor
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* This);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSMultiStreamInstanceTriShape_OnVisible
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DoneAddingInstances
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* geometry, RE::BSTArray<std::uint32_t>& a_instances);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSGrassShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSMultiBoundAABB_WithinFrustum
		{
			static bool thunk(RE::BSMultiBoundAABB* a_this, RE::NiFrustumPlanes* a_planes);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddQueuedGroupGIDBuffer
		{
			static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddGroupGIDBuffer
		{
			static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ReadGroupHeaderStreamTraits
		{
			static void thunk(RE::BSStreamHeader* streamHeader, RE::BSMultiStreamInstanceTriShape::GroupHeader* groupHeader, uint32_t size);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ReadInstanceGroupStreamTraits
		{
			static void thunk(RE::BSStreamHeader* streamHeader, uint16_t* groupHeader, uint32_t size);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddGroupQueuedGIDFile
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddGroupGIDFile
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DrawInstanceTriShape
		{
			static void thunk(RE::BSRenderPass* curPass, RE::BSMultiStreamInstanceTriShape* geometry);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LoadGrassType
		{
			static RE::BSMultiStreamInstanceTriShape* thunk(RE::BGSGrassManager* grassManager,
				RE::GrassParam* a_param,
				uint32_t CellXDivided,
				uint32_t CellYDivided,
				uint64_t* typeKey,
				RE::BSFixedString* modelPath);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			auto& trampoline = SKSE::GetTrampoline();

			stl::write_vfunc<0x0, BSMultiStreamInstanceTriShape_dtor>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x34, BSMultiStreamInstanceTriShape_OnVisible>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x3A, DoneAddingInstances>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);

			stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);
			stl::write_vfunc<0x29, BSMultiBoundAABB_WithinFrustum>(RE::VTABLE_BSMultiBoundAABB[0]);

			// Hooks to get capture raw instance data for cached grass
			stl::write_thunk_call<AddQueuedGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x7FF, 0));
			stl::write_thunk_call<AddGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x806, 0x75D));
			stl::write_thunk_call<ReadGroupHeaderStreamTraits>(REL::RelocationID(74599, 76327).address() + REL::Relocate(0x36, 0x36));
			stl::write_thunk_call<ReadGroupHeaderStreamTraits>(REL::RelocationID(74596, 76324).address() + REL::Relocate(0x2F, 0x33));
			stl::write_thunk_call<ReadInstanceGroupStreamTraits>(REL::RelocationID(74607, 76339).address() + REL::Relocate(0xCF, 0xCF));
			stl::write_thunk_call<AddGroupQueuedGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x394, 0x384));
			stl::write_thunk_call<AddGroupGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x39B, 0x38B));

			stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15204, 15372).address() + REL::Relocate(0x2F5, 0x0));
			stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x62B, 0x0));
			stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x25C, 0x0));

			std::uint8_t patch[] = { 0x4C, 0x89, 0xF2 };  // mov rdx, r14
			REL::safe_write(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x660, 0x648), patch, sizeof(patch));
			stl::write_thunk_call<DrawInstanceTriShape>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x663, 0x64B));
			trampoline.write_branch<5>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x668, 0x650), REL::RelocationID(100847, 107637).address() + REL::Relocate(0x759, 0x73A));

			// Skip mapping vanilla dynamic fade buffer
			if (REL::Module::IsAE()) {
				trampoline.write_branch<5>(REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x595), REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x6C6));
			} else {
				REL::safe_write(REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x595), REL::NOP5);
			}

			logger::info("[GRASS OPTIMIZATIONS] Installed hooks");
		}
	};
};
