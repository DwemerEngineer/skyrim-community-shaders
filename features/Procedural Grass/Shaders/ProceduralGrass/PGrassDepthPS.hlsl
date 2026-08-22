// Depth-prepass PS that dither-discards a blade's base in the same way as the colour pass to maintain depth consistency for blending with the terrain

#include "Common/Random.hlsli"

#include "ProceduralGrass/PGrassCommon.hlsli"

struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float BladeHeight : TEXCOORD0;
};

void main(PS_INPUT input)
{
	float groundProximity = 1.0 - saturate(input.BladeHeight / max(grassTerrainBlend.y, 0.01));
	float dissolve = groundProximity * grassTerrainBlend.x;
	float dither = Random::InterleavedGradientNoise(input.Position.xy, 0);
	clip(dither - dissolve);
}
