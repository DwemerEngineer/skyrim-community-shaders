#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

cbuffer CullParams : register(b0)
{
	float4 FrustumPlanes[6];

	// Use the main camera's projection because PerFrame contains the light's camera during shadow passes.
	float4 CameraViewProj[4];
	float4 CameraPos;

	float MinPixelSize;
	float FullDetailPixelSize;
	float LODMinKeep;
	float LODFadeBand;

	float MeshCostBias;
	float ProjScale;
	float MaxDistSq;
	float EdgeFadeStart;

	float AlphaParam1;
	float AlphaParam2;
	float FadeNow;
	float FadeInTimeRcp;

	float InvisibleFadeCull;
	float SimpleShadingPixelSize;
	float CollisionDistSq;
	float MidLODPixelSize;

	float MeshLODBandPx;
	float HiZEnabled;
	float2 HiZSize;

	float HiZTexelPixels;
	float HiZMipCount;
	float OcclusionBias;
	float CostBiasStartDist;

	float FarLODPixelSize;
	float3 _pad0;
};

cbuffer CullBucket : register(b1)
{
	uint GroupCount;
	float WavePeriod;
	float TimeBase;
	float PrevTimeBase;
	float3 BoundCenter;
	float ModelRadius;
	float DistScale;
	float MinPixelScale;
	float ComplexThreshold;
	float MidLODEnabled;
	// The dispatch covers the groups of the visible runs in this slice range.
	uint SliceTableOffset;
	uint SliceCount;
	float FarLODEnabled;
	// Split large buckets across rows to stay within the 65,535-group dispatch limit.
	uint DispatchWidth;
};

// Each group covers up to 64 contiguous instances from one slice.
struct CullGroup
{
	float3 Origin;
	float FadeStart;
	uint SourceStart;
	uint Count;
};

ByteAddressBuffer Instances : register(t0);
// Cache every run's groups until the bucket's slices change.
StructuredBuffer<CullGroup> Groups : register(t1);
// Scene-depth max pyramid; see GrassHiZCS.hlsl.
Texture2D<float> HiZ : register(t2);
// Each entry stores the run's first source group in .x and its first dispatch-group index in .y.
StructuredBuffer<uint2> SliceTable : register(t3);
StructuredBuffer<float> TextureNormalLength : register(t4);

RWByteAddressBuffer Compacted : register(u0);
RWStructuredBuffer<float4> Extras : register(u1);
RWByteAddressBuffer Counter : register(u2);

RWByteAddressBuffer MidLODCompacted : register(u3);
RWStructuredBuffer<float4> MidLODExtras : register(u4);

RWByteAddressBuffer FarLODCompacted : register(u5);
RWStructuredBuffer<float4> FarLODExtras : register(u6);
// Both LOD counts share one UAV so the shader stays within D3D11's eight-UAV limit.
RWByteAddressBuffer LODCounters : register(u7);

static const uint MiddleLODCountOffset = 0;
static const uint FarLODCountOffset = 4;

static const uint TierCount = 3;
static const uint TierCulled = TierCount;

// Shared survivor counts and output offsets let each group issue one global atomic per tier.
groupshared uint GroupTierCount[TierCount];
groupshared uint GroupTierBase[TierCount];

// Convert hashed bits to a uniform value in [0, 1).
float RandFloat(uint bits)
{
	const uint mantissaMask = 0x007FFFFFu;
	const uint one = 0x3F800000u;

	bits &= mantissaMask;
	bits |= one;

	return asfloat(bits) - 1.0;
}

// Precompute the per-instance wind term used by the vertex shader.
float WindScalar(float basis, float timer)
{
	const float a = 0.4 * (basis + timer);
	float sa, ca;
	sincos(a, sa, ca);
	const float t3 = 0.2 * cos(Math::PI * ca);
	const float t1 = sin(Math::PI * sa);
	const float t2 = sin(Math::TAU * sa);
	return (t1 + t2) * 0.3 + t3;
}

// Project a camera-relative direction without the matrix's translation column.
float4 ProjectDirection(float3 v)
{
	return float4(dot(CameraViewProj[0].xyz, v), dot(CameraViewProj[1].xyz, v), dot(CameraViewProj[2].xyz, v), dot(CameraViewProj[3].xyz, v));
}

// Return the instance's LOD tier or TierCulled.
uint CullInstance(uint idx, CullGroup group, out uint4 raw0, out uint4 raw1, out float4 e0, out float4 e1)
{
	raw1 = 0;
	e0 = 0.0;
	e1 = 0.0;

	const uint base = idx * 32;
	raw0 = Instances.Load4(base);

	const float2 localXY = float2(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16));
	const float localZ = f16tof32(raw0.y & 0xFFFF);

	const float3 world = float3(localXY, localZ) + group.Origin;

	const float3 dv = world - CameraPos.xyz;
	const float distSq = dot(dv, dv);

	const float dist = sqrt(distSq);

	// Ramp the mesh-cost bias in beyond CostBiasStartDist.
	const float costRamp = saturate((dist - CostBiasStartDist) / max(CostBiasStartDist, 1e-4));
	const float effCostBias = MeshCostBias * costRamp;

	const float dScale = lerp(1.0, DistScale, effCostBias);
	const float effMaxDistSq = MaxDistSq * dScale * dScale;
	if (distSq > effMaxDistSq)
		return TierCulled;

	[unroll] for (uint p = 0; p < 6; ++p)
	{
		if (dot(FrustumPlanes[p].xyz, world) - FrustumPlanes[p].w < 0.0)
			return TierCulled;
	}

	raw1 = Instances.Load4(base + 16);

	// Conservatively approximate the vertex shader's ScaleMask-based size variation.
	const float sizeVariance = f16tof32(raw1.z >> 16);
	const float instanceRadius = ModelRadius * (1.0 + max(sizeVariance, 0.0));

	const float projPx = (instanceRadius / dist) * ProjScale;
	const float pxScale = lerp(1.0, MinPixelScale, effCostBias);
	const float effMinPx = MinPixelSize * pxScale;
	if (projPx < effMinPx)
		return TierCulled;

	// Generate independent density and LOD dither values with one hash.
	const uint2 rand = Random::pcg2d(uint2(idx, 0u));

	float lodFade = 1.0;
	const float effFullPx = FullDetailPixelSize * pxScale;
	if (projPx < effFullPx) {
		const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4));
		const float keep = lerp(1.0, LODMinKeep, t);
		const float h = RandFloat(rand.x);
		if (h > keep + LODFadeBand)
			return TierCulled;
		lodFade = saturate((keep + LODFadeBand - h) / LODFadeBand);
	}

	const float maxDist = sqrt(effMaxDistSq);
	const float edgeStart = maxDist * EdgeFadeStart;
	const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

	const float4 clipOffset = float4(CameraViewProj[0].w, CameraViewProj[1].w, CameraViewProj[2].w, CameraViewProj[3].w);
	const float4 clip = ProjectDirection(dv) + clipOffset;
	const float distFade = 1.0 - saturate((length(clip.xyz) - AlphaParam1) / AlphaParam2);
	const float spawnFade = saturate((FadeNow - group.FadeStart) * FadeInTimeRcp);

	const float fade = distFade * spawnFade * lodFade * edgeFade;
	if (fade <= InvisibleFadeCull)
		return TierCulled;

	// Reject density and faded instances before sampling depth.
	if (HiZEnabled > 0.5) {
		// Transform the model-space bounding-sphere center into instance space.
		const float3 rot0 = float3(f16tof32(raw0.z & 0xFFFF), f16tof32(raw0.z >> 16), f16tof32(raw0.w & 0xFFFF));
		const float3 rot1 = float3(f16tof32(raw1.x & 0xFFFF), f16tof32(raw1.x >> 16), f16tof32(raw1.y & 0xFFFF));
		const float3 rot2 = float3(f16tof32(raw1.z & 0xFFFF), f16tof32(raw0.w >> 16), f16tof32(raw1.y >> 16));

		const float3 msCentre = BoundCenter * (1.0 + max(sizeVariance, 0.0));
		const float3 centreOffset = float3(dot(rot0, msCentre), dot(rot1, msCentre), dot(rot2, msCentre));
		const float3 dvC = dv + centreOffset;

		const float occRadius = instanceRadius + length(BoundCenter) * abs(sizeVariance);
		const float distC = max(length(dvC), 1e-4);
		const float projPxOcc = (occRadius / distC) * ProjScale;

		// The projection is linear, so offset the instance's clip position rather than projecting again.
		const float4 clipC = clip + ProjectDirection(centreOffset);
		if (clipC.w > 0.0) {
			const float2 uv = (clipC.xy / clipC.w) * float2(0.5, -0.5) + 0.5;
			const float2 tc = uv * HiZSize;
			// Measure the projected radius in level-0 Hi-Z texels.
			const float rT = projPxOcc / HiZTexelPixels;

			// Select a mip coarse enough to cover the sphere with a 2x2 footprint.
			float wantLevel = ceil(log2(max(2.0 * rT, 1.0)));
			float scale = exp2(wantLevel);
			float2 tcL = tc / scale;
			float rTL = rT / scale;
			int2 t0 = int2(floor(tcL - rTL));
			int2 t1 = int2(floor(tcL + rTL));

			// Rounding can leave the footprint one texel too wide; the next mip covers it.
			[branch] if (any(t1 - t0 > 1))
			{
				wantLevel += 1.0;
				scale *= 2.0;
				tcL *= 0.5;
				rTL *= 0.5;
				t0 = int2(floor(tcL - rTL));
				t1 = int2(floor(tcL + rTL));
			}

			// Skip occlusion when no available mip can cover the sphere safely.
			[branch] if (wantLevel <= HiZMipCount - 1.0)
			{
				const int level = (int)wantLevel;
				const int2 dimL = max(int2(ceil(HiZSize / scale)), int2(1, 1));
				const int2 lo = clamp(t0, int2(0, 0), dimL - 1);
				const int2 hi = clamp(t1, int2(0, 0), dimL - 1);

				// Test the sphere's nearest point; a camera inside it yields a non-occluded depth.
				// The nearest point scales dvC, so its clip position follows from clipC.
				const float nearScale = max(distC - occRadius, 0.0) / distC;
				const float4 clipN = (clipC - clipOffset) * nearScale + clipOffset;
				const float nearZ = clipN.z / max(clipN.w, 1e-4);

				const float tileMax = max(max(HiZ.Load(int3(lo, level)), HiZ.Load(int3(hi.x, lo.y, level))),
					max(HiZ.Load(int3(lo.x, hi.y, level)), HiZ.Load(int3(hi, level))));

				// Cull only when the sphere is behind every sampled tile, allowing for depth error.
				if (nearZ > tileMax + OcclusionBias)
					return TierCulled;
			}
		}
	}

	const float basis = (localXY.x + localXY.y) * -0.0078125;

	const float normalLength = TextureNormalLength[0];
	const float isComplex = normalLength > 0.0 ? (float)(abs(normalLength - 1.0) < ComplexThreshold) : -1.0;
	e0 = float4(group.Origin, isComplex);

	const float collisionFlag = (distSq < CollisionDistSq) ? 1.0 : 0.0;
	const float farFlag = (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize) ? 2.0 : 0.0;

	// Dither both LOD transitions with the same value to keep tier changes gradual and ordered.
	const float h = RandFloat(rand.y);
	const float halfBand = MeshLODBandPx * 0.5;
	const float bandRcp = 1.0 / max(MeshLODBandPx, 1e-4);

	uint tier = 0;
	if (MidLODEnabled > 0.5 && h < saturate((MidLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 1;
	if (FarLODEnabled > 0.5 && h < saturate((FarLODPixelSize + halfBand - projPx) * bandRcp))
		tier = 2;

	// Store the tier above the two low flag bits in e1.w.
	e1 = float4(WindScalar(basis, TimeBase * WavePeriod), WindScalar(basis, PrevTimeBase * WavePeriod), fade, collisionFlag + farFlag + 4.0 * (float)tier);
	return tier;
}

[numthreads(64, 1, 1)] void main(uint3 groupId : SV_GroupID, uint threadIdx : SV_GroupIndex) {
	if (threadIdx < TierCount)
		GroupTierCount[threadIdx] = 0;
	GroupMemoryBarrierWithGroupSync();

	uint tier = TierCulled;
	uint4 raw0 = 0;
	uint4 raw1 = 0;
	float4 e0 = 0.0;
	float4 e1 = 0.0;

	const uint groupIdx = groupId.y * DispatchWidth + groupId.x;
	if (groupIdx < GroupCount) {
		// Find the visible run containing this group; every thread in the group takes the same path.
		uint lo = 0;
		uint hi = SliceCount - 1;
		[loop] while (lo < hi)
		{
			const uint mid = (lo + hi + 1) >> 1;
			if (SliceTable[SliceTableOffset + mid].y <= groupIdx)
				lo = mid;
			else
				hi = mid - 1;
		}
		const uint2 run = SliceTable[SliceTableOffset + lo];

		const CullGroup group = Groups[run.x + (groupIdx - run.y)];
		// Use the source index to keep dither decisions stable across slice changes.
		if (threadIdx < group.Count)
			tier = CullInstance(group.SourceStart + threadIdx, group, raw0, raw1, e0, e1);
	}

	uint localSlot = 0;
	if (tier != TierCulled)
		InterlockedAdd(GroupTierCount[tier], 1, localSlot);
	GroupMemoryBarrierWithGroupSync();

	if (threadIdx < TierCount && GroupTierCount[threadIdx] != 0) {
		uint base;
		if (threadIdx == 0)
			Counter.InterlockedAdd(0, GroupTierCount[0], base);
		else if (threadIdx == 1)
			LODCounters.InterlockedAdd(MiddleLODCountOffset, GroupTierCount[1], base);
		else
			LODCounters.InterlockedAdd(FarLODCountOffset, GroupTierCount[2], base);
		GroupTierBase[threadIdx] = base;
	}
	GroupMemoryBarrierWithGroupSync();

	if (tier == TierCulled)
		return;

	const uint slot = GroupTierBase[tier] + localSlot;
	if (tier == 2) {
		FarLODCompacted.Store4(slot * 32, raw0);
		FarLODCompacted.Store4(slot * 32 + 16, raw1);
		FarLODExtras[slot * 2 + 0] = e0;
		FarLODExtras[slot * 2 + 1] = e1;
	} else if (tier == 1) {
		MidLODCompacted.Store4(slot * 32, raw0);
		MidLODCompacted.Store4(slot * 32 + 16, raw1);
		MidLODExtras[slot * 2 + 0] = e0;
		MidLODExtras[slot * 2 + 1] = e1;
	} else {
		Compacted.Store4(slot * 32, raw0);
		Compacted.Store4(slot * 32 + 16, raw1);
		Extras[slot * 2 + 0] = e0;
		Extras[slot * 2 + 1] = e1;
	}
}
