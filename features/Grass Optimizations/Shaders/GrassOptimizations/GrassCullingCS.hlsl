#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

cbuffer CullParams : register(b0)
{
    float4 FrustumPlanes[6];

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
    float MeshLODPixelSize;

    float MeshLODBandPx;
    float HiZEnabled;
    float2 HiZSize;

    float HiZTexelPixels;
    float HiZMipCount;
    float2 _pad;
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
    // Window into SliceTable: the dispatch covers only these slices' combined instance count.
    uint SliceTableOffset;
    uint SliceCount;
    float2 _pad2;
};

ByteAddressBuffer Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);
// 1/16-res max-depth reduction of the scene depth copy (see GrassHiZCS.hlsl).
Texture2D<float> HiZ : register(t2);
// .x = first instance of the slice in the bucket's buffers, .y = running total of the visible slices before it. 
// Maps compacted thread indices to a real instance indices.
StructuredBuffer<uint2> SliceTable : register(t3);

RWByteAddressBuffer Compacted : register(u0);
RWStructuredBuffer<float4> Extras : register(u1);
RWByteAddressBuffer Counter : register(u2);

RWByteAddressBuffer LODCompacted : register(u3);
RWStructuredBuffer<float4> LODExtras : register(u4);
RWByteAddressBuffer LODCounter : register(u5);

// Converts a uint hash to a random float between [0, 1)
float RandFloat(uint bits)
{
    const uint mantissaMask = 0x007FFFFFu;
    const uint one = 0x3F800000u;

    bits &= mantissaMask;
    bits |= one;
    
    return asfloat(bits) - 1.0;
}

// Precalculate wind sway per-instance, so the vertex shader can just multiply by the wind vector and add to the position.
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

[numthreads(64, 1, 1)] void main(uint3 tid : SV_DispatchThreadID)
{
    const uint compactIdx = tid.x;
    if (compactIdx >= InstanceCount || SliceCount == 0)
        return;

    // Map the compacted index back to a real instance, searching on the running total in .y.
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

    // Keyed off the real index, so an instance keeps its dither decisions as slices come and go.
    const uint idx = slice.x + (compactIdx - slice.y);

    // Utilize one hash for both dithers, since pcg2d's outputs are independent
    const uint2 rand = Random::pcg2d(uint2(idx, 0u));

    const uint base = idx * 32;
    const uint4 raw0 = Instances.Load4(base);
    const uint4 raw1 = Instances.Load4(base + 16);

    const float2 localXY = float2(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16));
    const float localZ = f16tof32(raw0.y & 0xFFFF);

    const float4 og = Origins[idx];
    const float3 world = float3(localXY, localZ) + og.xyz;

    const float3 dv = world - FrameBuffer::CameraPosAdjust.xyz;
    const float distSq = dot(dv, dv);

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

    // projPx is the instance's on-screen radius with ProjScale coming from the FOV and resolution
    const float projPx = (ClumpRadius / dist) * ProjScale;
    const float pxScale = lerp(1.0, MinPixelScale, MeshCostBias);
    const float effMinPx = MinPixelSize * pxScale;
    if (projPx < effMinPx)
        return;

    if (HiZEnabled > 0.5)
    {
        const float4 clipC = mul(FrameBuffer::CameraViewProj, float4(dv, 1.0));
        if (clipC.w > 0.0)
        {
            const float2 uv = (clipC.xy / clipC.w) * float2(0.5, -0.5) + 0.5;
            const float2 tc = uv * HiZSize;
            const float rT = projPx / HiZTexelPixels;  // instance radius expressed in level-0 texels

            // The level where the instance spans ~2 texels, fixing the sample count. 
            const float wantLevel = ceil(log2(max(2.0 * rT, 1.0)));

            // Skip the test when the pyramid lacks the level this instance needs. A level that does not cover it would underestimate the max and cull visible grass.
            [branch] if (wantLevel <= HiZMipCount - 1.0)
            {
            const int level = (int)wantLevel;
            const float scale = exp2((float)level);
            const float2 tcL = tc / scale;
            const float rTL = rT / scale;
            const int2 dimL = max(int2(HiZSize / scale), int2(1, 1));

            const int2 t0 = int2(floor(tcL - rTL));
            const int2 t1 = int2(floor(tcL + rTL));

            // Pull dv back to the bounding sphere's near surface, since an instance hides only when even its closest point sits behind the occluder.
            // A camera inside the sphere shortens dv to nothing, putting nearZ far below any tile depth so the test never culls.
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

            // Behind the farthest occluder of every covering tile means fully hidden.
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
        const float h = RandFloat(rand.x);
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
    
    const float fade = distFade * spawnFade * lodFade * edgeFade;
    if (fade <= InvisibleFadeCull)
        return;

    const float basis = (localXY.x + localXY.y) * -0.0078125;

    const float4 e0 = float4(og.xyz, IsComplex);
    
    const float collisionFlag = (distSq < CollisionDistSq) ? 1.0 : 0.0;
    const float farFlag = (SimpleShadingPixelSize > 0.0 && projPx < SimpleShadingPixelSize) ? 2.0 : 0.0;

    const float4 e1 = float4(WindScalar(basis, TimeBase * WavePeriod), WindScalar(basis, PrevTimeBase * WavePeriod), fade, collisionFlag + farFlag);
    
    // Dither over MeshLODBandPx so there is a smooth transition between full-detail and LOD instances
    bool useLOD = false;
    if (LODEnabled > 0.5)
    {
        const float halfBand = MeshLODBandPx * 0.5;
        const float t = saturate((MeshLODPixelSize + halfBand - projPx) / max(MeshLODBandPx, 1e-4));
        useLOD = RandFloat(rand.y) < t;
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