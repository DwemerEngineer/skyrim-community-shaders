#pragma once

#include "Buffer.h"
#include "Upscaling.h"

struct BucketSlice
{
	RE::BSMultiStreamInstanceTriShape* shape;
	std::vector<uint8_t> data;
	uint32_t count;
	float fadeStart;
	RE::NiPoint3 aabbMin;
	RE::NiPoint3 aabbMax;
};

struct VisibleRun
{
	uint32_t base;          // StartInstanceLocation / alpha-SRV base
	uint32_t count;         // instance count
	uint32_t cbFirstConst = UINT32_MAX;  // offset into baseCB, in 16-byte constants (multiple of 16)
	float sortKeySq = 0.0f;
};

struct GrassBucket
{
	ID3D11Buffer* instanceBuf = nullptr;
	ID3D11Buffer* fadeBuf = nullptr;
	ID3D11ShaderResourceView* fadeSRV = nullptr;
	uint32_t capacityInstances = 0;
	uint32_t totalInstances = 0;
	std::vector<BucketSlice> slices;
	bool dirty = false;
	uint32_t drawnFrame = UINT32_MAX;
	uint32_t drawnTechnique = UINT32_MAX;
	uint64_t descVal = 0;
	void* drawnVS = nullptr;
	uint32_t firstNewSlice = UINT32_MAX;
	std::vector<VisibleRun> visibleRuns;
	uint32_t registeredFrame = UINT32_MAX;
	const void* registeredProc = nullptr;

	void ReleaseResources()
	{
		if (instanceBuf)
			instanceBuf->Release();
		if (fadeBuf)
			fadeBuf->Release();
		if (fadeSRV)
			fadeSRV->Release();

		instanceBuf = fadeBuf = nullptr;
		fadeSRV = nullptr;
		capacityInstances = 0;
	}

	void Release()  
	{
		ReleaseResources();
		totalInstances = 0;
		slices.clear();
		visibleRuns.clear();
	}
};

struct PendingCapture
{
	RE::BSMultiStreamInstanceTriShape* shape = nullptr;
	RE::NiSourceTexture* diffuseTexture = nullptr;
	uint32_t instanceStride = 0;
	std::vector<uint8_t> bytes{};
	uint32_t count = 0;
	uint64_t descVal = 0;
	RE::NiPoint3 aabbMin;
	RE::NiPoint3 aabbMax;
};

struct RunSlot
{
	uint32_t base;
	float fadeNow;
	float fadeInTimeRcp;
	uint32_t pad;
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

	std::unordered_map<RE::NiSourceTexture*, GrassBucket> buckets;
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

	ID3D11Buffer* runBaseCB = nullptr;
	uint32_t runBaseCBCapacity = 0;
	ID3D11Buffer* runBaseCBRetired = nullptr;
	uint32_t retireFrame = UINT32_MAX;
	ID3D11DeviceContext1* ctx1 = nullptr;   
	bool triedCtx1Init = false;

	float timeAccum = 0.0f;
	float fadeInTimeRcp = 0.0f;

	void UpdateGrass();
	void ApplyRemovals(const std::vector<RE::BSMultiStreamInstanceTriShape*>& removes);
	void ApplyCaptures(std::vector<PendingCapture>& captures);
	void UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx);
	void RebuildBucket(GrassBucket& bucket, uint32_t instanceStride, ID3D11Device* device, ID3D11DeviceContext* ctx);
	void AppendNewSlices(GrassBucket& bucket, uint32_t instanceStride, ID3D11DeviceContext* ctx);
	void BuildVisibleRuns();
	bool EnsureBucketCapacity(GrassBucket& b, uint32_t neededInstances, uint32_t instanceStride, ID3D11Device* device);
	bool AabbVisible(const RE::NiFrustumPlanes& f, const RE::NiPoint3& mn, const RE::NiPoint3& mx);
	void CaptureGIDGroup(RE::BSMultiStreamInstanceTriShape* shape, RE::BSMultiStreamInstanceTriShape::GroupHeader* header, const uint16_t* instanceData);
	void ComputeCaptureAabb(PendingCapture& pc, const RE::NiPoint3& shapeTranslate);
	bool IsBucketRegistered(RE::BSMultiStreamInstanceTriShape* shape, uint32_t frame, const RE::NiCullingProcess* process);
	void MarkBucketRegistered(RE::BSMultiStreamInstanceTriShape* shape, uint32_t frame, const RE::NiCullingProcess* process);

	bool IsBucketKey(RE::NiSourceTexture* tex) const;
	RE::NiSourceTexture* GetGrassBucketKey(RE::NiAVObject* obj) const;

	void InitRunBaseCB();                           // call once at feature init
	void UploadRunBases(ID3D11DeviceContext* ctx);  // per frame, after BuildVisibleRuns
	bool EnsureRunBaseCapacity(uint32_t slots, ID3D11Device* device);

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

		struct ExecuteCullingPass
		{
			static void thunk(void* a_cullParam, int a2, int a3);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSCullingProcess_Process
		{
			static void thunk(RE::BSCullingProcess* process, RE::BSTArray<RE::NiPointer<RE::NiAVObject>>* objArray, bool processCullingProcess, bool queueCullingJob);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct RegisterObject
		{
			static void thunk(RE::BSShaderAccumulator*, RE::NiAVObject*, RE::BSBatchRenderer::GeometryGroup*);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ProcessAlphaGroups
		{
			static void thunk(RE::BSGeometryListCullingProcess*, RE::BSShaderAccumulator*);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct WithinFrustum
		{
			static bool thunk(RE::BSMultiBoundAABB* a_this, RE::NiFrustumPlanes* a_planes);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			auto& trampoline = SKSE::GetTrampoline();

			stl::write_vfunc<0x0, BSMultiStreamInstanceTriShape_dtor>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			//stl::write_vfunc<0x34, BSMultiStreamInstanceTriShape_OnVisible>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x3A, DoneAddingInstances>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);

			stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);
			//stl::write_vfunc<0x29, BSMultiBoundAABB_WithinFrustum>(RE::VTABLE_BSMultiBoundAABB[0]);

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

			//stl::write_thunk_call<ExecuteCullingPass>(REL::RelocationID(100416, 107134).address() + REL::Relocate(0xFE, 0));
			//stl::write_thunk_call<RegisterObject>(REL::RelocationID(74809, 0).address() + REL::Relocate(0x73, 0));
			
			//stl::write_thunk_call<RegisterObject>(REL::RelocationID(74809, 0).address() + REL::Relocate(0x159, 0));
			//stl::write_thunk_call<RegisterObject>(REL::RelocationID(101601, 0).address() + REL::Relocate(0x70, 0));
			//stl::write_thunk_call<RegisterObject>(REL::RelocationID(99974, 0).address() + REL::Relocate(0xE4, 0));

			//stl::write_thunk_call<ProcessAlphaGroups>(REL::RelocationID(100219, 0).address() + REL::Relocate(0x4E, 0));

			//stl::write_thunk_call<WithinFrustum, 6>(REL::RelocationID(74602, 0).address() + REL::Relocate(0x19C, 0));

			logger::info("[GRASS OPTIMIZATIONS] Installed hooks");
		}
	};
};
