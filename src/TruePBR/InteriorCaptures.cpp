#include "TruePBR/InteriorCaptures.h"

#include "Utils/Game.h"

namespace
{
	// Where the room export saves captures, relative to the game folder
	const std::filesystem::path kCaptureDirectory = "Data\\textures\\interiors";
	// Captured rooms are loaded while the camera is this near their capture point, in game units
	constexpr float kLoadRange = 8192.0f;
	// Wall distances in a capture file, in cube face order
	constexpr std::array<const char*, 6> kExtentNames = { "right", "left", "up", "down", "back", "front" };
}

void InteriorCaptures::Scan()
{
	// A load in flight belongs to the old records
	if (pendingLoad.valid())
		pendingLoad.wait();
	pendingLoad = {};
	pendingRecord = -1;
	records.clear();
	slotRecords.fill(-1);

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(kCaptureDirectory, error)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".json")
			continue;
		try {
			std::ifstream file(entry.path());
			const json data = json::parse(file);
			// Interiors without a door outside have nowhere to be placed
			if (!data.contains("worldspace"))
				continue;

			Record record;
			record.room = entry.path();
			record.room.replace_extension(".dds");
			if (!std::filesystem::exists(record.room))
				continue;

			const auto& worldspace = data.at("worldspace");
			record.worldspace = dataHandler ? dataHandler->LookupFormID(worldspace.at("formID").get<RE::FormID>(), worldspace.at("plugin").get<std::string>()) : 0;
			if (!record.worldspace)
				continue;
			const auto& position = data.at("position");
			record.position = { position.at(0).get<float>(), position.at(1).get<float>(), position.at(2).get<float>() };
			record.heading = data.at("heading").get<float>();
			const auto& extent = data.at("extent");
			for (size_t face = 0; face < kExtentNames.size(); ++face)
				record.extent[face] = std::max(extent.at(kExtentNames[face]).get<float>(), 1.0f);
			records.push_back(std::move(record));
		} catch (const std::exception& e) {
			logger::warn("[TruePBR] Skipping interior capture {}: {}", entry.path().string(), e.what());
		}
	}
	logger::info("[TruePBR] Found {} interior captures placed outside", records.size());
}

bool InteriorCaptures::Upload(ID3D11DeviceContext* a_context, const DirectX::ScratchImage& a_room, uint32_t a_slot)
{
	const auto& info = a_room.GetMetadata();
	try {
		if (!rooms) {
			// Sized and formatted by the first room; the export writes them all alike
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(info.width);
			desc.Height = static_cast<UINT>(info.height);
			desc.MipLevels = static_cast<UINT>(info.mipLevels);
			desc.ArraySize = 6 * kMaxRooms;
			desc.Format = info.format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
			rooms = std::make_unique<Texture2D>(desc, "TruePBR::InteriorCaptureRooms");

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
			srvDesc.TextureCubeArray.MipLevels = desc.MipLevels;
			srvDesc.TextureCubeArray.NumCubes = kMaxRooms;
			rooms->CreateSRV(srvDesc);
		}
	} catch (const std::exception& e) {
		logger::error("[TruePBR] Failed to create the interior capture rooms: {}", e.what());
		rooms.reset();
		return false;
	}

	const auto& desc = rooms->desc;
	if (info.width != desc.Width || info.height != desc.Height || info.mipLevels != desc.MipLevels || info.format != desc.Format) {
		logger::warn("[TruePBR] Interior capture is {}x{} with {} mips in format {}, unlike the loaded rooms", info.width, info.height, info.mipLevels, static_cast<int>(info.format));
		return false;
	}
	for (uint32_t face = 0; face < 6; ++face) {
		for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
			const auto* subresource = a_room.GetImage(mip, face, 0);
			const UINT index = D3D11CalcSubresource(mip, a_slot * 6 + face, desc.MipLevels);
			a_context->UpdateSubresource(rooms->resource.get(), index, nullptr, subresource->pixels, static_cast<UINT>(subresource->rowPitch), static_cast<UINT>(subresource->slicePitch));
		}
	}
	return true;
}

void InteriorCaptures::Update(ID3D11DeviceContext* a_context)
{
	if (!captures) {
		try {
			captures = std::make_unique<StructuredBuffer>(StructuredBufferDesc<Capture>(kMaxRooms), kMaxRooms, "TruePBR::InteriorCaptures");
			captures->CreateSRV();
		} catch (const std::exception& e) {
			logger::error("[TruePBR] Failed to create the interior capture buffer: {}", e.what());
			captures.reset();
			return;
		}
		for (auto& capture : uploaded)
			capture.position.w = -1.0f;
		captures->Update(uploaded.data(), sizeof(uploaded));
	}
	if (rescanRequested.exchange(false))
		Scan();

	// Captured rooms in this worldspace, nearest the camera first
	auto* player = RE::PlayerCharacter::GetSingleton();
	const auto* worldspace = player ? player->GetWorldspace() : nullptr;
	const auto camera = Util::GetEyePosition();
	std::vector<std::pair<float, int>> nearby;
	for (int index = 0; worldspace && index < static_cast<int>(records.size()); ++index) {
		const auto& record = records[index];
		const float distance = std::hypot(record.position.x - camera.x, record.position.y - camera.y, record.position.z - camera.z);
		if (record.worldspace == worldspace->GetFormID() && !record.failed && distance < kLoadRange)
			nearby.emplace_back(distance, index);
	}
	std::sort(nearby.begin(), nearby.end());
	if (nearby.size() > kMaxRooms)
		nearby.resize(kMaxRooms);
	auto isNearby = [&](int a_record) { return std::any_of(nearby.begin(), nearby.end(), [&](const auto& a_entry) { return a_entry.second == a_record; }); };

	// A finished load takes a slot no nearby room holds
	if (pendingLoad.valid() && pendingLoad.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		const auto loaded = pendingLoad.get();
		auto& record = records[pendingRecord];
		const auto slot = std::find_if(slotRecords.begin(), slotRecords.end(), [&](int a_record) { return a_record < 0 || !isNearby(a_record); });
		if (!loaded) {
			record.failed = true;
		} else if (slot != slotRecords.end()) {
			const auto index = static_cast<uint32_t>(slot - slotRecords.begin());
			if (Upload(a_context, *loaded, index)) {
				if (*slot >= 0)
					records[*slot].slot = -1;
				*slot = pendingRecord;
				record.slot = static_cast<int>(index);
			} else {
				record.failed = true;
			}
		}
		pendingRecord = -1;
	}

	// Rooms load one at a time, nearest first
	if (!pendingLoad.valid()) {
		for (const auto& [distance, index] : nearby) {
			const auto& record = records[index];
			if (record.slot >= 0)
				continue;
			pendingRecord = index;
			pendingLoad = std::async(std::launch::async, [room = record.room]() -> std::optional<DirectX::ScratchImage> {
				DirectX::ScratchImage loaded;
				DirectX::TexMetadata info;
				if (FAILED(DirectX::LoadFromDDSFile(room.c_str(), DirectX::DDS_FLAGS_NONE, &info, loaded))) {
					logger::warn("[TruePBR] Failed to read interior capture {}", room.string());
					return std::nullopt;
				}
				if (!info.IsCubemap() || info.arraySize != 6) {
					logger::warn("[TruePBR] Interior capture {} is not a cubemap", room.string());
					return std::nullopt;
				}
				return loaded;
			});
			break;
		}
	}

	std::array<Capture, kMaxRooms> data{};
	for (auto& capture : data)
		capture.position.w = -1.0f;
	uint32_t count = 0;
	for (const auto& [distance, index] : nearby) {
		const auto& record = records[index];
		if (record.slot < 0)
			continue;
		// Skyrim headings turn clockwise from +Y, so the room faces (sin, cos) and its right is a quarter turn on
		const float s = std::sin(record.heading);
		const float c = std::cos(record.heading);
		const auto& extent = record.extent;
		data[count].position = { record.position.x, record.position.y, record.position.z, static_cast<float>(record.slot) };
		data[count].right = { c, -s, 0.0f, extent[1] };
		data[count].forward = { s, c, 0.0f, extent[0] };
		data[count].extent = { extent[3], extent[2], extent[5], extent[4] };
		++count;
	}
	if (std::memcmp(data.data(), uploaded.data(), sizeof(data)) != 0) {
		uploaded = data;
		captures->Update(uploaded.data(), sizeof(uploaded));
	}

	ID3D11ShaderResourceView* views[2] = { rooms ? rooms->srv.get() : nullptr, captures->SRV() };
	a_context->PSSetShaderResources(23, 2, views);
}
