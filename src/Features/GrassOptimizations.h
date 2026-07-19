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
		bool ShowDebugVisualization = false;
		float BeginThinningDistance = 8000.0f;
		float ThinningDistance = 10000.0f;
		float MinDistantAmmount = 0.10f;
		float shadowGrassDistance = 8000.0f;
		float farGrassDistance = 8000.0f;
	};

	Settings settings;

	struct BucketKey
	{
		RE::NiSourceTexture* tex;
		uint64_t descVal;
		bool operator==(const BucketKey&) const = default;
	};

	struct BucketKeyHash
	{
		size_t operator()(const BucketKey& k) const
		{
			return std::hash<void*>{}(k.tex) ^ (std::hash<uint64_t>{}(k.descVal) << 1);
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
			rel(argsBuf);
			capacityInstances = 0;
			argsIndexCountWritten = false;
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
		float lodNearDistSq;
		float lodFarDistSq;
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
		float pad[3];  // 160
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
		float pad;
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
	float maxGrassDistance = 0.0f;
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
