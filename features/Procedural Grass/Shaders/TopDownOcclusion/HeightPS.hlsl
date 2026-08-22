struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

struct PS_OUTPUT
{
	float Highest : SV_Target0;
	float Lowest : SV_Target1;
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT output;
	output.Highest = input.WorldZ;
	output.Lowest = input.WorldZ;
	return output;
}
