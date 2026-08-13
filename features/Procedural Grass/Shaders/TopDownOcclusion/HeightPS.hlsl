// Writes world Z. The target's max blend keeps the highest surface per texel.

struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

float main(PS_INPUT input) : SV_Target0
{
	return input.WorldZ;
}
