// Rasterises world geometry into the top-down height map. Maps world coordinates to clip space with a subtract and a divide.

cbuffer HeightCB : register(b0)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;
	float2 WindowCentre;
	float HalfExtent;
	float Padding;
};

struct VS_INPUT
{
	float4 Position : POSITION;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input)
{
	float4 localPos = float4(input.Position.xyz, 1.0f);
	float3 worldPos = float3(dot(WorldRow0, localPos), dot(WorldRow1, localPos), dot(WorldRow2, localPos));

	VS_OUTPUT output;
    output.Position = float4((worldPos.xy - WindowCentre) / HalfExtent * float2(1.0f, -1.0f), 0.5f, 1.0f); // Flip y so world +Y runs down the texture.
	output.WorldZ = worldPos.z;
	return output;
}
