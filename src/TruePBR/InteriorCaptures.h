#pragma once

#include "Buffer.h"

#include <DirectXTex.h>
#include <future>

/**
 * Rooms captured inside interiors, placed in the exterior world through the door that links them, so windows near a
 * capture look into the room behind them. The room export saves each capture as a room cubemap with a sidecar file
 * giving where it lies outside.
 */
class InteriorCaptures
{
public:
	/** One captured room for the lighting shader; matches PBR::InteriorCapture. */
	struct alignas(16) Capture
	{
		float4 position;  // Capture point in the world, and its cube in the room array, negative when unused
		float4 right;     // Room right in the world, and the distance to its left wall
		float4 forward;   // Room forward in the world, towards its back wall, and the distance to its right wall
		float4 extent;    // Distances to the floor, the ceiling, the wall behind the capture point and the back wall
	};
	STATIC_ASSERT_ALIGNAS_16(Capture);

	/** Captured rooms loaded at once, nearest the camera first; enough for every room of a large building. */
	static constexpr uint32_t kMaxRooms = 8;

	/**
	 * @brief Picks the captured rooms near the camera, loads them in turn and binds them for the lighting shader.
	 * Runs on the render thread once a frame.
	 */
	void Update(ID3D11DeviceContext* a_context);

	/** @brief Reads the capture files again before the next update, as after a room export adds one. */
	void RequestRescan() { rescanRequested = true; }

private:
	struct Record
	{
		std::filesystem::path room;
		RE::FormID worldspace = 0;
		float3 position;
		float heading = 0.0f;
		std::array<float, 6> extent{};  // Right, left, up, down, back and front, as the cube faces
		int slot = -1;
		bool failed = false;
	};

	void Scan();
	bool Upload(ID3D11DeviceContext* a_context, const DirectX::ScratchImage& a_room, uint32_t a_slot);

	std::vector<Record> records;
	std::array<int, kMaxRooms> slotRecords = [] {
		std::array<int, kMaxRooms> slots;
		slots.fill(-1);
		return slots;
	}();
	std::future<std::optional<DirectX::ScratchImage>> pendingLoad;
	int pendingRecord = -1;
	std::atomic<bool> rescanRequested = true;

	std::unique_ptr<Texture2D> rooms;
	std::unique_ptr<StructuredBuffer> captures;
	std::array<Capture, kMaxRooms> uploaded{};
};
