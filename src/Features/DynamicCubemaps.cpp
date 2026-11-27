#include "DynamicCubemaps.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <array>
#include <cassert>
#include <span>

#include "I18n/I18n.h"
#include "LinearLighting.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"

DynamicCubemaps::Settings DynamicCubemaps::GetCommonBufferData() const
{
	auto data = settings;
	globals::features::linearLighting.SRGBToWorking(&data.CubemapColor.x);
	return data;
}

#define I18N_KEY_PREFIX "feature.dynamic_cubemaps."

constexpr auto MIPLEVELS = 9;

// Set while an interior room is being compressed and written on its worker thread
static std::atomic<bool> interiorRoomExporting = false;
// Set by the menu; room captures start, take frames and save during the cubemap update, while each frame's depth and
// color are valid
static std::atomic<bool> interiorRoomCaptureRequested = false;
static std::atomic<bool> interiorRoomSaveRequested = false;
static std::atomic<bool> interiorRoomCancelRequested = false;

// Room captures save themselves after this many seconds of looking around
constexpr float kInteriorRoomCaptureSeconds = 18.0f;
// Frames seen after the player moves this far from where the capture started are skipped, in game units
constexpr float kInteriorRoomCaptureMaxMove = 16.0f;
// Room cubemap face size
constexpr uint32_t kInteriorRoomSize = 1024;
// The camera's path during a room capture, as yaw and pitch in degrees from the starting heading with pitch down
// positive: rows from the ceiling down to the floor, wide enough that the screen sweeps the room from wall to wall, with
// the first and last looking nearly straight up and down to reach the ceiling and floor along the wall behind
constexpr std::array<std::array<float, 2>, 14> kInteriorRoomSweep = { { { 0.0f, 0.0f }, { -60.0f, -85.0f }, { 60.0f, -85.0f }, { 80.0f, -55.0f }, { -80.0f, -55.0f }, { -80.0f, -18.0f }, { 80.0f, -18.0f }, { 80.0f, 20.0f }, { -80.0f, 20.0f }, { -80.0f, 60.0f }, { 80.0f, 60.0f }, { 60.0f, 85.0f }, { -60.0f, 85.0f }, { 0.0f, 0.0f } } };

// Room cubemap depth is the distance from the capture point in game units, stored as log(distance / min) / log(max / min)
// scaled to below 1 so rooms without depth (opaque alpha) can be told apart; matches PBR::GetInteriorCubeDistance
constexpr float kInteriorRoomMinDistance = 16.0f;
constexpr float kInteriorRoomDistanceRange = 512.0f;
constexpr float kInteriorRoomDepthAlphaScale = 0.98f;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DynamicCubemaps::Settings,
	EnabledSSR,
	EnabledCreator);

std::vector<std::pair<std::string_view, std::string_view>> DynamicCubemaps::GetShaderDefineOptions()
{
	std::vector<std::pair<std::string_view, std::string_view>> result;
	if (settings.EnabledSSR) {
		result.push_back({ "ENABLESSR", "" });
	}

	return result;
}

void DynamicCubemaps::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("screen_space_reflections"), "Screen Space Reflections"), ImGuiTreeNodeFlags_DefaultOpen)) {
		recompileFlag |= ImGui::Checkbox(T(TKEY("enable_ssr"), "Enable Screen Space Reflections"), reinterpret_cast<bool*>(&settings.EnabledSSR));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_ssr_tooltip"), "Enable Screen Space Reflections on Water"));
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("interior_room_export"), "Interior Room Export"))) {
		ImGui::TextWrapped("%s", T(TKEY("interior_room_export_info"), "Captures a room cubemap with depth for True PBR interior mapping, in Data\\textures\\interiors. Windows look into the room rebuilt around where you stand, so stand in first person where a window would be, against one wall, facing across the room to the wall that should become the back wall. Start the capture and close the menu: the camera looks around the room by itself, from the ceiling down to the floor, and turns back when it saves. Hold still and leave the mouse alone meanwhile. Everything the camera sees is saved at full resolution; the rest comes from the dynamic cubemap."));
		if (interiorRoomCapture) {
			const float remaining = std::max(kInteriorRoomCaptureSeconds - (globals::state->timer - interiorRoomCapture->startTime), 0.0f);
			ImGui::Text(T(TKEY("interior_room_capturing"), "Capturing: the camera is looking around, saving in %.0f s"), remaining);
			if (ImGui::Button(T(TKEY("save_interior_room"), "Save Now")))
				interiorRoomSaveRequested = true;
			ImGui::SameLine();
			if (ImGui::Button(T(TKEY("cancel_interior_room"), "Cancel")))
				interiorRoomCancelRequested = true;
		} else {
			auto _disabled = Util::DisableGuard(interiorRoomExporting || interiorRoomCaptureRequested || !cubemapValid[0]);
			if (ImGui::Button(T(TKEY("capture_interior_room"), "Capture Interior Room"))) {
				interiorRoomCaptureRequested = true;
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("capture_interior_room_tooltip"), "Writes a room cubemap (sRGB BC7, distance in alpha) named after the current cell. Put it in the environment map slot (TX04) of a window's texture set, for example with PGPatcher's slot5: cubemaps from textures\\interiors are used as rooms without further setup."));
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("dynamic_cubemap_creator"), "Dynamic Cubemap Creator"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("%s", T(TKEY("creator_info"), "You must enable creator mode by adding the shader define CREATOR"));
		ImGui::Checkbox(T(TKEY("enable_creator"), "Enable Creator"), reinterpret_cast<bool*>(&settings.EnabledCreator));
		if (settings.EnabledCreator) {
			ImGui::ColorEdit3(T(TKEY("color"), "Color"), reinterpret_cast<float*>(&settings.CubemapColor));
			ImGui::SliderFloat(T(TKEY("roughness"), "Roughness"), &settings.CubemapColor.w, 0.0f, 1.0f, "%.2f");
			if (ImGui::Button(T(TKEY("export"), "Export"))) {
				auto device = globals::d3d::device;
				auto context = globals::d3d::context;

				D3D11_TEXTURE2D_DESC texDesc{};
				texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				texDesc.Height = 1;
				texDesc.Width = 1;
				texDesc.ArraySize = 6;
				texDesc.MipLevels = 1;
				texDesc.SampleDesc.Count = 1;
				texDesc.Usage = D3D11_USAGE_DEFAULT;
				texDesc.BindFlags = 0;
				texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

				D3D11_SUBRESOURCE_DATA subresourceData[6];

				struct PixelData
				{
					uint8_t r, g, b, a;
				};

				static PixelData colorPixel{};

				colorPixel = { (uint8_t)((settings.CubemapColor.x * 255.0f) + 0.5f),
					(uint8_t)((settings.CubemapColor.y * 255.0f) + 0.5f),
					(uint8_t)((settings.CubemapColor.z * 255.0f) + 0.5f),
					std::min((uint8_t)254u, (uint8_t)((settings.CubemapColor.w * 255.0f) + 0.5f)) };

				static PixelData emptyPixel{};

				subresourceData[0].pSysMem = &colorPixel;
				subresourceData[0].SysMemPitch = sizeof(PixelData);
				subresourceData[0].SysMemSlicePitch = sizeof(PixelData);

				for (uint i = 1; i < 6; i++) {
					subresourceData[i].pSysMem = &emptyPixel;
					subresourceData[i].SysMemPitch = sizeof(PixelData);
					subresourceData[i].SysMemSlicePitch = sizeof(PixelData);
				}

				winrt::com_ptr<ID3D11Texture2D> tempTexture;
				DirectX::ScratchImage image;

				try {
					DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, subresourceData, tempTexture.put()));
					DX::ThrowIfFailed(CaptureTexture(device, context, tempTexture.get(), image));

					if (std::filesystem::create_directories(defaultDynamicCubeMapSavePath)) {
						logger::info("Missing DynamicCubeMap Creator directory created: {}", defaultDynamicCubeMapSavePath);
					}

					std::filesystem::path DynamicCubeMapSavePath = defaultDynamicCubeMapSavePath;
					std::filesystem::path filename(std::format("R{:03d}G{:03d}B{:03d}A{:03d}.dds", colorPixel.r, colorPixel.g, colorPixel.b, colorPixel.a));
					DynamicCubeMapSavePath /= filename;

					if (std::filesystem::exists(DynamicCubeMapSavePath)) {
						logger::info("DynamicCubeMap Creator file for {} already exists, skipping.", filename.string());
					} else {
						DX::ThrowIfFailed(SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS::DDS_FLAGS_NONE, DynamicCubeMapSavePath.c_str()));
						logger::info("DynamicCubeMap Creator file for {} written", filename.string());
					}

				} catch (const std::exception& e) {
					logger::error("Failed in DynamicCubeMap Creator file: {} {}", defaultDynamicCubeMapSavePath, e.what());
				}

				image.Release();
			}
		}
		ImGui::TreePop();
	}
}

void DynamicCubemaps::LoadSettings(json& o_json)
{
	settings = o_json;
	recompileFlag = true;
}

void DynamicCubemaps::SaveSettings(json& o_json)
{
	o_json = settings;
}

void DynamicCubemaps::RestoreDefaultSettings()
{
	settings = {};
	recompileFlag = true;
}

void DynamicCubemaps::DataLoaded()
{
	MenuOpenCloseEventHandler::Register();
}

void DynamicCubemaps::PostPostLoad()
{
}

RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// When entering a new cell, reset the capture
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		if (!a_event->opening) {
			auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			dynamicCubemaps.resetCapture[0] = true;
			dynamicCubemaps.resetCapture[1] = true;
		}
	}
	return RE::BSEventNotifyControl::kContinue;
}

bool MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto ui = globals::game::ui;

	if (!ui) {
		logger::error("UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

	logger::info("Registered {}", typeid(singleton).name());

	return true;
}

void DynamicCubemaps::ClearShaderCache()
{
	resetCapture[0] = resetCapture[1] = true;
	cubemapValid[0] = cubemapValid[1] = false;
	nextTask = NextTask::kCaptureInferAndIrradianceA;
	if (detectCaptureLightingCS) {
		detectCaptureLightingCS->Release();
		detectCaptureLightingCS = nullptr;
	}
	if (updateCubemapCS) {
		updateCubemapCS->Release();
		updateCubemapCS = nullptr;
	}
	if (updateCubemapReflectionsCS) {
		updateCubemapReflectionsCS->Release();
		updateCubemapReflectionsCS = nullptr;
	}
	if (updateCubemapFakeReflectionsCS) {
		updateCubemapFakeReflectionsCS->Release();
		updateCubemapFakeReflectionsCS = nullptr;
	}
	if (inferCubemapCS) {
		inferCubemapCS->Release();
		inferCubemapCS = nullptr;
	}
	if (inferCubemapReflectionsCS) {
		inferCubemapReflectionsCS->Release();
		inferCubemapReflectionsCS = nullptr;
	}
	if (inferCubemapFakeReflectionsCS) {
		inferCubemapFakeReflectionsCS->Release();
		inferCubemapFakeReflectionsCS = nullptr;
	}
	if (specularIrradianceCS) {
		specularIrradianceCS->Release();
		specularIrradianceCS = nullptr;
	}
	if (bc6hEncodeCS) {
		bc6hEncodeCS->Release();
		bc6hEncodeCS = nullptr;
	}
	if (exportInteriorRoomCS) {
		exportInteriorRoomCS->Release();
		exportInteriorRoomCS = nullptr;
	}
	if (accumulateInteriorRoomCS) {
		accumulateInteriorRoomCS->Release();
		accumulateInteriorRoomCS = nullptr;
	}
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderDetectLighting()
{
	if (!detectCaptureLightingCS) {
		logger::debug("Compiling DetectCaptureLightingCS");
		detectCaptureLightingCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\DetectCaptureLightingCS.hlsl", {}, "cs_5_0"));
	}
	return detectCaptureLightingCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderUpdate()
{
	if (!updateCubemapCS) {
		logger::debug("Compiling UpdateCubemapCS");
		updateCubemapCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\UpdateCubemapCS.hlsl", {}, "cs_5_0"));
	}
	return updateCubemapCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderUpdateReflections()
{
	if (!updateCubemapReflectionsCS) {
		logger::debug("Compiling UpdateCubemapCS REFLECTIONS");
		updateCubemapReflectionsCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\UpdateCubemapCS.hlsl", { { "REFLECTIONS", "" } }, "cs_5_0"));
	}
	return updateCubemapReflectionsCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderUpdateFakeReflections()
{
	if (!updateCubemapFakeReflectionsCS) {
		logger::debug("Compiling UpdateCubemapCS FAKEREFLECTIONS");
		updateCubemapFakeReflectionsCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\UpdateCubemapCS.hlsl", { { "FAKEREFLECTIONS", "" } }, "cs_5_0"));
	}
	return updateCubemapFakeReflectionsCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderInferrence()
{
	if (!inferCubemapCS) {
		logger::debug("Compiling InferCubemapCS");
		inferCubemapCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\InferCubemapCS.hlsl", {}, "cs_5_0"));
	}
	return inferCubemapCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderInferrenceReflections()
{
	if (!inferCubemapReflectionsCS) {
		logger::debug("Compiling InferCubemapCS REFLECTIONS");
		inferCubemapReflectionsCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\InferCubemapCS.hlsl", { { "REFLECTIONS", "" } }, "cs_5_0"));
	}
	return inferCubemapReflectionsCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderInferrenceFakeReflections()
{
	if (!inferCubemapFakeReflectionsCS) {
		logger::debug("Compiling InferCubemapCS FAKEREFLECTIONS");
		inferCubemapFakeReflectionsCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\InferCubemapCS.hlsl", { { "FAKEREFLECTIONS", "" } }, "cs_5_0"));
	}
	return inferCubemapFakeReflectionsCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderSpecularIrradiance()
{
	if (!specularIrradianceCS) {
		logger::debug("Compiling SpecularIrradianceCS");
		specularIrradianceCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\SpecularIrradianceCS.hlsl", {}, "cs_5_0"));
	}
	return specularIrradianceCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderBC6HEncode()
{
	if (!bc6hEncodeCS) {
		logger::debug("Compiling BC6HEncodeCS");
		bc6hEncodeCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\BC6HEncodeCS.hlsl", {}, "cs_5_0"));
	}
	return bc6hEncodeCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderExportInteriorRoom()
{
	if (!exportInteriorRoomCS) {
		logger::debug("Compiling ExportInteriorRoomCS");
		exportInteriorRoomCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\ExportInteriorRoomCS.hlsl", {}, "cs_5_0"));
	}
	return exportInteriorRoomCS;
}

ID3D11ComputeShader* DynamicCubemaps::GetComputeShaderAccumulateInteriorRoom()
{
	if (!accumulateInteriorRoomCS) {
		logger::debug("Compiling ExportInteriorRoomCS ACCUMULATE");
		accumulateInteriorRoomCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DynamicCubemaps\\ExportInteriorRoomCS.hlsl", { { "ACCUMULATE", "" } }, "cs_5_0"));
	}
	return accumulateInteriorRoomCS;
}

/** Shows a notification on the HUD from any thread, so room captures can be followed with the menu closed. */
static void ShowInteriorRoomMessage(std::string a_message)
{
	if (auto* tasks = SKSE::GetTaskInterface())
		tasks->AddTask([message = std::move(a_message)]() { RE::SendHUDMessage::ShowHUDMessage(message.c_str()); });
}

/**
 * Fills the pixels of a square RGBA image whose alpha is zero from their surroundings, colour and alpha alike: each
 * level of a pyramid averages the filled pixels below it, then holes take the next level's value, bilinearly, from the
 * coarsest level down, so gaps blend in from their edges.
 * @return False when no pixel is filled
 */
static bool FillInteriorRoomHoles(float* a_pixels, size_t a_size)
{
	struct Level
	{
		size_t size;
		std::vector<DirectX::XMFLOAT4> color;
		std::vector<float> weight;
	};
	std::vector<Level> levels;
	levels.push_back({ a_size, std::vector<DirectX::XMFLOAT4>(a_size * a_size), std::vector<float>(a_size * a_size) });
	for (size_t i = 0; i < a_size * a_size; ++i) {
		levels[0].color[i] = { a_pixels[i * 4], a_pixels[i * 4 + 1], a_pixels[i * 4 + 2], a_pixels[i * 4 + 3] };
		levels[0].weight[i] = a_pixels[i * 4 + 3] > 0.0f ? 1.0f : 0.0f;
	}

	while (levels.back().size > 1) {
		const Level& fine = levels.back();
		Level coarse{ fine.size / 2, std::vector<DirectX::XMFLOAT4>(fine.size * fine.size / 4), std::vector<float>(fine.size * fine.size / 4) };
		for (size_t y = 0; y < coarse.size; ++y) {
			for (size_t x = 0; x < coarse.size; ++x) {
				DirectX::XMFLOAT4 sum{};
				float weight = 0.0f;
				for (size_t child = 0; child < 4; ++child) {
					const size_t index = (y * 2 + child / 2) * fine.size + x * 2 + child % 2;
					const float w = fine.weight[index];
					sum = { sum.x + fine.color[index].x * w, sum.y + fine.color[index].y * w, sum.z + fine.color[index].z * w, sum.w + fine.color[index].w * w };
					weight += w;
				}
				const size_t index = y * coarse.size + x;
				if (weight > 0.0f)
					coarse.color[index] = { sum.x / weight, sum.y / weight, sum.z / weight, sum.w / weight };
				coarse.weight[index] = std::min(weight, 1.0f);
			}
		}
		levels.push_back(std::move(coarse));
	}
	if (levels.back().weight[0] <= 0.0f)
		return false;

	for (size_t level = levels.size() - 1; level-- > 0;) {
		Level& fine = levels[level];
		const Level& coarse = levels[level + 1];
		for (size_t y = 0; y < fine.size; ++y) {
			for (size_t x = 0; x < fine.size; ++x) {
				const size_t index = y * fine.size + x;
				const float w = fine.weight[index];
				if (w >= 1.0f)
					continue;
				// Bilinear from the coarser level, whose texel centres sit between every other fine texel
				const float cx = std::clamp((x + 0.5f) * 0.5f - 0.5f, 0.0f, static_cast<float>(coarse.size - 1));
				const float cy = std::clamp((y + 0.5f) * 0.5f - 0.5f, 0.0f, static_cast<float>(coarse.size - 1));
				const size_t x0 = static_cast<size_t>(cx), y0 = static_cast<size_t>(cy);
				const size_t x1 = std::min(x0 + 1, coarse.size - 1), y1 = std::min(y0 + 1, coarse.size - 1);
				const float fx = cx - x0, fy = cy - y0;
				const auto& c00 = coarse.color[y0 * coarse.size + x0];
				const auto& c10 = coarse.color[y0 * coarse.size + x1];
				const auto& c01 = coarse.color[y1 * coarse.size + x0];
				const auto& c11 = coarse.color[y1 * coarse.size + x1];
				auto lerp2 = [&](float a, float b, float c, float d) { return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy; };
				const DirectX::XMFLOAT4 filled = { lerp2(c00.x, c10.x, c01.x, c11.x), lerp2(c00.y, c10.y, c01.y, c11.y), lerp2(c00.z, c10.z, c01.z, c11.z), lerp2(c00.w, c10.w, c01.w, c11.w) };
				auto& color = fine.color[index];
				color = { color.x * w + filled.x * (1 - w), color.y * w + filled.y * (1 - w), color.z * w + filled.z * (1 - w), color.w * w + filled.w * (1 - w) };
				fine.weight[index] = 1.0f;
			}
		}
	}

	for (size_t i = 0; i < a_size * a_size; ++i) {
		if (a_pixels[i * 4 + 3] <= 0.0f) {
			const auto& color = levels[0].color[i];
			a_pixels[i * 4] = color.x;
			a_pixels[i * 4 + 1] = color.y;
			a_pixels[i * 4 + 2] = color.z;
			a_pixels[i * 4 + 3] = color.w;
		}
	}
	return true;
}

/** Room direction of a cube texel's centre (x right, y up, z towards the back wall), as D3D lays out cube faces. */
static DirectX::XMFLOAT3 GetInteriorRoomDirection(size_t a_face, size_t a_x, size_t a_y, size_t a_size)
{
	const float u = (a_x + 0.5f) / a_size * 2.0f - 1.0f;
	const float v = (a_y + 0.5f) / a_size * 2.0f - 1.0f;
	constexpr std::array<std::array<float, 9>, 6> axes = { {
		{ 0, 0, 1, 0, -1, 0, -1, 0, 0 },   // +X: (1, -v, -u)
		{ 0, 0, -1, 0, -1, 0, 1, 0, 0 },   // -X: (-1, -v, u)
		{ 1, 0, 0, 0, 0, 1, 0, 1, 0 },     // +Y: (u, 1, v)
		{ 1, 0, 0, 0, 0, -1, 0, -1, 0 },   // -Y: (u, -1, -v)
		{ 1, 0, 0, 0, -1, 0, 0, 0, 1 },    // +Z: (u, -v, 1)
		{ -1, 0, 0, 0, -1, 0, 0, 0, -1 },  // -Z: (-u, -v, -1)
	} };
	const auto& a = axes[a_face];
	const DirectX::XMFLOAT3 direction = { a[0] * u + a[3] * v + a[6], a[1] * u + a[4] * v + a[7], a[2] * u + a[5] * v + a[8] };
	const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
	return { direction.x / length, direction.y / length, direction.z / length };
}

/**
 * Distances from the capture point to the room's walls, floor and ceiling in cube face order, from each texel's
 * distance in alpha: for each face, how far along its axis the farthest surfaces near its middle lie, as walls lie
 * beyond the furniture before them and, down a long room, beyond the floor and ceiling that fill most of the view.
 */
static std::array<float, 6> MeasureInteriorRoom(const float* a_pixels, size_t a_size)
{
	// Texels within about 30 degrees of the face's axis
	constexpr float kCentre = 0.577f;
	// Past this, texels see out through windows rather than a wall
	constexpr float kOutside = 0.9f * kInteriorRoomMinDistance * kInteriorRoomDistanceRange;
	std::array<float, 6> extent{};
	std::vector<float> distances;
	for (size_t face = 0; face < 6; ++face) {
		distances.clear();
		for (size_t y = 0; y < a_size; ++y) {
			for (size_t x = 0; x < a_size; ++x) {
				const float u = (x + 0.5f) / a_size * 2.0f - 1.0f;
				const float v = (y + 0.5f) / a_size * 2.0f - 1.0f;
				if (std::abs(u) > kCentre || std::abs(v) > kCentre)
					continue;
				const float distance = a_pixels[((face * a_size + y) * a_size + x) * 4 + 3];
				if (distance >= kOutside)
					continue;
				const auto direction = GetInteriorRoomDirection(face, x, y, a_size);
				const float axis = std::abs(face < 2 ? direction.x : face < 4 ? direction.y :
																				direction.z);
				distances.push_back(distance * axis);
			}
		}
		if (distances.empty()) {
			extent[face] = kOutside;
			continue;
		}
		const auto percentile = distances.begin() + distances.size() * 9 / 10;
		std::nth_element(distances.begin(), percentile, distances.end());
		extent[face] = std::max(*percentile, 1.0f);
	}
	return extent;
}

// Wall distances in a capture's sidecar, in cube face order
constexpr std::array<const char*, 6> kInteriorRoomExtentNames = { "right", "left", "up", "down", "back", "front" };

/** Scales a room cube's color, encodes its distances into alpha and builds its mip chain in 8-bit sRGB for the encoder. */
static DirectX::ScratchImage EncodeInteriorRoomCube(DirectX::ScratchImage& a_cube, float a_colorScale)
{
	auto* pixels = reinterpret_cast<float*>(a_cube.GetPixels());
	const size_t count = a_cube.GetPixelsSize() / (4 * sizeof(float));
	for (size_t i = 0; i < count; ++i) {
		for (size_t channel = 0; channel < 3; ++channel)
			pixels[i * 4 + channel] = std::min(pixels[i * 4 + channel] * a_colorScale, 1.0f);
		// Distance at the room's real scale, so interior mapping can rebuild the room around the capture point
		const float ratio = std::clamp(pixels[i * 4 + 3] / kInteriorRoomMinDistance, 1.0f, kInteriorRoomDistanceRange);
		pixels[i * 4 + 3] = kInteriorRoomDepthAlphaScale * std::log(ratio) / std::log(kInteriorRoomDistanceRange);
	}
	DirectX::ScratchImage mips;
	DX::ThrowIfFailed(DirectX::GenerateMipMaps(a_cube.GetImages(), a_cube.GetImageCount(), a_cube.GetMetadata(), DirectX::TEX_FILTER_DEFAULT, 0, mips));
	DirectX::ScratchImage encoded;
	DX::ThrowIfFailed(DirectX::Convert(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, encoded));
	return encoded;
}

/**
 * Where a room captured in an interior lies in the exterior world, found through a door of the cell that leads
 * outside: the interior is turned and moved so that door meets the exterior door it links to.
 * @return The worldspace, capture point and heading outside, or null when no door of the cell leads outside
 */
static json GetInteriorRoomPlacement(RE::TESObjectCELL* a_cell, const float3& a_position, float a_heading)
{
	json placement;
	a_cell->ForEachReference([&](RE::TESObjectREFR* a_door) {
		const auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
		const auto outsideDoor = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
		const auto* worldspace = outsideDoor ? outsideDoor->GetWorldspace() : nullptr;
		const auto* back = outsideDoor ? outsideDoor->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
		const auto* file = worldspace ? worldspace->GetFile(0) : nullptr;
		if (!file || !back || !back->teleportData)
			return RE::BSContainer::ForEachResult::kContinue;

		// Leaving, the player faces away from the building at the exterior marker; entering, along the interior marker
		const float turn = teleport->teleportData->rotation.z + RE::NI_PI - back->teleportData->rotation.z;
		const auto inside = a_door->GetPosition();
		const auto outside = outsideDoor->GetPosition();
		const float dx = a_position.x - inside.x;
		const float dy = a_position.y - inside.y;
		// Headings turn clockwise from +Y
		const float c = std::cos(turn);
		const float s = std::sin(turn);
		placement["worldspace"] = { { "plugin", std::string(file->GetFilename()) }, { "formID", worldspace->GetLocalFormID() } };
		placement["position"] = { outside.x + dx * c + dy * s, outside.y - dx * s + dy * c, outside.z + a_position.z - inside.z };
		placement["heading"] = a_heading + turn;
		return RE::BSContainer::ForEachResult::kStop;
	});
	return placement;
}

/** Turns the player's first-person view on the game thread, as heading and pitch in radians with pitch down positive. */
static void SetInteriorRoomView(float a_heading, float a_pitch)
{
	if (auto* tasks = SKSE::GetTaskInterface()) {
		tasks->AddTask([a_heading, a_pitch]() {
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				player->SetHeading(a_heading);
				player->SetLooking(a_pitch);
			}
		});
	}
}

/** Yaw and pitch in radians along the room capture's camera path, at a share of the way through it, at even speed. */
static std::array<float, 2> GetInteriorRoomSweepAngles(float a_progress)
{
	std::array<float, kInteriorRoomSweep.size() - 1> lengths{};
	float total = 0.0f;
	for (size_t i = 0; i < lengths.size(); ++i) {
		lengths[i] = std::hypot(kInteriorRoomSweep[i + 1][0] - kInteriorRoomSweep[i][0], kInteriorRoomSweep[i + 1][1] - kInteriorRoomSweep[i][1]);
		total += lengths[i];
	}
	float along = std::clamp(a_progress, 0.0f, 1.0f) * total;
	for (size_t i = 0; i < lengths.size(); ++i) {
		if (along <= lengths[i] || i + 1 == lengths.size()) {
			const float t = lengths[i] > 0.0f ? std::min(along / lengths[i], 1.0f) : 1.0f;
			const auto& from = kInteriorRoomSweep[i];
			const auto& to = kInteriorRoomSweep[i + 1];
			return { RE::deg_to_rad(from[0] + (to[0] - from[0]) * t), RE::deg_to_rad(from[1] + (to[1] - from[1]) * t) };
		}
		along -= lengths[i];
	}
	return {};
}

/** Room capture constants for a capture started facing a heading, with the given offset between cameras. */
static DynamicCubemaps::ExportInteriorRoomCB GetInteriorRoomConstants(float a_heading, const float3& a_offset)
{
	// The heading becomes the back wall; Skyrim's heading is clockwise from +Y with +Z up
	DynamicCubemaps::ExportInteriorRoomCB data{};
	data.RoomForward = { std::sin(a_heading), std::cos(a_heading), 0.0f, 0.0f };
	data.RoomRight = { std::cos(a_heading), -std::sin(a_heading), 0.0f, 0.0f };
	data.RoomUp = { 0.0f, 0.0f, 1.0f, 0.0f };
	data.CaptureOffset = { a_offset.x, a_offset.y, a_offset.z, 0.0f };
	return data;
}

/** Runs one pass of ExportInteriorRoomCS over the room cubemap's faces, then unbinds everything it bound. */
static void DispatchInteriorRoomPass(ID3D11ComputeShader* a_shader, std::span<ID3D11ShaderResourceView* const> a_srvs, std::span<ID3D11UnorderedAccessView* const> a_uavs, ID3D11Buffer* a_buffer, ID3D11SamplerState* a_sampler)
{
	auto context = globals::d3d::context;
	context->CSSetShaderResources(0, static_cast<UINT>(a_srvs.size()), a_srvs.data());
	context->CSSetUnorderedAccessViews(0, static_cast<UINT>(a_uavs.size()), a_uavs.data(), nullptr);
	context->CSSetConstantBuffers(0, 1, &a_buffer);
	context->CSSetSamplers(0, 1, &a_sampler);
	context->CSSetShader(a_shader, nullptr, 0);
	context->Dispatch((kInteriorRoomSize + 7) / 8, (kInteriorRoomSize + 7) / 8, 6);

	std::array<ID3D11ShaderResourceView*, 8> nullSrvs{};
	std::array<ID3D11UnorderedAccessView*, 8> nullUavs{};
	ID3D11Buffer* nullBuffer = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	context->CSSetShaderResources(0, static_cast<UINT>(a_srvs.size()), nullSrvs.data());
	context->CSSetUnorderedAccessViews(0, static_cast<UINT>(a_uavs.size()), nullUavs.data(), nullptr);
	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	context->CSSetSamplers(0, 1, &nullSampler);
	context->CSSetShader(nullptr, nullptr, 0);
}

void DynamicCubemaps::StartInteriorRoomCapture()
{
	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player || !player->GetParentCell() || !cubemapValid[0]) {
		logger::warn("[DynamicCubemaps] No settled capture to start an interior room from");
		return;
	}
	// The capture turns the player's own view, which only the first-person camera follows directly
	if (auto* camera = RE::PlayerCamera::GetSingleton(); !camera || !camera->IsInFirstPerson()) {
		ShowInteriorRoomMessage(T(TKEY("interior_room_capture_first_person"), "Room capture needs first person"));
		return;
	}
	// Recompiled for every capture so an updated shader applies without a restart; the disk cache keeps unchanged ones cheap
	for (auto* shader : { &exportInteriorRoomCS, &accumulateInteriorRoomCS }) {
		if (*shader) {
			(*shader)->Release();
			*shader = nullptr;
		}
	}
	if (!GetComputeShaderExportInteriorRoom() || !GetComputeShaderAccumulateInteriorRoom()) {
		logger::error("[DynamicCubemaps] ExportInteriorRoomCS failed to compile; interior room capture disabled");
		return;
	}

	auto capture = std::make_unique<InteriorRoomCapture>();
	try {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = kInteriorRoomSize;
		texDesc.Height = kInteriorRoomSize;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 6;
		texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = 6;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.ArraySize = 6;

		srvDesc.Format = uavDesc.Format = texDesc.Format;
		capture->color = std::make_unique<Texture2D>(texDesc, "DynamicCubemaps::InteriorRoomCaptureColor");
		capture->color->CreateSRV(srvDesc);
		capture->color->CreateUAV(uavDesc);

		// Read back while accumulating, which typed UAVs only allow for single 32-bit channels
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		capture->weight = std::make_unique<Texture2D>(texDesc, "DynamicCubemaps::InteriorRoomCaptureWeight");
		capture->weight->CreateSRV(srvDesc);
		capture->weight->CreateUAV(uavDesc);

		capture->cb = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ExportInteriorRoomCB>(), "DynamicCubemaps::InteriorRoomCaptureCB");
	} catch (const std::exception& e) {
		logger::error("[DynamicCubemaps] Failed to start interior room capture: {}", e.what());
		return;
	}
	constexpr float zero[4] = {};
	globals::d3d::context->ClearUnorderedAccessViewFloat(capture->color->uav.get(), zero);
	globals::d3d::context->ClearUnorderedAccessViewFloat(capture->weight->uav.get(), zero);

	capture->heading = player->GetAngleZ();
	capture->pitch = player->GetAngleX();
	const auto eyePosition = Util::GetEyePosition();
	capture->eyePosition = { eyePosition.x, eyePosition.y, eyePosition.z };
	capture->startTime = globals::state->timer;
	interiorRoomCapture = std::move(capture);
	interiorRoomSaveRequested = false;
	interiorRoomCancelRequested = false;

	ShowInteriorRoomMessage(T(TKEY("interior_room_capture_started"), "Room capture: hold still while the camera looks around"));
}

void DynamicCubemaps::AccumulateInteriorRoom()
{
	auto& capture = *interiorRoomCapture;
	const auto eyePosition = Util::GetEyePosition();
	const float3 offset = { eyePosition.x - capture.eyePosition.x, eyePosition.y - capture.eyePosition.y, eyePosition.z - capture.eyePosition.z };
	if (offset.Length() > kInteriorRoomCaptureMaxMove)
		return;

	capture.cb->Update(GetInteriorRoomConstants(capture.heading, offset));

	auto renderer = globals::game::renderer;
	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	const std::array<ID3D11ShaderResourceView*, 2> srvs = { depth.depthSRV, main.SRV };
	const std::array<ID3D11UnorderedAccessView*, 2> uavs = { capture.color->uav.get(), capture.weight->uav.get() };
	DispatchInteriorRoomPass(accumulateInteriorRoomCS, srvs, uavs, capture.cb->CB(), computeSampler);
}

void DynamicCubemaps::ExportInteriorRoom()
{
	auto capture = std::move(interiorRoomCapture);
	auto player = RE::PlayerCharacter::GetSingleton();
	auto cell = player ? player->GetParentCell() : nullptr;
	if (!capture || !cell) {
		logger::warn("[DynamicCubemaps] No interior room capture to save");
		return;
	}

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	// A cube texture, so the capture reads back and saves as a cubemap
	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = kInteriorRoomSize;
	texDesc.Height = kInteriorRoomSize;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 6;
	texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	uavDesc.Texture2DArray.ArraySize = 6;

	winrt::com_ptr<ID3D11Texture2D> roomTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> roomUAV;
	DirectX::ScratchImage image;

	try {
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, roomTexture.put()));
		Util::SetResourceName(roomTexture.get(), "DynamicCubemaps::InteriorRoomExport");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(roomTexture.get(), &uavDesc, roomUAV.put()));
		Util::SetResourceName(roomUAV.get(), "DynamicCubemaps::InteriorRoomExport UAV");

		// Captured positions are relative to the camera at the last cubemap capture, and the room to where it started
		capture->cb->Update(GetInteriorRoomConstants(capture->heading, capture->eyePosition - cameraPreviousPosAdjust[0]));

		const std::array<ID3D11ShaderResourceView*, 5> srvs = { envTexture->srv.get(), envCapturePositionTexture->srv.get(), envCaptureRawTexture->srv.get(), capture->color->srv.get(), capture->weight->srv.get() };
		const std::array<ID3D11UnorderedAccessView*, 1> uavs = { roomUAV.get() };
		DispatchInteriorRoomPass(exportInteriorRoomCS, srvs, uavs, capture->cb->CB(), computeSampler);

		DX::ThrowIfFailed(CaptureTexture(device, context, roomTexture.get(), image));
	} catch (const std::exception& e) {
		logger::error("[DynamicCubemaps] Failed to capture interior room: {}", e.what());
		return;
	}

	std::filesystem::path savePath = interiorRoomSavePath;
	const auto* editorID = cell->GetFormEditorID();
	const std::string cellName = editorID && editorID[0] ? editorID : std::format("{:08X}", cell->GetFormID());
	for (uint32_t index = 0;; ++index) {
		auto candidate = savePath / std::format("{}_{}.dds", cellName, index);
		if (!std::filesystem::exists(candidate)) {
			savePath = std::move(candidate);
			break;
		}
	}

	// Where the room lies outside, read while the cell's doors are loaded
	json record = GetInteriorRoomPlacement(cell, capture->eyePosition, capture->heading);
	if (record.empty())
		logger::info("[DynamicCubemaps] No door of {} leads outside, so its room is only shown by materials that use it", cellName);
	record["cell"] = cellName;
	record["interior"] = { { "position", { capture->eyePosition.x, capture->eyePosition.y, capture->eyePosition.z } }, { "heading", capture->heading } };

	// Filling, measuring and mips run on the CPU off the render thread; FinishInteriorRoomExport then compresses on the GPU
	interiorRoomExporting = true;
	ShowInteriorRoomMessage(T(TKEY("interior_room_capture_saving"), "Room capture: saving"));
	const std::string fileName = savePath.filename().string();
	interiorRoomSavedMessage = std::vformat(T(TKEY("interior_room_capture_saved"), "Room capture saved as {}"), std::make_format_args(fileName));
	interiorRoomFailedMessage = T(TKEY("interior_room_capture_failed"), "Room capture failed; see CommunityShaders.log");
	interiorRoomExport = std::async(std::launch::async, [image = std::move(image), savePath, record = std::move(record)]() mutable {
		// Captured as 32-bit float already, which the processing below reads directly
		DirectX::ScratchImage linear = std::move(image);
		auto* pixels = reinterpret_cast<float*>(linear.GetPixels());
		const size_t pixelCount = linear.GetPixelsSize() / (4 * sizeof(float));

		// Texels nothing saw, such as straight down at the player's feet, have no distance and only a guessed color;
		// they are filled per face, and faces nothing saw at all take the room's average
		const size_t faceSize = linear.GetMetadata().width;
		const size_t facePixels = faceSize * faceSize;
		std::vector<size_t> emptyFaces;
		for (size_t face = 0; face < pixelCount / facePixels; ++face) {
			if (!FillInteriorRoomHoles(pixels + face * facePixels * 4, faceSize))
				emptyFaces.push_back(face);
		}
		if (emptyFaces.size() * facePixels == pixelCount)
			throw std::runtime_error("no depth was captured");
		if (!emptyFaces.empty()) {
			std::array<double, 4> average{};
			for (size_t i = 0; i < pixelCount; ++i) {
				if (pixels[i * 4 + 3] > 0.0f) {
					for (size_t channel = 0; channel < 4; ++channel)
						average[channel] += pixels[i * 4 + channel];
				}
			}
			const double filled = static_cast<double>(pixelCount - emptyFaces.size() * facePixels);
			for (size_t face : emptyFaces) {
				for (size_t i = face * facePixels; i < (face + 1) * facePixels; ++i) {
					for (size_t channel = 0; channel < 4; ++channel)
						pixels[i * 4 + channel] = static_cast<float>(average[channel] / filled);
				}
			}
		}

		// Scale the HDR capture to an albedo-like average: room images are lit by the window's emittance and by
		// daylight rather than by their own brightness, and emission is normalized by the room's average anyway
		double luminance = 0.0;
		for (size_t i = 0; i < pixelCount; ++i) {
			luminance += 0.2126 * pixels[i * 4] + 0.7152 * pixels[i * 4 + 1] + 0.0722 * pixels[i * 4 + 2];
		}
		const float scale = 0.3f / static_cast<float>(std::max(luminance / static_cast<double>(pixelCount), 1e-4));

		// Where the room's walls are, for the sidecar
		const auto extent = MeasureInteriorRoom(pixels, faceSize);
		for (size_t face = 0; face < extent.size(); ++face)
			record["extent"][kInteriorRoomExtentNames[face]] = extent[face];

		InteriorRoomExport result;
		result.path = savePath;
		result.room = EncodeInteriorRoomCube(linear, scale);
		result.record = std::move(record);
		return result;
	});
}

void DynamicCubemaps::FinishInteriorRoomExport()
{
	try {
		auto result = interiorRoomExport.get();
		std::filesystem::create_directories(result.path.parent_path());
		// The GPU encoder takes about a second a cube, where the CPU's full mode search takes minutes
		DirectX::ScratchImage compressed;
		DX::ThrowIfFailed(DirectX::Compress(globals::d3d::device, result.room.GetImages(), result.room.GetImageCount(), result.room.GetMetadata(), DXGI_FORMAT_BC7_UNORM_SRGB, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_ALPHA_WEIGHT_DEFAULT, compressed));
		DX::ThrowIfFailed(DirectX::SaveToDDSFile(compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(), DirectX::DDS_FLAGS_NONE, result.path.c_str()));

		// The sidecar places the room outside for windows near it
		auto recordPath = result.path;
		recordPath.replace_extension(".json");
		std::ofstream(recordPath) << result.record.dump(1, '\t');
		globals::features::truePBR.interiorCaptures.RequestRescan();

		logger::info("[DynamicCubemaps] Interior room written to {}", result.path.string());
		ShowInteriorRoomMessage(std::move(interiorRoomSavedMessage));
	} catch (const std::exception& e) {
		logger::error("[DynamicCubemaps] Failed to save interior room: {}", e.what());
		ShowInteriorRoomMessage(std::move(interiorRoomFailedMessage));
	}
	interiorRoomExporting = false;
}

void DynamicCubemaps::UpdateCubemapCapture(bool a_reflections)
{
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	ID3D11ShaderResourceView* srvs[2] = { depth.depthSRV, main.SRV };
	context->CSSetShaderResources(0, 2, srvs);

	uint index = a_reflections ? 1 : 0;

	ID3D11UnorderedAccessView* uavs[4];
	if (a_reflections) {
		uavs[0] = envCaptureReflectionsTexture->uav.get();
		uavs[1] = envCaptureRawReflectionsTexture->uav.get();
		uavs[2] = envCapturePositionReflectionsTexture->uav.get();
	} else {
		uavs[0] = envCaptureTexture->uav.get();
		uavs[1] = envCaptureRawTexture->uav.get();
		uavs[2] = envCapturePositionTexture->uav.get();
	}

	uavs[3] = captureLightingState->UAV();
	context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);

	UpdateCubemapCB updateData{};

	updateData.CameraPreviousPosAdjust = cameraPreviousPosAdjust[index];
	updateData.CaptureIndex = index;
	updateData.CaptureDeltaTime = std::max(0.0f, globals::state->timer - previousCaptureTime[index]);
	updateData.ResetCapture = resetCapture[index];
	previousCaptureTime[index] = globals::state->timer;
	resetCapture[index] = false;

	auto eyePosition = Util::GetEyePosition();

	cameraPreviousPosAdjust[index] = { eyePosition.x, eyePosition.y, eyePosition.z };

	updateCubemapCB->Update(updateData);

	ID3D11Buffer* buffer = updateCubemapCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetSamplers(0, 1, &computeSampler);

	context->CSSetShader(GetComputeShaderDetectLighting(), nullptr, 0);
	globals::profiler->BeginPass(a_reflections ? "DynamicCubemaps::DetectLightingReflections" : "DynamicCubemaps::DetectLighting");
	context->Dispatch(1, 1, 1);
	globals::profiler->EndPass();

	context->CSSetShader(a_reflections ? (fakeReflections ? GetComputeShaderUpdateFakeReflections() : GetComputeShaderUpdateReflections()) : GetComputeShaderUpdate(), nullptr, 0);

	globals::profiler->BeginPass(a_reflections ? "DynamicCubemaps::CaptureReflections" : "DynamicCubemaps::Capture");
	context->Dispatch((uint32_t)std::ceil(envCaptureTexture->desc.Width / 8.0f), (uint32_t)std::ceil(envCaptureTexture->desc.Height / 8.0f), 6);
	globals::profiler->EndPass();

	uavs[0] = nullptr;
	uavs[1] = nullptr;
	uavs[2] = nullptr;
	uavs[3] = nullptr;
	context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	context->CSSetShaderResources(0, 2, srvs);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* nullSampler = { nullptr };
	context->CSSetSamplers(0, 1, &nullSampler);
}

/**
 * @brief Infers local reflection information from captured cubemap data.
 *
 * @param a_reflections If true, infers from the reflections capture; otherwise from the base capture.
 */
void DynamicCubemaps::Inferrence(bool a_reflections)
{
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	// Infer local reflection information
	ID3D11UnorderedAccessView* uav = envInferredTexture->uav.get();

	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	globals::profiler->BeginPass(a_reflections ? "DynamicCubemaps::CaptureMipsReflections" : "DynamicCubemaps::CaptureMips");
	context->GenerateMips((a_reflections ? envCaptureReflectionsTexture : envCaptureTexture)->srv.get());
	globals::profiler->EndPass();

	auto& cubemap = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];

	ID3D11ShaderResourceView* srvs[3] = { (a_reflections ? envCaptureReflectionsTexture : envCaptureTexture)->srv.get(), cubemap.SRV, defaultCubemap };
	context->CSSetShaderResources(0, 3, srvs);

	context->CSSetSamplers(0, 1, &computeSampler);

	context->CSSetShader(a_reflections ? (fakeReflections ? GetComputeShaderInferrenceFakeReflections() : GetComputeShaderInferrenceReflections()) : GetComputeShaderInferrence(), nullptr, 0);

	globals::profiler->BeginPass(a_reflections ? "DynamicCubemaps::InferReflections" : "DynamicCubemaps::Infer");
	context->Dispatch((uint32_t)std::ceil(envCaptureTexture->desc.Width / 8.0f), (uint32_t)std::ceil(envCaptureTexture->desc.Height / 8.0f), 6);
	globals::profiler->EndPass();

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	srvs[2] = nullptr;
	context->CSSetShaderResources(0, 3, srvs);

	uav = nullptr;

	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetShader(nullptr, 0, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);
}

/**
 * @brief Computes pre-filtered specular environment maps for a specified mip range.
 *
 * When `a_doSetup` is true, copies inferred cubemap faces and regenerates mipmaps before filtering.
 *
 * @param a_reflections If true, processes reflections texture; otherwise, base texture.
 * @param a_startLevel Starting mip level for filtering (inclusive).
 * @param a_endLevel Ending mip level for filtering (exclusive).
 * @param a_doSetup If true, performs face copying and mipmap generation before filtering.
 */
void DynamicCubemaps::Irradiance(bool a_reflections, uint32_t a_startLevel, uint32_t a_endLevel, bool a_doSetup)
{
	auto context = globals::d3d::context;

	if (a_doSetup) {
		globals::profiler->BeginPass(a_reflections ? "DynamicCubemaps::PrepareIrradianceReflections" : "DynamicCubemaps::PrepareIrradiance");
		for (uint face = 0; face < 6; face++) {
			uint srcSubresourceIndex = D3D11CalcSubresource(0, face, MIPLEVELS);
			context->CopySubresourceRegion(envFilteredTexture->resource.get(), D3D11CalcSubresource(0, face, MIPLEVELS), 0, 0, 0, envInferredTexture->resource.get(), srcSubresourceIndex, nullptr);
		}

		auto srv = envInferredTexture->srv.get();
		context->GenerateMips(srv);
		globals::profiler->EndPass();
	}

	// Compute pre-filtered specular environment map for the requested mip range.
	{
		auto srv = envInferredTexture->srv.get();
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetSamplers(0, 1, &computeSampler);
		context->CSSetShader(GetComputeShaderSpecularIrradiance(), nullptr, 0);

		ID3D11Buffer* buffer = spmapCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		float const delta_roughness = 1.0f / std::max(float(MIPLEVELS - 1), 1.0f);

		// Advance size to match a_startLevel.
		std::uint32_t size = std::max(envTexture->desc.Width, envTexture->desc.Height) / 2;
		for (uint32_t i = 1; i < a_startLevel; i++)
			size /= 2;

		// Suffix: A = level 1, BA = levels 2..N-1, BB = last level.
		const char* suffix = (a_startLevel == 1) ? "A" : (a_endLevel == MIPLEVELS) ? "BB" :
		                                                                             "BA";
		const auto passName = a_reflections ? std::format("DynamicCubemaps::IrradianceReflections{}", suffix) : std::format("DynamicCubemaps::Irradiance{}", suffix);
		globals::profiler->BeginPass(passName);
		for (std::uint32_t level = a_startLevel; level < a_endLevel; level++, size /= 2) {
			const UINT numGroups = (UINT)std::max(1u, (size + 7u) / 8u);

			const SpecularMapFilterSettingsCB spmapConstants = { level * delta_roughness };
			spmapCB->Update(spmapConstants);

			auto uav = uavArray[level - 1];

			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->Dispatch(numGroups, numGroups, 6);
		}
		globals::profiler->EndPass();
	}

	ID3D11ShaderResourceView* nullSRV = { nullptr };
	ID3D11SamplerState* nullSampler = { nullptr };
	ID3D11Buffer* nullBuffer = { nullptr };
	ID3D11UnorderedAccessView* nullUAV = { nullptr };

	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetSamplers(0, 1, &nullSampler);
	context->CSSetShader(nullptr, 0, 0);
	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
}

void DynamicCubemaps::CompressToBC6H(bool a_reflections)
{
	auto context = globals::d3d::context;

	auto shader = GetComputeShaderBC6HEncode();
	if (!shader) {
		logger::error("BC6HEncodeCS failed to compile; BC6H compression disabled");
		return;
	}

	auto* srcSRV = envFilteredTextureArraySRV;

	context->CSSetShader(shader, nullptr, 0);
	context->CSSetShaderResources(0, 1, &srcSRV);

	ID3D11Buffer* cb = bc6hEncodeCB->CB();
	context->CSSetConstantBuffers(0, 1, &cb);

	std::uint32_t mipDim = std::max(envTexture->desc.Width, envTexture->desc.Height);

	globals::profiler->BeginPass(a_reflections ? "DynamicCubemaps::BC6HReflections" : "DynamicCubemaps::BC6H");
	for (std::uint32_t level = 0; level < bc6hMipLevels; ++level) {
		std::uint32_t srcWidth = std::max(1u, mipDim >> level);
		std::uint32_t srcHeight = std::max(1u, mipDim >> level);
		std::uint32_t blocksX = std::max(1u, srcWidth / 4);
		std::uint32_t blocksY = std::max(1u, srcHeight / 4);

		BC6HEncodeCB cbData{};
		cbData.TextureSizeInBlocksX = blocksX;
		cbData.TextureSizeInBlocksY = blocksY;
		cbData.MipLevel = level;
		bc6hEncodeCB->Update(cbData);

		context->CSSetUnorderedAccessViews(0, 1, &bc6hScratchUAVs[level], nullptr);

		std::uint32_t dispatchX = std::max(1u, (blocksX + 7) / 8);
		std::uint32_t dispatchY = std::max(1u, (blocksY + 7) / 8);
		context->Dispatch(dispatchX, dispatchY, 6);
	}
	globals::profiler->EndPass();

	{
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	globals::profiler->BeginPass(a_reflections ? "DynamicCubemaps::PublishReflections" : "DynamicCubemaps::Publish");
	auto dst = a_reflections ? envReflectionsTextureBC6H : envTextureBC6H;
	for (std::uint32_t level = 0; level < bc6hMipLevels; ++level) {
		for (std::uint32_t face = 0; face < 6; ++face) {
			const bool tail = level >= bc6hScratchMipLevels;
			ID3D11Resource* src = tail ? bc6hTailScratchTexture->resource.get() : bc6hScratchTexture->resource.get();
			UINT srcSubresource = tail ? D3D11CalcSubresource(0, (level - bc6hScratchMipLevels) * 6 + face, 1) : D3D11CalcSubresource(level, face, bc6hScratchMipLevels);
			context->CopySubresourceRegion(dst->resource.get(), D3D11CalcSubresource(level, face, bc6hMipLevels), 0, 0, 0, src, srcSubresource, nullptr);
		}
	}
	context->CopyResource((a_reflections ? envReflectionsTexture : envTexture)->resource.get(), envFilteredTexture->resource.get());
	cubemapValid[a_reflections ? 1 : 0] = true;
	globals::profiler->EndPass();
}

/**
 * @brief Advances the cubemap update pipeline state machine by one task.
 *
 * Executes the next step in a multi-frame sequence that captures, infers, filters,
 * and compresses environment cubemaps. Resets capture when game time jumps
 * significantly and recompiles shaders if needed. Processes either the base or
 * reflection variant depending on the current task.
 */
void DynamicCubemaps::UpdateCubemap()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Cubemap Update");

	// Reset capture when game time jumps (wait menu, timescale changes, console commands)
	if (auto calendar = globals::game::calendar) {
		float currentHoursPassed = calendar->GetHoursPassed();
		float hoursPassedDiff = std::abs(currentHoursPassed - previousHoursPassed);
		previousHoursPassed = currentHoursPassed;

		if (hoursPassedDiff >= 0.01f) {  // ~36 seconds game time
			resetCapture[0] = true;
			resetCapture[1] = true;
		}
	}

	if (recompileFlag) {
		logger::debug("Recompiling for Dynamic Cubemaps");
		auto shaderCache = globals::shaderCache;
		if (!shaderCache->Clear("Data//Shaders//ISReflectionsRayTracing.hlsl"))
			// if can't find specific hlsl file cache, clear all image space files
			shaderCache->Clear(RE::BSShader::Types::ImageSpace);
		recompileFlag = false;
	}

	if (!GetComputeShaderDetectLighting() || !GetComputeShaderUpdate() || !GetComputeShaderInferrence() || !GetComputeShaderSpecularIrradiance() || !GetComputeShaderBC6HEncode())
		return;
	if (activeReflections && !(fakeReflections ?
									 (GetComputeShaderUpdateFakeReflections() && GetComputeShaderInferrenceFakeReflections()) :
									 (GetComputeShaderUpdateReflections() && GetComputeShaderInferrenceReflections())))
		return;

	// A room export processed on its worker thread is compressed and saved here, where the GPU can be used
	if (interiorRoomExport.valid() && interiorRoomExport.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		FinishInteriorRoomExport();
	if (interiorRoomCaptureRequested.exchange(false) && !interiorRoomCapture)
		StartInteriorRoomCapture();
	if (interiorRoomCapture && interiorRoomCancelRequested.exchange(false)) {
		SetInteriorRoomView(interiorRoomCapture->heading, interiorRoomCapture->pitch);
		interiorRoomCapture.reset();
		ShowInteriorRoomMessage(T(TKEY("interior_room_capture_cancelled"), "Room capture cancelled"));
	}
	if (interiorRoomCapture) {
		AccumulateInteriorRoom();
		const float progress = (globals::state->timer - interiorRoomCapture->startTime) / kInteriorRoomCaptureSeconds;
		if (interiorRoomSaveRequested.exchange(false) || progress >= 1.0f) {
			SetInteriorRoomView(interiorRoomCapture->heading, interiorRoomCapture->pitch);
			ExportInteriorRoom();
		} else {
			// The view turns for the next frame, which this frame's accumulation then sees
			const auto [yaw, pitch] = GetInteriorRoomSweepAngles(progress);
			SetInteriorRoomView(interiorRoomCapture->heading + yaw, pitch);
		}
	}

	static constexpr uint32_t kIrradianceSplit = 2;
	static constexpr uint32_t kIrradianceSplitB = MIPLEVELS - 1;

	switch (nextTask) {
	case NextTask::kCaptureInferAndIrradianceA:
		UpdateCubemapCapture(false);
		Inferrence(false);
		Irradiance(false, 1, kIrradianceSplit, /*doSetup=*/true);
		nextTask = NextTask::kIrradianceBA;
		break;

	case NextTask::kIrradianceBA:
		Irradiance(false, kIrradianceSplit, kIrradianceSplitB, /*doSetup=*/false);
		nextTask = NextTask::kIrradianceBBAndBC6H;
		break;

	case NextTask::kIrradianceBBAndBC6H:
		Irradiance(false, kIrradianceSplitB, MIPLEVELS, /*doSetup=*/false);
		CompressToBC6H(false);
		nextTask = activeReflections ? NextTask::kCaptureInferAndIrradianceA2 : NextTask::kCaptureInferAndIrradianceA;
		break;

	case NextTask::kCaptureInferAndIrradianceA2:
		UpdateCubemapCapture(true);
		Inferrence(true);
		Irradiance(true, 1, kIrradianceSplit, /*doSetup=*/true);
		nextTask = NextTask::kIrradianceBA2;
		break;

	case NextTask::kIrradianceBA2:
		Irradiance(true, kIrradianceSplit, kIrradianceSplitB, /*doSetup=*/false);
		nextTask = NextTask::kIrradianceBBAndBC6H2;
		break;

	case NextTask::kIrradianceBBAndBC6H2:
		Irradiance(true, kIrradianceSplitB, MIPLEVELS, /*doSetup=*/false);
		CompressToBC6H(true);
		nextTask = NextTask::kCaptureInferAndIrradianceA;
		break;
	}
}

void DynamicCubemaps::PostDeferred()
{
	auto context = globals::d3d::context;

	ID3D11ShaderResourceView* views[2] = {
		cubemapValid[activeReflections ? 1 : 0] ? (activeReflections ? envReflectionsTextureBC6H : envTextureBC6H)->srv.get() : nullptr,
		cubemapValid[0] ? envTextureBC6H->srv.get() : nullptr
	};
	context->PSSetShaderResources(30, 2, views);
}

void DynamicCubemaps::SetupResources()
{
	GetComputeShaderDetectLighting();
	GetComputeShaderUpdate();
	GetComputeShaderUpdateReflections();
	GetComputeShaderInferrence();
	GetComputeShaderInferrenceReflections();
	GetComputeShaderSpecularIrradiance();
	GetComputeShaderBC6HEncode();

	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &computeSampler));
		Util::SetResourceName(computeSampler, "DynamicCubemaps::ComputeSampler");
	}

	auto& cubemap = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];

	{
		D3D11_TEXTURE2D_DESC texDesc;
		cubemap.texture->GetDesc(&texDesc);
		assert(texDesc.Width == (1u << (MIPLEVELS - 1)));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		cubemap.SRV->GetDesc(&srvDesc);

		texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		// Create additional resources

		texDesc.MipLevels = MIPLEVELS;
		texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		srvDesc.Format = texDesc.Format;
		srvDesc.TextureCube.MipLevels = MIPLEVELS;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.MipSlice = 0;
		uavDesc.Texture2DArray.FirstArraySlice = 0;
		uavDesc.Texture2DArray.ArraySize = texDesc.ArraySize;

		envCaptureTexture = new Texture2D(texDesc);
		envCaptureTexture->CreateSRV(srvDesc);
		envCaptureTexture->CreateUAV(uavDesc);

		envCaptureRawTexture = new Texture2D(texDesc);
		envCaptureRawTexture->CreateSRV(srvDesc);
		envCaptureRawTexture->CreateUAV(uavDesc);

		envCapturePositionTexture = new Texture2D(texDesc);
		envCapturePositionTexture->CreateSRV(srvDesc);
		envCapturePositionTexture->CreateUAV(uavDesc);

		envCaptureReflectionsTexture = new Texture2D(texDesc);
		envCaptureReflectionsTexture->CreateSRV(srvDesc);
		envCaptureReflectionsTexture->CreateUAV(uavDesc);

		envCaptureRawReflectionsTexture = new Texture2D(texDesc);
		envCaptureRawReflectionsTexture->CreateSRV(srvDesc);
		envCaptureRawReflectionsTexture->CreateUAV(uavDesc);

		envCapturePositionReflectionsTexture = new Texture2D(texDesc);
		envCapturePositionReflectionsTexture->CreateSRV(srvDesc);
		envCapturePositionReflectionsTexture->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		envTexture = new Texture2D(texDesc);
		envTexture->CreateSRV(srvDesc);
		envTexture->CreateUAV(uavDesc);

		envReflectionsTexture = new Texture2D(texDesc);
		envReflectionsTexture->CreateSRV(srvDesc);
		envReflectionsTexture->CreateUAV(uavDesc);

		envFilteredTexture = new Texture2D(texDesc, "DynamicCubemaps::EnvFiltered");
		envFilteredTexture->CreateSRV(srvDesc);
		envFilteredTexture->CreateUAV(uavDesc);

		const float clear[4]{};
		for (auto* texture : { envTexture, envReflectionsTexture }) {
			globals::d3d::context->ClearUnorderedAccessViewFloat(texture->uav.get(), clear);
			globals::d3d::context->GenerateMips(texture->srv.get());
		}

		// Texture2DArray SRVs used by BC6H encoder (Load() requires array dimension, not TextureCube)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC arraySRVDesc = {};
			arraySRVDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
			arraySRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			arraySRVDesc.Texture2DArray.FirstArraySlice = 0;
			arraySRVDesc.Texture2DArray.ArraySize = 6;
			arraySRVDesc.Texture2DArray.MostDetailedMip = 0;
			arraySRVDesc.Texture2DArray.MipLevels = MIPLEVELS;
			DX::ThrowIfFailed(device->CreateShaderResourceView(envFilteredTexture->resource.get(), &arraySRVDesc, &envFilteredTextureArraySRV));
			Util::SetResourceName(envFilteredTextureArraySRV, "DynamicCubemaps::EnvFiltered ArraySRV");
		}

		envInferredTexture = new Texture2D(texDesc, "DynamicCubemaps::EnvInferred");
		envInferredTexture->CreateSRV(srvDesc);
		envInferredTexture->CreateUAV(uavDesc);

		// BC6H scratch: R32G32B32A32_UINT at quarter-resolution, 6-face array.
		// Encoded directly into here via UAV, then copied per subresource to the BC6H texture.
		// Mips below 4x4 still take one block each, so they are encoded into a 1x1 tail array.
		{
			std::uint32_t scratchBase = std::max(1u, texDesc.Width / 4);
			bc6hMipLevels = std::min<std::uint32_t>(MIPLEVELS, static_cast<std::uint32_t>(std::size(bc6hScratchUAVs)));
			bc6hScratchMipLevels = 0;
			for (std::uint32_t d = scratchBase; d > 0; d >>= 1)
				++bc6hScratchMipLevels;
			bc6hScratchMipLevels = std::min(bc6hScratchMipLevels, bc6hMipLevels);

			D3D11_TEXTURE2D_DESC scratchDesc = {};
			scratchDesc.Width = scratchBase;
			scratchDesc.Height = std::max(1u, texDesc.Height / 4);
			scratchDesc.MipLevels = bc6hScratchMipLevels;
			scratchDesc.ArraySize = 6;
			scratchDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
			scratchDesc.SampleDesc.Count = 1;
			scratchDesc.Usage = D3D11_USAGE_DEFAULT;
			scratchDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			scratchDesc.MiscFlags = 0;
			bc6hScratchTexture = new Texture2D(scratchDesc, "DynamicCubemaps::BC6HScratch");

			D3D11_UNORDERED_ACCESS_VIEW_DESC scratchUAVDesc = {};
			scratchUAVDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
			scratchUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			scratchUAVDesc.Texture2DArray.FirstArraySlice = 0;
			scratchUAVDesc.Texture2DArray.ArraySize = 6;
			for (std::uint32_t level = 0; level < bc6hScratchMipLevels; ++level) {
				scratchUAVDesc.Texture2DArray.MipSlice = level;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(bc6hScratchTexture->resource.get(), &scratchUAVDesc, &bc6hScratchUAVs[level]));
				Util::SetResourceName(bc6hScratchUAVs[level], "DynamicCubemaps::BC6HScratch UAV mip%u", level);
			}

			if (bc6hMipLevels > bc6hScratchMipLevels) {
				D3D11_TEXTURE2D_DESC tailDesc = scratchDesc;
				tailDesc.Width = 1;
				tailDesc.Height = 1;
				tailDesc.MipLevels = 1;
				tailDesc.ArraySize = 6 * (bc6hMipLevels - bc6hScratchMipLevels);
				bc6hTailScratchTexture = new Texture2D(tailDesc, "DynamicCubemaps::BC6HTailScratch");

				scratchUAVDesc.Texture2DArray.MipSlice = 0;
				for (std::uint32_t level = bc6hScratchMipLevels; level < bc6hMipLevels; ++level) {
					scratchUAVDesc.Texture2DArray.FirstArraySlice = 6 * (level - bc6hScratchMipLevels);
					DX::ThrowIfFailed(device->CreateUnorderedAccessView(bc6hTailScratchTexture->resource.get(), &scratchUAVDesc, &bc6hScratchUAVs[level]));
					Util::SetResourceName(bc6hScratchUAVs[level], "DynamicCubemaps::BC6HTailScratch UAV mip%u", level);
				}
			}
		}

		// BC6H compressed cubemap textures (shader-read-only).
		{
			D3D11_TEXTURE2D_DESC bc6hDesc = {};
			bc6hDesc.Width = texDesc.Width;
			bc6hDesc.Height = texDesc.Height;
			bc6hDesc.MipLevels = bc6hMipLevels;
			bc6hDesc.ArraySize = 6;
			bc6hDesc.Format = DXGI_FORMAT_BC6H_UF16;
			bc6hDesc.SampleDesc.Count = 1;
			bc6hDesc.Usage = D3D11_USAGE_DEFAULT;
			bc6hDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			bc6hDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

			D3D11_SHADER_RESOURCE_VIEW_DESC bc6hSRVDesc = {};
			bc6hSRVDesc.Format = DXGI_FORMAT_BC6H_UF16;
			bc6hSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			bc6hSRVDesc.TextureCube.MostDetailedMip = 0;
			bc6hSRVDesc.TextureCube.MipLevels = bc6hMipLevels;

			envTextureBC6H = new Texture2D(bc6hDesc, "DynamicCubemaps::EnvTextureBC6H");
			envTextureBC6H->CreateSRV(bc6hSRVDesc);

			envReflectionsTextureBC6H = new Texture2D(bc6hDesc, "DynamicCubemaps::EnvReflectionsBC6H");
			envReflectionsTextureBC6H->CreateSRV(bc6hSRVDesc);
		}

		updateCubemapCB = new ConstantBuffer(ConstantBufferDesc<UpdateCubemapCB>(), "DynamicCubemaps::UpdateCubemapCB");
		captureLightingState = std::make_unique<StructuredBuffer>(StructuredBufferDesc<CaptureLightingState>(uint64_t(1), true, false), 1, "DynamicCubemaps::CaptureLightingState");
		captureLightingState->CreateUAV();
		const UINT clearState[4]{};
		globals::d3d::context->ClearUnorderedAccessViewUint(captureLightingState->UAV(), clearState);
	}

	{
		bc6hEncodeCB = new ConstantBuffer(ConstantBufferDesc<BC6HEncodeCB>(), "DynamicCubemaps::BC6HEncodeCB");
	}

	{
		spmapCB = new ConstantBuffer(ConstantBufferDesc<SpecularMapFilterSettingsCB>(), "DynamicCubemaps::SpmapCB");
	}

	{
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = envFilteredTexture->desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;

		uavDesc.Texture2DArray.FirstArraySlice = 0;
		uavDesc.Texture2DArray.ArraySize = envTexture->desc.ArraySize;

		for (std::uint32_t level = 1; level < MIPLEVELS; ++level) {
			uavDesc.Texture2DArray.MipSlice = level;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(envFilteredTexture->resource.get(), &uavDesc, &uavArray[level - 1]));
			Util::SetResourceName(uavArray[level - 1], "DynamicCubemaps::EnvFiltered UAV mip%u", level);
		}
	}

	{
		DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\DynamicCubemaps\\defaultcubemap.dds", nullptr, &defaultCubemap);
	}
}

void DynamicCubemaps::Reset()
{
	activeReflections = globals::state->activeReflections;

	if (globals::game::sky)
		fakeReflections = activeReflections && globals::game::sky->flags.any(RE::Sky::Flags::kHideSky);
	else
		fakeReflections = false;

	if (!activeReflections && !Util::IsInterior()) {
		activeReflections = true;
		fakeReflections = true;
	}
}
#undef I18N_KEY_PREFIX
