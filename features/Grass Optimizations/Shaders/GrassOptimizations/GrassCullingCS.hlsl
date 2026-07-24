#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"

cbuffer CullParams : register(b0)
{
    float4 FrustumPlanes[6];
    float MaxDistSq;
    float FullDetailPixelSize;
    float MeshCostBias;
    float LODMinKeep;
    float LODFadeBand;
    float ProjScale;
    float MinPixelSize;
    float EdgeFadeStart;
    float CollisionDistSq;
    float AlphaParam1;
    float AlphaParam2;
    float FadeNow;
    float FadeInTimeRcp;
    float InvisibleFadeCull;
    float MeshLODPixelSize;
    float MeshLODBandPx;
    float HiZEnabled;
    float2 HiZSize;
    float HiZTexelPixels;
    float HiZMipCount;
    // Below this projected size the pixel shader drops detail it cannot resolve. 0 = never.
    float SimpleShadingPixelSize;
    float2 hiZPad;
};

cbuffer CullBucket : register(b1)
{
    uint InstanceCount;
    float WavePeriod;
    float TimeBase;
    float PrevTimeBase;
    float3 BoundCenter;
    float ClumpRadius;
    float DistScale;
    float MinPixelScale;
    float IsComplex;
    float LODEnabled;
    // Window into SliceTable describing which slices of this bucket are worth visiting. The
    // dispatch covers only their combined instance count, so whole cells that are out of range or
    // behind the camera cost nothing at all rather than a thread each.
    uint SliceTableOffset;
    uint SliceCount;
    float2 _pad2;
};

ByteAddressBuffer Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);
// 1/16-res max-depth reduction of the scene depth copy (see GrassHiZCS.hlsl).
Texture2D<float> HiZ : register(t2);
// .x = first instance of the slice in the bucket's buffers, .y = running total of the visible
// slices before it. Lets a compacted thread index map back to a real instance index.
StructuredBuffer<uint2> SliceTable : register(t3);

RWByteAddressBuffer Compacted : register(u0);
RWStructuredBuffer<float4> Extras : register(u1);
RWByteAddressBuffer Counter : register(u2);

// Second bin, drawn with the low-poly LOD mesh. Bound as null when the bucket has no LOD mesh,
// in which case LODEnabled is 0 and nothing is ever routed here.
RWByteAddressBuffer LODCompacted : register(u3);
RWStructuredBuffer<float4> LODExtras : register(u4);
RWByteAddressBuffer LODCounter : register(u5);

float Hash(uint i)
{
    i = (i ^ 61u) ^ (i >> 16u);
    i *= 9u;
    i ^= i >> 4u;
    i *= 0x27d4eb2du;
    i ^= i >> 15u;
    return (float) (i & 0xFFFFFFu) / (float) 0x1000000u;
}

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

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint compactIdx = tid.x;
    // SliceCount 0 would make the search bound below underflow to 0xFFFFFFFF and read the table
    // out of bounds. The CPU never dispatches an empty bucket, so this only guards the invariant.
    if (compactIdx >= InstanceCount || SliceCount == 0)
        return;

    // Map the compacted index back to a real instance. Binary search over the visible slices —
    // at most a handful of steps, and it replaces threads that would otherwise have been spawned
    // for every instance in the bucket including whole cells nowhere near the view.
    uint lo = 0;
    uint hi = SliceCount - 1;
    [loop] while (lo < hi)
    {
        const uint mid = (lo + hi + 1) >> 1;
        if (SliceTable[SliceTableOffset + mid].y <= compactIdx)
            lo = mid;
        else
            hi = mid - 1;
    }
    const uint2 slice = SliceTable[SliceTableOffset + lo];

    // Everything downstream keys off the real index, including Hash() — a clump must keep the
    // same dither decision as slices come and go, or thinning would flicker.
    const uint idx = slice.x + (compactIdx - slice.y);

    const uint base = idx * 32;
    const uint4 raw0 = Instances.Load4(base);
    const uint4 raw1 = Instances.Load4(base + 16);

    const float2 localXY = float2(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16));
    const float localZ = f16tof32(raw0.y & 0xFFFF);

    const float4 og = Origins[idx];
    const float3 world = float3(localXY, localZ) + og.xyz;

    const float3 dv = world - FrameBuffer::CameraPosAdjust.xyz;
    const float distSq = dot(dv, dv);

    // Per-mesh cost weighting, blended by MeshCostBias: 0 = thresholds are literal (a "4 px"
    // setting means 4 px on every grass type), 1 = full sqrt(tris/6) weighting that culls
    // heavier meshes sooner. Applied to both the distance cap and the pixel thresholds.
    const float dScale = lerp(1.0, DistScale, MeshCostBias);
    const float scaleSq = dScale * dScale;
    const float effMaxDistSq = MaxDistSq * scaleSq;
    if (distSq > effMaxDistSq)
        return;

	[unroll]
    for (uint p = 0; p < 6; ++p)
    {
        if (dot(FrustumPlanes[p].xyz, world) - FrustumPlanes[p].w < 0.0)
            return;
    }

    const float dist = sqrt(distSq);

    // --- Projected-size LOD ---
    // projPx = this clump's on-screen radius in pixels (distance + clump size + FOV + resolution
    // via ProjScale, and per-type mesh cost via MinPixelScale). Full density above
    // FullDetailPixelSize, stochastically thinned toward LODMinKeep as it shrinks, culled below
    // MinPixelSize. Replaces the old raw-distance thinning with a resolution/FOV-aware metric.
    const float projPx = (ClumpRadius / dist) * ProjScale;
    const float pxScale = lerp(1.0, MinPixelScale, MeshCostBias);
    const float effMinPx = MinPixelSize * pxScale;
    if (projPx < effMinPx)
        return;

    // --- Hi-Z occlusion cull ---
    // Early-Z already rejects the FRAGMENTS of hidden grass, but only after the vertex shader has
    // run. Vertex work dominates the depth pass, so discarding hidden instances here is the part
    // the hardware cannot do for us.
    if (HiZEnabled > 0.5)
    {
        const float4 clipC = mul(FrameBuffer::CameraViewProj, float4(dv, 1.0));
        if (clipC.w > 0.0)
        {
            const float2 uv = (clipC.xy / clipC.w) * float2(0.5, -0.5) + 0.5;
            const float2 tc = uv * HiZSize;
            const float rT = projPx / HiZTexelPixels;  // clump radius expressed in level-0 texels

            // Pick the pyramid level where this clump spans at most ~2 texels, so the sample count
            // stays fixed no matter how large it projects. Near-camera clumps cover hundreds of
            // level-0 texels; a coarse texel is the exact max of all of them, so this loses no
            // conservatism — unlike sampling a subset of fine texels, which could underestimate
            // the max and cull visible grass.
            const float wantLevel = ceil(log2(max(2.0 * rT, 1.0)));

            // If the clump needs a coarser level than the pyramid has, skip the test rather than
            // testing against a level that does not cover it. Not culling is always safe; culling
            // on an underestimated max is not. This also covers the case where the mip reduction
            // failed to load and only level 0 holds valid data.
            [branch] if (wantLevel <= HiZMipCount - 1.0)
            {
            const int level = (int)wantLevel;
            const float scale = exp2((float)level);
            const float2 tcL = tc / scale;
            const float rTL = rT / scale;
            const int2 dimL = max(int2(HiZSize / scale), int2(1, 1));

            const int2 t0 = int2(floor(tcL - rTL));
            const int2 t1 = int2(floor(tcL + rTL));

            // Nearest point of the clump's bounding sphere, as NDC z. Camera inside the sphere
            // collapses this to the camera position, which never culls.
            const float3 dvNear = dv * (max(dist - ClumpRadius, 0.0) / max(dist, 1e-4));
            const float4 clipN = mul(FrameBuffer::CameraViewProj, float4(dvNear, 1.0));
            const float nearZ = clipN.z / max(clipN.w, 1e-4);

            float tileMax = 0.0;
			[unroll] for (int y = 0; y < 3; ++y)
            {
				[unroll] for (int x = 0; x < 3; ++x)
                {
                    if (t0.x + x <= t1.x && t0.y + y <= t1.y)
                    {
                        const int2 t = clamp(t0 + int2(x, y), int2(0, 0), dimL - 1);
                        tileMax = max(tileMax, HiZ.Load(int3(t, level)));
                    }
                }
            }

            // Nearest point still behind the farthest occluder in every covering tile means every
            // one of those tiles is fully closed off in front of this clump.
            if (nearZ > tileMax)
                return;
            }
        }
    }

    float lodFade = 1.0;
    const float effFullPx = FullDetailPixelSize * pxScale;
    if (projPx < effFullPx)
    {
        const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4));
        const float keep = lerp(1.0, LODMinKeep, t);
        const float h = Hash(idx);
        if (h > keep + LODFadeBand)
            return;
        lodFade = saturate((keep + LODFadeBand - h) / LODFadeBand);
    }

    const float maxDist = sqrt(effMaxDistSq);
    const float edgeStart = maxDist * EdgeFadeStart;
    const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

    const float4 clip = mul(FrameBuffer::CameraViewProj, float4(dv, 1.0));
    const float distFade = 1.0 - saturate((length(clip.xyz) - AlphaParam1) / AlphaParam2);
    const float spawnFade = saturate((FadeNow - og.w) * FadeInTimeRcp);

    // The depth PS discards when fade * baseAlpha < AlphaTestRefRS, so any instance whose fade
    // is at/below the alpha-test ref cannot pass even at baseAlpha == 1 — it is invisible but
    // would still be fully rasterized in both passes. Cull it here instead. At the default
    // threshold 0.0 this only removes exactly-zero-fade instances (provably invisible, e.g.
    // freshly spawned cells before fade-in starts); raising it toward the game's alpha-test ref
    // trims the invisible outer fade shell too.
    const float fade = distFade * spawnFade * lodFade * edgeFade;
    if (fade <= InvisibleFadeCull)
        return;

    const float basis = (localXY.x + localXY.y) * -0.0078125;

    const float4 e0 = float4(og.xyz, IsComplex);
    // e1.w packs two flags for the vertex shader: 1.0 = within collision range, 2.0 = far enough
    // that the pixel shader should skip detail work. Packed rather than given its own channel
    // because all eight Extras channels are already spoken for.
    const float collisionFlag = (distSq < CollisionDistSq) ? 1.0 : 0.0;
    const float farFlag = (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize) ? 2.0 : 0.0;

    const float4 e1 = float4(WindScalar(basis, TimeBase * WavePeriod), WindScalar(basis, PrevTimeBase * WavePeriod), fade, collisionFlag + farFlag);
    
    // --- Mesh-swap LOD bin selection ---
    // Below MeshLODPixelSize this instance is drawn with the low-poly LOD mesh instead. The swap
    // is dithered over MeshLODBandPx: t ramps 0 -> 1 across the band and each instance draws its
    // own hash, so a clump crossing the threshold converts gradually rather than flipping whole.
    // A different hash salt than the density thinning above keeps the two decisions uncorrelated.
    bool useLOD = false;
    if (LODEnabled > 0.5)
    {
        const float halfBand = MeshLODBandPx * 0.5;
        const float t = saturate((MeshLODPixelSize + halfBand - projPx) / max(MeshLODBandPx, 1e-4));
        useLOD = Hash(idx ^ 0x9E3779B9u) < t;
    }

    uint slot;
    if (useLOD)
    {
        LODCounter.InterlockedAdd(0, 1, slot);
        LODCompacted.Store4(slot * 32, raw0);
        LODCompacted.Store4(slot * 32 + 16, raw1);
        LODExtras[slot * 2 + 0] = e0;
        LODExtras[slot * 2 + 1] = e1;
    }
    else
    {
        Counter.InterlockedAdd(0, 1, slot);
        Compacted.Store4(slot * 32, raw0);
        Compacted.Store4(slot * 32 + 16, raw1);
        Extras[slot * 2 + 0] = e0;
        Extras[slot * 2 + 1] = e1;
    }
}