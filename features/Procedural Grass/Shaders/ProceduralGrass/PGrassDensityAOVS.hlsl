// Fullscreen triangle for the terrain-darkening pass.

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
};

VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT o;
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	o.Position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
