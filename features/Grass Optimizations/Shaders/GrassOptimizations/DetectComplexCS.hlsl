// Decides whether a grass diffuse is "complex" — i.e. its lower half holds a normal/specular map
// rather than more colour. Probing the bottom-left texel and decoding it as a normal is enough:
// only a real normal map decodes to unit length there, so |length(rgb * 2 - 1) - 1| is the test.
// Threshold is the user's ComplexGrassThreshold, since compression perturbs the length slightly.
//
// Run once per unique texture at cell load, never per frame — it stalls on a readback.

Texture2D<float4> BaseTex : register(t0);
RWStructuredBuffer<uint> Result : register(u0);

cbuffer DetectParams : register(b0)
{
    uint TexHeight;
    float Threshold;
    uint2 _pad;
};

[numthreads(1, 1, 1)]
void main()
{
    float3 complexTest = BaseTex.Load(int3(0, (int) TexHeight - 1, 0)).xyz * 2.0 - 1.0;
    float complexLength = length(complexTest);
    Result[0] = (abs(complexLength - 1.0) < Threshold) ? 1u : 0u;
}