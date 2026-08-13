#include "TerrainShadows.h"

#include <bit>
#include <cctype>
#include <ranges>

#include <DirectXTex.h>
#include <pystring/pystring.h>

#include "I18n/I18n.h"
#include "State.h"
#include "TerrainHeightMap.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.terrain_shadows."

namespace
{
	void RequestTimeJumpSynchronization()
	{
		Util::RequestTimeJumpTransition();
	}

	class TimeJumpEventHandler :
		public RE::BSTEventSink<RE::TESWaitStopEvent>,
		public RE::BSTEventSink<RE::TESSleepStopEvent>,
		public RE::BSTEventSink<RE::TESFastTravelEndEvent>,
		public RE::BSTEventSink<RE::BGSActorCellEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESWaitStopEvent*, RE::BSTEventSource<RE::TESWaitStopEvent>*) override
		{
			RequestTimeJumpSynchronization();
			return RE::BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
		{
			RequestTimeJumpSynchronization();
			return RE::BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
		{
			RequestTimeJumpSynchronization();
			return RE::BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
		{
			if (!a_event || a_event->flags != RE::BGSActorCellEvent::CellFlag::kLeave)
				return RE::BSEventNotifyControl::kContinue;

			const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_event->cellID);
			if (cell && cell->IsInteriorCell())
				RequestTimeJumpSynchronization();
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	struct ConsoleCommandHook
	{
		static bool IsGameHourSetCommand(std::string_view a_command) noexcept
		{
			auto consumeToken = [&](std::string_view a_token) {
				while (!a_command.empty() && std::isspace(static_cast<unsigned char>(a_command.front())))
					a_command.remove_prefix(1);

				const auto tokenEnd = a_command.find_first_of(" \t\r\n");
				const auto token = a_command.substr(0, tokenEnd);
				if (!std::ranges::equal(token, a_token, [](char a_lhs, char a_rhs) {
						return std::tolower(static_cast<unsigned char>(a_lhs)) == std::tolower(static_cast<unsigned char>(a_rhs));
					})) {
					return false;
				}

				a_command.remove_prefix(token.size());
				return true;
			};

			if (!consumeToken("set") || !consumeToken("gamehour") || !consumeToken("to"))
				return false;

			while (!a_command.empty() && std::isspace(static_cast<unsigned char>(a_command.front())))
				a_command.remove_prefix(1);
			return !a_command.empty();
		}

		static void thunk(RE::FxDelegateArgs* a_args)
		{
			bool changesGameHour = false;
			if (a_args && a_args->GetArgCount() > 0) {
				const auto& commandValue = (*a_args)[0];
				if (commandValue.IsString()) {
					const auto command = commandValue.GetString();
					changesGameHour = command && IsGameHourSetCommand(command);
				}
			}

			func(a_args);
			if (changesGameHour)
				RequestTimeJumpSynchronization();
		}

		static void Install()
		{
			stl::detour_thunk<ConsoleCommandHook>(REL::RelocationID(50157, 51084));
			logger::info("[Terrain Shadows] Installed console time-change hook");
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PapyrusSetGlobalValueHook
	{
		static void thunk(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::TESGlobal* a_global, float a_value)
		{
			const auto calendar = globals::game::calendar;
			const bool changesGameHour = calendar && a_global == calendar->gameHour;
			const float previousValue = changesGameHour ? a_global->value : 0.0f;

			func(a_vm, a_stackID, a_global, a_value);
			if (changesGameHour && a_global->value != previousValue)
				RequestTimeJumpSynchronization();
		}

		static void Install()
		{
			stl::detour_thunk<PapyrusSetGlobalValueHook>(REL::RelocationID(55352, 55923));
			logger::info("[Terrain Shadows] Installed Papyrus GameHour change hook");
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainShadows::Settings,
	EnableTerrainShadow)

void TerrainShadows::PostPostLoad()
{
	ConsoleCommandHook::Install();
	PapyrusSetGlobalValueHook::Install();
}

void TerrainShadows::DataLoaded()
{
	const auto eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
	if (!eventSourceHolder) {
		logger::error("[Terrain Shadows] Script event source holder not found");
		return;
	}

	static TimeJumpEventHandler eventHandler;
	if (const auto waitEvents = eventSourceHolder->GetEventSource<RE::TESWaitStopEvent>())
		waitEvents->AddEventSink(&eventHandler);
	if (const auto sleepEvents = eventSourceHolder->GetEventSource<RE::TESSleepStopEvent>())
		sleepEvents->AddEventSink(&eventHandler);
	if (const auto fastTravelEvents = eventSourceHolder->GetEventSource<RE::TESFastTravelEndEvent>())
		fastTravelEvents->AddEventSink(&eventHandler);

	if (const auto player = RE::PlayerCharacter::GetSingleton())
		player->AsBGSActorCellEventSource()->AddEventSink(&eventHandler);
	else
		logger::warn("[Terrain Shadows] Player singleton not found");
}

void TerrainShadows::GameLoaded()
{
	Util::RequestGameLoadTransition();
}

void TerrainShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainShadows::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable_terrain_shadow"), "Enable Terrain Shadow"), &settings.EnableTerrainShadow);

	if (ImGui::CollapsingHeader(T(TKEY("debug"), "Debug"))) {
		std::string curr_worldspace = "N/A";
		std::string curr_worldspace_name = "N/A";
		auto tes = RE::TES::GetSingleton();
		if (tes) {
			auto worldspace = tes->GetRuntimeData2().worldSpace;
			if (worldspace) {
				curr_worldspace = worldspace->GetFormEditorID();
				curr_worldspace_name = worldspace->GetName();
			}
		}
		ImGui::Text(fmt::format("Current worldspace: {} ({})", curr_worldspace, curr_worldspace_name).c_str());
		ImGui::Text(fmt::format("Has height map: {}", TerrainHeightMap::GetSingleton()->Contains(curr_worldspace)).c_str());

		ImGui::Separator();

		ImGui::BulletText("shadowUpdateCBData");
		ImGui::Indent();
		{
			ImGui::Text(fmt::format("LightPxDir: ({}, {})", shadowUpdateCBData.LightPxDir.x, shadowUpdateCBData.LightPxDir.y).c_str());
			ImGui::Text(fmt::format("LightDeltaZ: ({}, {})", shadowUpdateCBData.LightDeltaZ.x, shadowUpdateCBData.LightDeltaZ.y).c_str());
			ImGui::Text(fmt::format("StartPxCoord: {}", shadowUpdateCBData.StartPxCoord).c_str());
			ImGui::Text(fmt::format("PxSize: ({}, {})", shadowUpdateCBData.PxSize.x, shadowUpdateCBData.PxSize.y).c_str());
		}
		ImGui::Unindent();

		if (ImGui::TreeNode(T(TKEY("buffer_viewer"), "Buffer Viewer"))) {
			static float debugRescale = .1f;
			ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

			if (texShadowHeight) {
				BUFFER_VIEWER_NODE_BULLET(texShadowHeight, debugRescale)
			}
			ImGui::TreePop();
		}
	}
}

void TerrainShadows::ClearShaderCache()
{
	if (shadowUpdateProgram) {
		shadowUpdateProgram->Release();
		shadowUpdateProgram = nullptr;
	}

	CompileComputeShaders();
}

void TerrainShadows::SetupResources()
{
	TerrainHeightMap::GetSingleton()->Discover();

	logger::debug("Creating constant buffers...");
	{
		shadowUpdateCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShadowUpdateCB>(), "TerrainShadows::UpdateCB");
	}

	CompileComputeShaders();
}

void TerrainShadows::CompileComputeShaders()
{
	logger::debug("Compiling shaders...");
	{
		auto program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\ShadowUpdate.cs.hlsl", {}, "cs_5_0"));
		if (program_ptr)
			shadowUpdateProgram.attach(program_ptr);
	}
}

bool TerrainShadows::IsHeightMapReady()
{
	return TerrainHeightMap::GetSingleton()->IsReady();
}

TerrainShadows::PerFrame TerrainShadows::GetCommonBufferData()
{
	auto heightMap = TerrainHeightMap::GetSingleton();
	bool isHeightmapReady = IsHeightMapReady();

	PerFrame data = {
		.EnableTerrainShadow = settings.EnableTerrainShadow && isHeightmapReady,
	};

	if (isHeightmapReady) {
		data.Scale = heightMap->GetScale();
		data.Offset = heightMap->GetOffset();
		data.ZRange = heightMap->GetZRange();
	}

	return data;
}

void TerrainShadows::Precompute()
{
	if (!TerrainHeightMap::GetSingleton()->GetCached())
		return;

	logger::info("Creating shadow texture...");
	{
		if (texShadowHeight) {
			auto context = globals::d3d::context;

			std::array<ID3D11ShaderResourceView*, 1> srvs = { nullptr };
			context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
			context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		}

		texShadowHeight.release();

		auto texHeightMap = TerrainHeightMap::GetSingleton()->GetTexture();

		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = texHeightMap->desc.Width,
			.Height = texHeightMap->desc.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texShadowHeight = std::make_unique<Texture2D>(texDesc, "TerrainShadows::ShadowHeight");
		texShadowHeight->CreateSRV(srvDesc);
		texShadowHeight->CreateUAV(uavDesc);
	}
#undef I18N_KEY_PREFIX

	needPrecompute = false;
}

bool TerrainShadows::UpdateShadow(bool a_refreshImmediately)
{
	ZoneScoped;

	if (!IsHeightMapReady())
		return false;

	// don't forget to change NTHREADS in shader!
	constexpr uint updateLength = 128u;
	constexpr uint logUpdateLength = std::bit_width(128u) - 1;  // integer log2, https://stackoverflow.com/questions/994593/how-to-do-an-integer-log2-in-c

	auto context = globals::d3d::context;

	if (texShadowHeight) {
		std::array<ID3D11ShaderResourceView*, 1> srvs = { nullptr };
		context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	}

	auto accumulator = *globals::game::currentAccumulator.get();
	auto shadowSceneNode = accumulator->GetRuntimeData().activeShadowSceneNode;
	if (!shadowSceneNode)
		return false;
	auto sunLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get());
	if (!sunLight)
		return false;
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Occlusion - Update Shadows");

	/* ---- UPDATE CB ---- */
	auto heightMap = TerrainHeightMap::GetSingleton();
	auto texHeightMap = heightMap->GetTexture();
	auto cachedHeightmap = heightMap->GetCached();

	uint width = texHeightMap->desc.Width;
	uint height = texHeightMap->desc.Height;

	// only update direction at the start of each cycle
	static uint edgePxCoord;
	static int signDir;
	static uint maxUpdates;
	if (a_refreshImmediately)
		shadowUpdateIdx = 0;
	if (shadowUpdateIdx == 0) {
		const auto worldDirection = sunLight->GetWorldDirection();
		float3 dirLightDir = { worldDirection.x, worldDirection.y, worldDirection.z };
		if (dirLightDir.z > 0)
			dirLightDir = -dirLightDir;

		// in UV
		float3 invScale = cachedHeightmap->pos1 - cachedHeightmap->pos0;
		invScale.z = cachedHeightmap->zRange.y - cachedHeightmap->zRange.x;
		float3 dirLightPxDir = dirLightDir / invScale;
		dirLightPxDir.x *= width;
		dirLightPxDir.y *= height;

		float stepMult;
		if (abs(dirLightPxDir.x) >= abs(dirLightPxDir.y)) {
			stepMult = 1.f / abs(dirLightPxDir.x);
			edgePxCoord = dirLightPxDir.x > 0 ? 0 : (width - 1);
			signDir = dirLightPxDir.x > 0 ? 1 : -1;
			maxUpdates = (width + updateLength - 1) >> logUpdateLength;
		} else {
			stepMult = 1.f / abs(dirLightPxDir.y);
			edgePxCoord = dirLightPxDir.y > 0 ? 0 : height - 1;
			signDir = dirLightPxDir.y > 0 ? 1 : -1;
			maxUpdates = (height + updateLength - 1) >> logUpdateLength;
		}
		dirLightPxDir *= stepMult;

		shadowUpdateCBData.LightPxDir = { dirLightPxDir.x, dirLightPxDir.y };

		// soft shadow angles
		float lenUV = float2{ dirLightDir.x, dirLightDir.y }.Length();
		float dirLightAngle = atan2(-dirLightDir.z, lenUV);
		float shadowSofteningRadiusAngle = RE::NI_PI / 180.f;
		float upperAngle = std::max(0.f, dirLightAngle - shadowSofteningRadiusAngle);
		float lowerAngle = std::min(RE::NI_HALF_PI - 1e-2f, dirLightAngle + shadowSofteningRadiusAngle);

		shadowUpdateCBData.LightDeltaZ = -(lenUV / invScale.z * stepMult) * float2{ std::tan(upperAngle), std::tan(lowerAngle) };
	}

	shadowUpdateCBData.PxSize = { 1.f / texHeightMap->desc.Width, 1.f / texHeightMap->desc.Height };
	shadowUpdateCBData.PosRange = { cachedHeightmap->pos0.z, cachedHeightmap->pos1.z };
	shadowUpdateCBData.ZRange = cachedHeightmap->zRange;
	shadowUpdateCBData.BlendWeight = a_refreshImmediately ? 1.0f : 0.5f;

	/* ---- BACKUP ---- */
	struct ShaderState
	{
		ID3D11ShaderResourceView* srvs[1] = { nullptr };
		ID3D11ComputeShader* shader = nullptr;
		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		ID3D11Buffer* buffer = nullptr;
	} old, newer;

	/* ---- DISPATCH ---- */

	newer.srvs[0] = texHeightMap->srv.get();
	newer.uavs[0] = texShadowHeight->uav.get();
	newer.buffer = shadowUpdateCB->CB();

	context->CSSetShaderResources(0, ARRAYSIZE(newer.srvs), newer.srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(newer.uavs), newer.uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &newer.buffer);
	context->CSSetShader(shadowUpdateProgram.get(), nullptr, 0);
	globals::profiler->BeginPass("TerrainShadows::ShadowUpdate");
	const uint updateCount = a_refreshImmediately ? maxUpdates : 1u;
	for (uint update = 0; update < updateCount; ++update) {
		shadowUpdateCBData.StartPxCoord = edgePxCoord + signDir * shadowUpdateIdx * updateLength;
		shadowUpdateCB->Update(shadowUpdateCBData);
		context->Dispatch(abs(shadowUpdateCBData.LightPxDir.x) >= abs(shadowUpdateCBData.LightPxDir.y) ? height : width, 1, 1);
		shadowUpdateIdx = (shadowUpdateIdx + 1) % maxUpdates;
	}
	globals::profiler->EndPass();

	/* ---- RESTORE ---- */
	context->CSSetShaderResources(0, ARRAYSIZE(old.srvs), old.srvs);
	context->CSSetShader(old.shader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(old.uavs), old.uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &old.buffer);
	return true;
}

void TerrainShadows::ReflectionsPrepass()
{
	if (texShadowHeight) {
		auto context = globals::d3d::context;

		std::array<ID3D11ShaderResourceView*, 1> srvs = { texShadowHeight->srv.get() };
		context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	}
}

void TerrainShadows::EarlyPrepass()
{
	if (TerrainHeightMap::GetSingleton()->LoadForCurrentWorldspace()) {
		shadowUpdateIdx = 0;
		needPrecompute = true;
	}

	const auto requestedRefreshGeneration = Util::GetCompletedCelestialTransitionGeneration();
	const bool timeJumpRefresh = requestedRefreshGeneration != handledTimeJumpRefreshGeneration;
	if (!settings.EnableTerrainShadow)
		return;

	const bool refreshImmediately = needPrecompute || timeJumpRefresh;
	if (needPrecompute)
		Precompute();

	if (UpdateShadow(refreshImmediately) && timeJumpRefresh)
		handledTimeJumpRefreshGeneration = requestedRefreshGeneration;

	if (texShadowHeight) {
		auto context = globals::d3d::context;

		std::array<ID3D11ShaderResourceView*, 1> srvs = { texShadowHeight->srv.get() };
		context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	}
}
