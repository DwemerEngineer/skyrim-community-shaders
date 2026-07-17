#pragma once

#include "Buffer.h"
#include "Upscaling.h"

struct BucketSlice
{
	RE::BSMultiStreamInstanceTriShape* shape;
	std::vector<float> data;
	uint32_t count;
	float fadeStart;
	RE::NiPoint3 origin;
	uint32_t bufferOffset = UINT32_MAX;
};

struct PendingCapture
{
	RE::BSMultiStreamInstanceTriShape* shape = nullptr;
	RE::NiSourceTexture* diffuseTexture = nullptr;
	std::vector<float> expanded;
	uint32_t count = 0;
	uint64_t descVal = 0;
	RE::NiPoint3 origin;
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
	static constexpr uint32_t kBands = 2;

	// Static GPU state (rebuilt on capture/removal)
	ID3D11Buffer* instanceBuf = nullptr;  // ByteAddressBuffer: raw half-packed records
	ID3D11ShaderResourceView* instanceSRV = nullptr;
	ID3D11Buffer* originBuf = nullptr;  // StructuredBuffer<float3> per-instance origin
	ID3D11ShaderResourceView* originSRV = nullptr;
	ID3D11Buffer* fadeBuf = nullptr;  // StructuredBuffer<float> per-instance fade start
	ID3D11ShaderResourceView* fadeSRV = nullptr;

	ID3D11Buffer* visibleBuf[kBands] = {};
	ID3D11UnorderedAccessView* visibleUAV[kBands] = {};
	ID3D11ShaderResourceView* visibleSRV[kBands] = {};
	ID3D11Buffer* argsBuf[kBands] = {};

	ID3D11Buffer* windBuf = nullptr;  // float2 per instance {cur, prev}
	ID3D11UnorderedAccessView* windUAV = nullptr;
	ID3D11ShaderResourceView* windSRV = nullptr;

	ID3D11Buffer* lodFadeBuf = nullptr;  // float per instance
	ID3D11UnorderedAccessView* lodFadeUAV = nullptr;
	ID3D11ShaderResourceView* lodFadeSRV = nullptr;

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

	bool isComplex = false;
	bool argsIndexCountWritten = false;

	uint32_t cullSlot = UINT32_MAX;

	bool typeParamsValid = false;
	float wavePeriod = 1.0f;
	RE::NiPoint3 boundCenter{};
	float clumpRadius = 128.0f;
	float distScale = 1.0f;
	float minPixelScale = 1.0f;

	void ReleaseResources()
	{
		auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		rel(instanceSRV);
		rel(instanceBuf);
		rel(originSRV);
		rel(originBuf);
		rel(fadeSRV);
		rel(fadeBuf);
		for (uint32_t i = 0; i < kBands; ++i) {
			rel(visibleSRV[i]);
			rel(visibleUAV[i]);
			rel(visibleBuf[i]);
			rel(argsBuf[i]);
		}
		rel(windSRV);
		rel(windUAV);
		rel(windBuf);
		rel(lodFadeSRV);
		rel(lodFadeUAV);
		rel(lodFadeBuf);
		capacityInstances = 0;
		argsIndexCountWritten = false;
	}

	void Release()
	{
		ReleaseResources();
		totalInstances = 0;
		slices.clear();
	}
	// cull buffers (visibleBuf/argsBuf + UAVs) added in piece 2
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

	std::unordered_map<BucketKey, GrassBucket, BucketKeyHash> buckets;
	std::mutex bucketMutex;

	std::vector<PendingCapture> pendingCaptures;
	std::vector<RE::BSMultiStreamInstanceTriShape*> pendingRemoves;
	std::mutex pendingMutex;

	RE::NiFrustumPlanes capturedPlanes{};
	RE::NiPoint3 capturedCamPos{};
	uint32_t planesFrame = UINT32_MAX;
	std::mutex planesMutex;
	float maxGrassDistance = 0.0f;
	float maxDistSq = 0.0f;

	REL::Relocation<const RE::NiRTTI*> BSGrassShaderProperty_Ni_RTTI{ RE::BSGrassShaderProperty::Ni_RTTI };
	REL::Relocation<const RE::NiRTTI*> BSMultiStreamInstanceTriShape_Ni_RTTI{ RE::BSMultiStreamInstanceTriShape::Ni_RTTI };
	REL::Relocation<const RE::NiRTTI*> BSParabolicCullingProcess_Ni_RTTI{ RE::BSParabolicCullingProcess::Ni_RTTI };
	REL::Relocation<const RE::NiRTTI*> BSCubeMapCamera_Ni_RTTI{ RE::BSCubeMapCamera::Ni_RTTI };

	std::unordered_set<RE::NiSourceTexture*> bucketKeys;
	mutable std::shared_mutex bucketKeysMutex;

	uint32_t lastFrame = UINT32_MAX;
	uint32_t lastRegisteredFrame12 = UINT32_MAX;
	uint32_t lastRegisteredFrame0 = UINT32_MAX;

	uint32_t retireFrame = UINT32_MAX;
	ID3D11DeviceContext1* ctx1 = nullptr;
	bool triedCtx1Init = false;

	float timeAccum = 0.0f;
	float fadeInTimeRcp = 0.0f;

	ID3D11Buffer* triggerCB = nullptr;
	RE::NiPoint3 lastTriggerOrigin{ FLT_MAX, FLT_MAX, FLT_MAX };

	float windTimer = 0.0f;
	float prevWindTimer = 0.0f;

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
		float pad[2];
	};
	static_assert(sizeof(CullBucketCB) == 48);

	ID3D11ComputeShader* cullCS = nullptr;
	ID3D11Buffer* cullParamsCB = nullptr;  // per-frame frustum + params
	ID3D11Buffer* cullBucketCB = nullptr;
	uint32_t cullBucketCBSlots = 0;
	static constexpr uint32_t kSlotBytes = 256;
	bool cullInit = false;
	ID3D11Buffer* bandCB = nullptr;  // static 2 slots: {0} near, {1} far

	struct CullParamsCB
	{
		float frustumPlanes[6][4];  // 96
		float cameraPos[3];         // 108
		uint32_t pad0;              // 112
		float maxDistSq;
		float lodNearDistSq;
		float lodFarDistSq;
		float lodMinKeep;  // 128
		float lodFadeBand;
		float projScale;
		float minPixelSize;
		float bandDistSq;  // 144
	};
	static_assert(sizeof(CullParamsCB) == 144);
	ID3D11Buffer* grassFrameCB = nullptr; 

	ID3D11ComputeShader* detectCS = nullptr;
	ID3D11Buffer* detectResultBuf = nullptr;
	ID3D11UnorderedAccessView* detectResultUAV = nullptr;
	ID3D11Buffer* detectStaging = nullptr;
	ID3D11Buffer* detectParamsCB = nullptr; 
	std::unordered_map<RE::NiSourceTexture*, bool> complexCache;

	float timeBase = 0.0f;
	float prevTimeBase = 0.0f;

	float cachedComplexThreshold = -1.0f; 

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
	bool StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src,uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex);

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

			if (REL::Module::IsAE()) {
				trampoline.write_branch<5>(REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x595), REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x6C6));
			} else {
				REL::safe_write(REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x595), REL::NOP5);
			}

			logger::info("[GRASS OPTIMIZATIONS] Installed hooks");
		}
	};
};
