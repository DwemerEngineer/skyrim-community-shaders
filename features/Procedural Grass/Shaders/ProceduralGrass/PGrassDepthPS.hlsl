// Keep the faded blade base out of the depth buffer so terrain remains visible through the blend.

#include "ProceduralGrass/PGrassCommon.hlsli"

struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float BladeHeight : TEXCOORD0;
};

void main(PS_INPUT input)
{
	float groundProximity = 1.0 - smoothstep(0.0, max(grassTerrainBlend.y, 0.01), input.BladeHeight);
	float opacity = 1.0 - groundProximity * grassTerrainBlend.x;
	clip(opacity - 0.999);
}
