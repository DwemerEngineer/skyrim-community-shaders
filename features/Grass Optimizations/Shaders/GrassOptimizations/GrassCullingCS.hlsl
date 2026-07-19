#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"

#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"

cbuffer CullParams : register(b0)
{
    float4 FrustumPlanes[6];
    float MaxDistSq;
    float LODNearDistSq;
    float LODFarDistSq;
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
    float3 pad;
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
    float _pad1;
};

ByteAddressBuffer Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);

RWByteAddressBuffer Compacted : register(u0);
RWStructuredBuffer<float4> Extras : register(u1);
RWByteAddressBuffer Counter : register(u2);

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
    const uint idx = tid.x;
    if (idx >= InstanceCount)
        return;

    const uint base = idx * 32;
    const uint4 raw0 = Instances.Load4(base);
    const uint4 raw1 = Instances.Load4(base + 16);

    const float2 localXY = float2(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16));
    const float localZ = f16tof32(raw0.y & 0xFFFF);

    const float4 og = Origins[idx];
    const float3 world = float3(localXY, localZ) + og.xyz;

    const float3 dv = world - FrameBuffer::CameraPosAdjust.xyz;
    const float distSq = dot(dv, dv);

    const float scaleSq = DistScale * DistScale;
    const float effMaxDistSq = MaxDistSq * scaleSq;
    if (distSq > effMaxDistSq)
        return;

    float lodFade = 1.0;
    const float effNearSq = LODNearDistSq * scaleSq;
    if (distSq > effNearSq)
    {
        const float effFarSq = LODFarDistSq * scaleSq;
        const float t = saturate((distSq - effNearSq) / max(effFarSq - effNearSq, 1e-4));
        const float keep = lerp(1.0, LODMinKeep, t);
        const float h = Hash(idx);
        if (h > keep + LODFadeBand)
            return;
        lodFade = saturate((keep + LODFadeBand - h) / LODFadeBand);
    }
    
	[unroll]
    for (uint p = 0; p < 6; ++p)
    {
        if (dot(FrustumPlanes[p].xyz, world) - FrustumPlanes[p].w < 0.0)
            return;
    }

    const float dist = sqrt(distSq);
    if ((ClumpRadius / dist) * ProjScale < MinPixelSize * MinPixelScale)
        return;

    const float maxDist = sqrt(effMaxDistSq);
    const float edgeStart = maxDist * EdgeFadeStart;
    const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));

    const float4 clip = mul(FrameBuffer::CameraViewProj, float4(dv, 1.0));
    const float distFade = 1.0 - saturate((length(clip.xyz) - AlphaParam1) / AlphaParam2);
    const float spawnFade = saturate((FadeNow - og.w) * FadeInTimeRcp);

    const float basis = (localXY.x + localXY.y) * -0.0078125;

    const float4 e0 = float4(og.xyz, IsComplex);
    const float4 e1 = float4(WindScalar(basis, TimeBase * WavePeriod), WindScalar(basis, PrevTimeBase * WavePeriod), distFade * spawnFade * lodFade * edgeFade, (distSq < CollisionDistSq) ? 1.0 : 0.0);
    
    uint slot;
    Counter.InterlockedAdd(0, 1, slot);
    Compacted.Store4(slot * 32, raw0);
    Compacted.Store4(slot * 32 + 16, raw1);
    Extras[slot * 2 + 0] = e0;
    Extras[slot * 2 + 1] = e1;
}