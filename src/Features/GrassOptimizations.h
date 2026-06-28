#pragma once

#include "Buffer.h"
#include "Upscaling.h"

struct MergedGrassData
{
	ID3D11Buffer* mergedInstanceVertBuf = nullptr; 
	ID3D11Buffer* instanceGroupMapBuffer = nullptr;
	ID3D11ShaderResourceView* instanceGroupMapSRV = nullptr;

	ID3D11Buffer* groupFadeBuffer = nullptr;
	ID3D11ShaderResourceView* groupFadeSRV = nullptr;
	uint32_t groupFadeCapacity = 0;

	uint32_t totalInstances = 0;
	uint32_t mergedCapacity = 0;
	bool needsRebuild = true;
	uint32_t builtGroupGen = 0;
	uint32_t rebuildFramesLeft = 0;

	~MergedGrassData()
	{
		if (mergedInstanceVertBuf) {
			mergedInstanceVertBuf->Release();
			mergedInstanceVertBuf = nullptr;
		}
		if (instanceGroupMapSRV) {
			instanceGroupMapSRV->Release();
			instanceGroupMapSRV = nullptr;
		}
		if (instanceGroupMapBuffer) {
			instanceGroupMapBuffer->Release();
			instanceGroupMapBuffer = nullptr;
		}

		if (groupFadeSRV) {
			groupFadeSRV->Release();
			groupFadeSRV = nullptr;
		}
		if (groupFadeBuffer) {
			groupFadeBuffer->Release();
			groupFadeBuffer = nullptr;
		}
	}
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

	std::unordered_map<RE::BSMultiStreamInstanceTriShape*, MergedGrassData> mergedData;
	std::mutex mergedMutex;

	std::mutex pendingMutex;  // guards the pending lists
	std::vector<RE::BSMultiStreamInstanceTriShape*> pendingRemove;
	std::vector<RE::BSMultiStreamInstanceTriShape*> pendingDirty;
	uint32_t lastSetupFrame = UINT32_MAX;

	uint32_t maxDrawsPerFrame = 8;
	uint32_t rebuildsThisFrame = 0;

	uint32_t cb2Frame = 0;
	std::pair<uint32_t, float> lastCB2Key{ UINT32_MAX, FLT_MAX };

	MergedGrassData& GetOrCreateMergedData(RE::BSMultiStreamInstanceTriShape* shape)
	{
		auto& data = mergedData[shape];
		std::lock_guard lock(mergedMutex);

		return data;
	}

	void EnsureFadeCapacity(MergedGrassData& d, uint32_t groupCount, ID3D11Device* device);
	void EnsureMergedCapacity(MergedGrassData& d, uint32_t totalInstances, uint32_t instanceStride, ID3D11Device* device);
	void BuildMergedBuffers(RE::BSMultiStreamInstanceTriShape* shape, MergedGrassData& data, uint32_t instanceStride, ID3D11Device* device, ID3D11DeviceContext* ctx);
	void HookDrawMultiStreamInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry);
	void DrainPending();

	/** @brief Installs the hooks after all plugins have loaded. */
	virtual void PostPostLoad() override;

	struct Hooks
	{
		struct BSMultiStreamInstanceTriShape_dtor
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* This);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSMultiStreamInstanceTriShape_DoneAddingInstances
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* This, RE::BSTArray<std::uint32_t>& a_instances);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSMultiStreamInstanceTriShape_AddGroup
		{
			static uint32_t thunk(RE::BSMultiStreamInstanceTriShape* This, std::uint32_t a_numInstances, std::uint16_t* a_instanceData, std::uint32_t numShorts, float a_arg4);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSMultiStreamInstanceTriShape_RemoveGroup
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* This, std::uint32_t a_numInstances);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSGrassShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/*
		struct BSGrassShaderProperty_dtor
		{
			static void thunk(RE::BSGrassShaderProperty* This);
			static inline REL::Relocation<decltype(thunk)> func;
		};
		*/

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

		struct AddGroupGIDFile
		{
			static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DrawInstanceTriShape
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* geometry);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ProcessAttachQueue
		{
			static void thunk(RE::BGSGrassManager* a1);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ID3D11Device_CreateInputLayout
		{
			static ID3D11InputLayout* thunk(ID3D11Device* device, const D3D11_INPUT_ELEMENT_DESC* elementDesc, UINT elementCount, const void* shaderByteCode, SIZE_T bytecodeLength, ID3D11InputLayout** inputLayout);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SetLayout
		{
			static void thunk(ID3D11DeviceContext * cxt, ID3D11InputLayout * layout);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddQueuedGroupDone
		{
			static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, uint32_t a_instanceCount, void* groupAlloc, uint32_t a4, RE::BSTArray<std::uint32_t>& groupIndicies, float a6);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{

			auto& trampoline = SKSE::GetTrampoline();

			stl::write_vfunc<0x0, BSMultiStreamInstanceTriShape_dtor>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x3A, BSMultiStreamInstanceTriShape_DoneAddingInstances>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x3C, BSMultiStreamInstanceTriShape_AddGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x3D, BSMultiStreamInstanceTriShape_RemoveGroup>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			//stl::write_vfunc<0x0, BSGrassShaderProperty_dtor>(RE::VTABLE_BSGrassShaderProperty[0]);
			stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);

			stl::write_thunk_call<AddQueuedGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x7FF, 0));
			stl::write_thunk_call<AddGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x806, 0x75D));
			stl::write_thunk_call<ReadGroupHeaderStreamTraits>(REL::RelocationID(74596, 76324).address() + REL::Relocate(0x2F, 0x33));
			stl::write_thunk_call<ReadInstanceGroupStreamTraits>(REL::RelocationID(74607, 76339).address() + REL::Relocate(0xCF, 0xCF));
			stl::write_thunk_call<AddGroupGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x39B, 0x38B));

			stl::write_thunk_call<AddQueuedGroupDone>(REL::RelocationID(74592, 0).address() + REL::Relocate(0x163, 0));

			std::uint8_t patch[] = { 0x4C, 0x89, 0xF1 };  // mov rcx, [r14+170h]
			REL::safe_write(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x660, 0x648), patch, sizeof(patch));
			stl::write_thunk_call<DrawInstanceTriShape>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x663, 0x64B));
			trampoline.write_branch<5>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x668, 0x650), REL::RelocationID(100847, 107637).address() + REL::Relocate(0x759, 0x73A));

			stl::write_thunk_call<ProcessAttachQueue>(REL::RelocationID(35586, 36592).address() + REL::Relocate(0x57, 0x87));

			if (REL::Module::IsAE()) {
				trampoline.write_branch<5>(REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x595), REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x6C6));
			} else {
				REL::safe_write(REL::RelocationID(99996, 106685).address() + REL::Relocate(0x54D, 0x595), REL::NOP5);
			}


			logger::info("[GRASS OPTIMIZATIONS] Installed hooks");
		}
	};
};
