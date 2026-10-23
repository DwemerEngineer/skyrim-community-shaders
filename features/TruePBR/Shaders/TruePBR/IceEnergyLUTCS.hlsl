// Bakes the ice energy compensation table for a rough GGX dielectric at IOR 1.31.
// Each texel stores 1 / (E_R + eta^2 * E_T), the inverse of the energy kept by single scattering (Turquin 2019).
// R is light entering the ice, G is light leaving it. x is NdotV, y is perceptual roughness.
// See: Sforza & Pellacini 2022, "Enforcing Energy Preservation in Microfacet Models"

#include "Common/BRDF.hlsli"
#include "Common/Math.hlsli"

static const uint LUTSize = 32;
static const uint NumSamples = 4096;
static const float InvNumSamples = 1.0 / float(NumSamples);
static const float IceIOR = 1.31;

RWTexture2D<float2> IceEnergyLUT : register(u0);

// Sample i-th point from Hammersley point set of NumSamples points total.
float2 SampleHammersley(uint i)
{
	return float2((i + 0.5) * InvNumSamples, reversebits(i) * 2.3283064365386963e-10);
}

// Height-uncorrelated Smith masking for direction d over microfacet normal m.
float SmithG1(float alpha, float3 d, float3 m)
{
	if (d.z * dot(d, m) <= 0.0)
		return 0.0;
	float a2 = alpha * alpha;
	float c2 = d.z * d.z;
	return 2.0 * abs(d.z) / (abs(d.z) + sqrt(c2 - a2 * c2 + a2));
}

// Sample the GGX distribution of visible normals.
// See: Heitz 2018, "Sampling the GGX Distribution of Visible Normals"
float3 SampleGGXVNDF(float3 V, float alpha, float2 u)
{
	float3 Vh = normalize(float3(alpha * V.x, alpha * V.y, V.z));
	float lengthSq = dot(Vh.xy, Vh.xy);
	float3 T1 = lengthSq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lengthSq) : float3(1.0, 0.0, 0.0);
	float3 T2 = cross(Vh, T1);
	float r = sqrt(u.x);
	float phi = Math::TAU * u.y;
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5 * (1.0 + Vh.z);
	t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
	float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
	return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(1e-7, Nh.z)));
}

// Directional albedo of the reflected and transmitted lobes.
// Visible normal sampling reduces the estimator weight to G1 of the outgoing direction.
float EnergyAlbedo(float NdotV, float roughness, float eta)
{
	float alpha = roughness * roughness;
	float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
	float albedo = 0.0;
	[loop] for (uint i = 0; i < NumSamples; i++)
	{
		float3 M = SampleGGXVNDF(V, alpha, SampleHammersley(i));
		float VdotM = dot(V, M);
		float F = BRDF::F_DielectricCos(eta, VdotM);
		float3 L = 2.0 * VdotM * M - V;
		albedo += F * SmithG1(alpha, L, M);
		float k = 1.0 - (1.0 - VdotM * VdotM) / (eta * eta);
		if (k > 0.0) {
			float3 T = -V / eta + (VdotM / eta - sqrt(k)) * M;
			albedo += (1.0 - F) * SmithG1(alpha, T, M);
		}
	}
	return albedo * InvNumSamples;
}

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (any(tid.xy >= LUTSize))
		return;

	float NdotV = clamp(tid.x / (LUTSize - 1.0), 0.02, 0.98);
	float roughness = max(tid.y / (LUTSize - 1.0), 0.035);
	float2 albedo = float2(EnergyAlbedo(NdotV, roughness, IceIOR), EnergyAlbedo(NdotV, roughness, 1.0 / IceIOR));
	IceEnergyLUT[tid.xy] = 1.0 / max(albedo, 1e-4);
}
