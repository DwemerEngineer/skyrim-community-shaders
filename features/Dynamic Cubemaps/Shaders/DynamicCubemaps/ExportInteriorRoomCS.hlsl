// Builds an interior mapping room cubemap: the room around the camera where the capture started, with the player's
// heading then as the room's back wall, so cube directions are in the room's frame (x right, y up, z towards the back
// wall). The alpha channel holds each texel's distance from that camera in game units, which the export encodes for
// interior mapping to rebuild the room around the capture point.
//
// With ACCUMULATE, each frame of the capture writes the full-resolution frame into the cube where it is on screen,
// keeping for every texel the frame that saw it nearest the middle of the screen. Without it, the result is completed:
// texels never seen on screen take the dynamic cubemap capture, preferring what it saw over what it inferred, blended
// across a band where the frame's edge was.

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "DynamicCubemaps/CubemapCommon.hlsli"

cbuffer ExportInteriorRoomCB : register(b0)
{
	float4 RoomRight;
	float4 RoomUp;
	float4 RoomForward;
	float4 CaptureOffset;  // From the camera where the capture started to the camera the positions are relative to
};

// Distance for the sky and anything else without a surface
static const float SkyDistance = 100000.0;
// Width of the band at the screen's edge where the frame blends into the capture, in screen UV
static const float EdgeBlend = 0.05;

/** World direction of a room cubemap texel, from its direction in the room's frame. */
float3 GetRoomDirection(uint3 texel, uint2 size)
{
	const float3 room = GetCubemapSamplingVector(texel, float2(size));
	return room.x * RoomRight.xyz + room.y * RoomUp.xyz + room.z * RoomForward.xyz;
}

#if defined(ACCUMULATE)

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> ColorTexture : register(t1);

SamplerState LinearSampler : register(s0);

RWTexture2DArray<float4> RoomColor : register(u0);  // Frame color, and distance in alpha
RWTexture2DArray<float> RoomWeight : register(u1);  // How far inside the screen the stored frame saw the texel, 0 for never

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	uint width, height, faces;
	RoomColor.GetDimensions(width, height, faces);
	if (any(ThreadID.xy >= uint2(width, height)))
		return;

	const float3 worldDirection = GetRoomDirection(ThreadID, uint2(width, height));
	const float4 positionCS = mul(FrameBuffer::CameraViewProj, float4(worldDirection * 1000.0, 1.0));
	const float2 uv = positionCS.xy / positionCS.w * float2(0.5, -0.5) + 0.5;
	if (positionCS.w <= 0.0 || FrameBuffer::IsOutsideFrame(uv))
		return;

	// Texels seen nearer the middle of the screen are less stretched by the frame's own projection
	const float2 edge = min(uv, 1.0 - uv);
	const float weight = min(edge.x, edge.y);
	if (weight <= RoomWeight[ThreadID])
		return;

	const float2 sampleUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
	const float depth = DepthTexture.SampleLevel(LinearSampler, sampleUV, 0);
	// The player's own body and weapon are too close to belong to the room
	if (SharedData::GetScreenDepth(depth) <= 16.5)
		return;

	const float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0));
	const float distance = depth >= 1.0 ? SkyDistance : length(position.xyz / position.w + CaptureOffset.xyz);
	const float3 color = ColorManagement::SceneToLinear(ColorTexture.SampleLevel(LinearSampler, sampleUV, 0).rgb);
	if (!all(isfinite(color)) || !isfinite(distance))
		return;

	RoomColor[ThreadID] = float4(max(color, 0.0), distance);
	RoomWeight[ThreadID] = weight;
}

#else

TextureCube<float4> EnvTexture : register(t0);
TextureCube<float4> EnvPositionTexture : register(t1);
TextureCube<float4> EnvRawTexture : register(t2);  // Captured colors only, with alpha 1 where something was seen
Texture2DArray<float4> RoomColor : register(t3);
Texture2DArray<float> RoomWeight : register(t4);

SamplerState LinearSampler : register(s0);

RWTexture2DArray<float4> RoomImage : register(u0);

[numthreads(8, 8, 1)] void main(uint3 ThreadID : SV_DispatchThreadID) {
	uint width, height, faces;
	RoomImage.GetDimensions(width, height, faces);
	if (any(ThreadID.xy >= uint2(width, height)))
		return;

	// The capture, preferring what it saw over what it inferred
	const float3 worldDirection = GetRoomDirection(ThreadID, uint2(width, height));
	const float4 raw = EnvRawTexture.SampleLevel(LinearSampler, worldDirection, 0);
	const float seen = saturate(raw.a);
	float3 color = lerp(EnvTexture.SampleLevel(LinearSampler, worldDirection, 0).rgb, raw.rgb / max(raw.a, 1e-3), seen);
	// Captured positions are in kilometres relative to the camera at the last capture, and zero where nothing was seen,
	// so filtering next to unseen texels is undone with the share of seen texels in the same footprint. Texels it barely
	// saw hold stale positions far beyond the room, so they are left without a distance and filled from around them
	const float3 capturePosition = EnvPositionTexture.SampleLevel(LinearSampler, worldDirection, 0).xyz / max(seen, 1e-3);
	float distance = seen >= 0.5 ? length(capturePosition * 1000.0 - CaptureOffset.xyz) : 0.0;

	const float weight = RoomWeight[ThreadID];
	if (weight > 0.0) {
		// Without a captured distance there is nothing to blend the frame's depth with
		const float frameWeight = distance > 0.0 ? saturate(weight / EdgeBlend) : 1.0;
		const float4 frame = RoomColor[ThreadID];
		color = lerp(color, frame.rgb, frameWeight);
		distance = lerp(distance, frame.a, frameWeight);
	}

	// Linear sRGB primaries, stored in an sRGB texture like any other color texture
#	if defined(ENABLE_ACESCG)
	color = AP1TosRGB(color);
#	endif

	RoomImage[ThreadID] = float4(max(color, 0.0), all(isfinite(color)) ? distance : 0.0);
}

#endif
