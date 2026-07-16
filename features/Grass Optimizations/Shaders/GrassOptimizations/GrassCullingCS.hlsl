#include "Common/Math.hlsli"

cbuffer CullParams : register(b0)
{
    float4 FrustumPlanes[6];
    float3 CameraPos;
    uint _pad0;
    float MaxDistSq;
    float pad1;
    float LODNearDistSq;
    float LODFarDistSq;
    float LODMinKeep;
    float ClumpRadius;
    float ProjScale;
    float MinPixelSize;
    float3 BandDistSq;
    float _pad2;
};

cbuffer CullBucket : register(b1)
{
    uint InstanceCount;
    float WavePeriod;
    float TimeBase;
    float PrevTimeBase;
};

StructuredBuffer<float4> Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);

AppendStructuredBuffer<uint> Visible0 : register(u0); // nearest
AppendStructuredBuffer<uint> Visible1 : register(u1);
AppendStructuredBuffer<uint> Visible2 : register(u2);
AppendStructuredBuffer<uint> Visible3 : register(u3); // farthest
RWStructuredBuffer<float2> WindScalars : register(u4);

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

    const float4 d1 = Instances[idx * 4];
    const float3 local = d1.xyz;
    const float3 world = local + Origins[idx].xyz;

    const float3 dv = world - CameraPos;
    const float distSq = dot(dv, dv);
    if (distSq > MaxDistSq)
        return;

    if (distSq > LODNearDistSq)
    {
        const float t = saturate((distSq - LODNearDistSq) / (LODFarDistSq - LODNearDistSq));
        if (Hash(idx) > lerp(1.0, LODMinKeep, t))
            return;
    }

	[unroll]
    for (uint p = 0; p < 6; ++p)
    {
        if (dot(FrustumPlanes[p].xyz, world) - FrustumPlanes[p].w < 0.0)
            return;
    }

    if ((ClumpRadius / sqrt(distSq)) * ProjScale < MinPixelSize)
        return;

	// survivor: wind is only ever read for these
    const float basis = (d1.x + d1.y) * -0.0078125;
    WindScalars[idx] = float2(WindScalar(basis, TimeBase * WavePeriod), WindScalar(basis, PrevTimeBase * WavePeriod));

	// distance band → draw order. Dynamic UAV indexing needs SM5.1, so branch.
    uint band = 0;
    if (distSq > BandDistSq.x)
        band = 1;
    if (distSq > BandDistSq.y)
        band = 2;
    if (distSq > BandDistSq.z)
        band = 3;

    if (band == 0)
        Visible0.Append(idx);
    else if (band == 1)
        Visible1.Append(idx);
    else if (band == 2)
        Visible2.Append(idx);
    else
        Visible3.Append(idx);
}