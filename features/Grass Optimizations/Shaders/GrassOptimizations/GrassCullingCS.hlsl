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
    float ProjScale;
    float MinPixelSize;
    float EdgeOnCos;
    float3 BandDistSq;
    float _pad2;
    row_major float4x4 ViewProj;
    float2 HiZDims;
    uint MaxHiZMip;
    uint HiZValid;
};

cbuffer CullBucket : register(b1)
{
    uint InstanceCount;
    float WavePeriod;
    float TimeBase;
    float PrevTimeBase;
    float3 BoundCenter;
    float ClumpRadius;
};

StructuredBuffer<float4> Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);
Texture2D<float> HiZ : register(t2);
SamplerState PointClamp : register(s0);

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

	// --- Hi-Z occlusion ---
    if (HiZValid)
    {
		// Bound sphere in world space. Grass extends up from its base, so the mesh
		// bound is NOT origin-centered — using the base as the center would leave
		// most of the blade outside the sphere and cull visible grass.
        const float3 boundWorld = world + BoundCenter;

        const float3 toCamDir = normalize(CameraPos - boundWorld);
        const float3 nearWorld = boundWorld + toCamDir * ClumpRadius; // nearest point

        const float4 clip = mul(ViewProj, float4(nearWorld, 1.0));
        if (clip.w > 0.0)
        {
            const float instZ = clip.z / clip.w; // standard Z: larger = farther
            const float2 uv = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;

			// screen footprint, padded — underestimating it can miss a far occluder
			// in the periphery and lower the sampled max → false culls
            const float4 clipC = mul(ViewProj, float4(boundWorld, 1.0));
            const float rPx = (ClumpRadius / max(clipC.w, 1e-4)) * ProjScale * 1.25;
            const float2 rUV = rPx / HiZDims;

            const uint mip = min((uint) ceil(log2(max(2.0 * rPx, 1.0))), MaxHiZMip);

            const float2 uvMin = saturate(uv - rUV);
            const float2 uvMax = saturate(uv + rUV);

            float tileMax = HiZ.SampleLevel(PointClamp, uvMin, mip);
            tileMax = max(tileMax, HiZ.SampleLevel(PointClamp, float2(uvMax.x, uvMin.y), mip));
            tileMax = max(tileMax, HiZ.SampleLevel(PointClamp, float2(uvMin.x, uvMax.y), mip));
            tileMax = max(tileMax, HiZ.SampleLevel(PointClamp, uvMax, mip));

			// Cull only if the instance's NEAREST point is still deeper than the
			// FARTHEST occluder covering its footprint → provably fully hidden.
            if (instZ > tileMax)
                return;
        }
    }

    const float basis = (d1.x + d1.y) * -0.0078125;
    WindScalars[idx] = float2(WindScalar(basis, TimeBase * WavePeriod),
	                          WindScalar(basis, PrevTimeBase * WavePeriod));

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