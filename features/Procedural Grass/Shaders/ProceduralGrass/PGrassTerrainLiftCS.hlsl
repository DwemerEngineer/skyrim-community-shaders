// Caches the rendered terrain surface for Low and Far roots in a world-aligned map.
// Export both the lift above LAND and the surface height that bounds it at detailed crests.
// Approach distance controls the generator's blend back to LAND.

#include "Common/FrameBuffer.hlsli"

#include "ProceduralGrass/PGrassCommon.hlsli"

Texture2D<float> TerrainHeightTexture : register(t0);
Texture2D<float> SceneDepth : register(t1);  // Full-resolution scene depth, including the High and Mid prepass.
RWTexture2D<float> MeasuredHeight : register(u0);
RWTexture2D<float> AppliedLift : register(u1);
RWTexture2D<float> AppliedHeight : register(u2);
SamplerState LinearSampler : register(s0);

static const float InvalidSurfaceHeight = 1.0e30f;
// Simplified LOD triangles can span a LAND cell across the loaded boundary.
static const float TerrainLodOverlapMargin = 4096.0f;

float SampleLandHeight(float2 world2D)
{
	return lerp(heightMapZRange.x, heightMapZRange.y, TerrainHeightTexture.SampleLevel(LinearSampler, world2D * heightMapScale + heightMapOffset, 0));
}

/** @brief Projects a camera-relative position; xy is its scene-depth pixel, z its depth, and w is positive when it is on screen. */
float4 ProjectToSceneDepth(float3 position)
{
	float4 clip = mul(FrameBuffer::CameraViewProj, float4(position, 1.0f));
	float2 uv = clip.xy / max(clip.w, 1.0f) * float2(0.5f, -0.5f) + 0.5f;
	return float4(uv / dynamicResolutionInverted, clip.z / max(clip.w, 1.0f), clip.w > 1.0f && all(uv >= 0.0f) && all(uv < 1.0f) ? 1.0f : -1.0f);
}

/** @brief Returns how far a position lies outside the loaded cells; negative inside. */
float DistanceOutsideLoadedLand(float2 world2D)
{
	float2 outside = max(loadedLandBounds.xy - world2D, world2D - loadedLandBounds.zw);
	return max(outside.x, outside.y);
}

/**
 * @brief Measures the rendered terrain's absolute height at a cell centre near or outside the loaded LAND boundary.
 * Returns `previous` when the cell cannot be measured this frame, so cells keep what was seen when last visible.
 */
float MeasureHeight(float2 world2D, float previous)
{
	const bool forwardPerspective = FrameBuffer::CameraProj._m32 == 1.0f && FrameBuffer::CameraProj._m33 == 0.0f && FrameBuffer::CameraProj._m23 < 0.0f;
	const float landHeight = SampleLandHeight(world2D);
	const float3 root = float3(world2D, landHeight) - FrameBuffer::CameraPosAdjust.xyz;
	const float4 rootPixel = ProjectToSceneDepth(root);
	if (!forwardPerspective || rootPixel.w < 0.0f)
		return previous;

	// When the LAND height itself is visible, the rendered terrain is not above it.
	const float sceneDepth = SceneDepth.Load(int3(rootPixel.xy, 0));
	if (sceneDepth >= rootPixel.z)
		return landHeight + 4.0f;

	// Include the overlap of simplified LOD triangles with loaded LAND.
	const float2 sceneNDC = (floor(rootPixel.xy) + 0.5f) * dynamicResolutionInverted * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
	float4 scenePosition = mul(FrameBuffer::CameraViewProjInverse, float4(sceneNDC, sceneDepth, 1.0f));
	scenePosition.xyz = scenePosition.xyz / scenePosition.w + FrameBuffer::CameraPosAdjust.xyz;
	const float sceneLift = scenePosition.z - SampleLandHeight(scenePosition.xy);
	if (DistanceOutsideLoadedLand(scenePosition.xy) <= -TerrainLodOverlapMargin || sceneLift < -8.0f || sceneLift > TerrainLiftMax)
		return previous;

	// Terrain seen at a grazing angle recedes quickly up the screen, while rock, ledge, and trunk faces keep nearly
	// the same depth. Require at least the recession of a 45 degree slope across two rows.
	const float sceneViewDepth = FrameBuffer::CameraProj._m23 / (sceneDepth - FrameBuffer::CameraProj._m22);
	const float upperViewDepth = FrameBuffer::CameraProj._m23 / (SceneDepth.Load(int3(rootPixel.xy, 0), int2(0, -2)) - FrameBuffer::CameraProj._m22);
	if (upperViewDepth - sceneViewDepth < sceneViewDepth * 4.0f * dynamicResolutionInverted.y / abs(FrameBuffer::CameraProj._m11))
		return previous;

	// The vertical line above the root leaves the visible surface at the rendered terrain height.
	// A root still covered at the limit is behind a real hill instead.
	float lowLift = 0.0f;
	float highLift = min(sceneLift * 2.0f + 32.0f, TerrainLiftMax);
	const float4 limitPixel = ProjectToSceneDepth(root + float3(0.0f, 0.0f, highLift));
	if (limitPixel.w < 0.0f || SceneDepth.Load(int3(limitPixel.xy, 0)) < limitPixel.z)
		return previous;
	for (uint i = 0u; i < 5u; ++i) {
		const float probeLift = (lowLift + highLift) * 0.5f;
		const float4 probePixel = ProjectToSceneDepth(root + float3(0.0f, 0.0f, probeLift));
		if (SceneDepth.Load(int3(probePixel.xy, 0)) < probePixel.z)
			lowLift = probeLift;
		else
			highLift = probeLift;
	}
	// Use the first uncovered height; a lower estimate can bury short Far blades.
	return landHeight + highLift + 1.0f;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const int2 texel = dispatchID.xy;
	// Each texel holds the one cell of the camera-centred window that wraps onto it.
	const int2 cell = terrainLiftOrigin.xy + ((texel - terrainLiftOrigin.xy) & (TerrainLiftDim - 1));
	const int2 previousCell = terrainLiftOrigin.zw + ((texel - terrainLiftOrigin.zw) & (TerrainLiftDim - 1));
	const float2 world2D = (float2(cell) + 0.5f) * TerrainLiftCellSize;
	const float outsideDistance = DistanceOutsideLoadedLand(world2D);

	// A wrapped texel knows nothing about its new cell. Retain the LOD reference as LAND loads.
	const bool wrapped = any(cell != previousCell);
	float measured = wrapped ? InvalidSurfaceHeight : MeasuredHeight[texel];
	const float2 referenceOffset = abs(world2D - grassLodOrigin);
	const bool needsReference = GetTerrainLiftBlend(max(referenceOffset.x, referenceOffset.y)) > 0.0f;
	if (needsReference && outsideDistance > -TerrainLodOverlapMargin && (outsideDistance > 0.0f || measured >= InvalidSurfaceHeight) && (uint(texel.x & 1) | uint(texel.y & 1) << 1) == terrainLiftPhase) {
		// Keep a known reference after LAND loads, so only approach distance lowers its roots.
		measured = MeasureHeight(world2D, measured);
	}
	MeasuredHeight[texel] = measured;

	// Unknown cells stay on LAND. The generator caps interpolated lift at the cached surface height.
	const bool valid = measured < InvalidSurfaceHeight;
	const float landHeight = SampleLandHeight(world2D);
	AppliedLift[texel] = valid ? max(measured - landHeight, 0.0f) : 0.0f;
	AppliedHeight[texel] = valid ? measured : landHeight;
}
