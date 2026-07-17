#include "Common/Math.hlsli"

cbuffer CullParams : register(b0)
{
    float4 FrustumPlanes[6];
    float3 CameraPos;
    uint _pad0;
    float MaxDistSq;
    float LODNearDistSq;
    float LODFarDistSq;
    float LODMinKeep;
    float LODFadeBand;
    float ProjScale;
    float MinPixelSize;
    float BandDistSq;
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
    float2 _pad1;
};

StructuredBuffer<float4> Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);

AppendStructuredBuffer<uint> VisibleNear : register(u0);
AppendStructuredBuffer<uint> VisibleFar : register(u1);
RWStructuredBuffer<float2> WindScalars : register(u2);
RWStructuredBuffer<float> LodFades : register(u3);

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
    const float3 world = d1.xyz + Origins[idx].xyz;

    const float3 dv = world - CameraPos;
    const float distSq = dot(dv, dv);

	// heavy meshes stop sooner: DistScale < 1 for high vertex counts
    const float scaleSq = DistScale * DistScale;
    if (distSq > MaxDistSq * scaleSq)
        return;

	// LOD thinning with a fade band: instances leave gradually via Color.w instead of
	// popping at the hash boundary, which is what capped how low LODMinKeep could go.
    float lodFade = 1.0;
    const float effNearSq = LODNearDistSq * scaleSq;
    if (distSq > effNearSq)
    {
        const float effFarSq = LODFarDistSq * scaleSq;
        const float t = saturate((distSq - effNearSq) / max(effFarSq - effNearSq, 1e-4));
        const float keep = lerp(1.0, LODMinKeep, t);
        const float h = Hash(idx);
        if (h > keep + LODFadeBand)
            return; // fully faded — costs no VS at all
        lodFade = saturate((keep + LODFadeBand - h) / LODFadeBand);
    }

	[unroll]
    for (uint p = 0; p < 6; ++p)
    {
        if (dot(FrustumPlanes[p].xyz, world) - FrustumPlanes[p].w < 0.0)
            return;
    }

    if ((ClumpRadius / sqrt(distSq)) * ProjScale < MinPixelSize * MinPixelScale)
        return;

	// survivor: these are only ever read for appended instances
    const float basis = (d1.x + d1.y) * -0.0078125;
    WindScalars[idx] = float2(WindScalar(basis, TimeBase * WavePeriod),
	                          WindScalar(basis, PrevTimeBase * WavePeriod));
    LodFades[idx] = lodFade;

    if (distSq > BandDistSq)
        VisibleFar.Append(idx);
    else
        VisibleNear.Append(idx);
}