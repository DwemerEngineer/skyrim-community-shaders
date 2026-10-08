// Rasterises world geometry into the top-down height map, or with GRASS_SURFACE into the topmost grass surface record.
// Maps world coordinates to clip space with a subtract and a divide.

cbuffer HeightCB : register(b0)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;
	float2 WindowCentre;
	float HalfExtent;
	float Padding;
	float4 GrassSurfaceParams;  // Type, density, lower height bound, inverse height range.
#if defined(GRASS_SURFACE_ALPHA_TEST)
	float4 GrassAlphaParams;  // Alpha threshold, material alpha, use vertex alpha, unused.
	float4 GrassUVTransform;  // UV scale XY and offset XY.
#endif
};

struct HeightOutput
{
	float4 Position: SV_POSITION;
	float3 WorldPosition: TEXCOORD0;
#if defined(GRASS_SURFACE_ALPHA_TEST)
	float2 UV: TEXCOORD1;
	float Alpha: TEXCOORD2;
#endif
};

#if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION;
#	if defined(GRASS_SURFACE_ALPHA_TEST)
	float2 UV: TEXCOORD0;
	float4 Color: COLOR0;
#	endif
};

HeightOutput main(VS_INPUT input)
{
	float4 localPos = float4(input.Position.xyz, 1.0f);
	float3 worldPos = float3(dot(WorldRow0, localPos), dot(WorldRow1, localPos), dot(WorldRow2, localPos));

	HeightOutput output;
	output.Position = float4((worldPos.xy - WindowCentre) / HalfExtent * float2(1.0f, -1.0f), 0.5f, 1.0f);  // Flip y so world +Y runs down the texture.
	output.WorldPosition = worldPos;
#	if defined(GRASS_SURFACE_ALPHA_TEST)
	output.UV = input.UV * GrassUVTransform.xy + GrassUVTransform.zw;
	output.Alpha = GrassAlphaParams.y * lerp(1.0f, input.Color.a, GrassAlphaParams.z);
#	endif
	return output;
}

#elif defined(GRASS_SURFACE)
#	if defined(GRASS_SURFACE_ALPHA_TEST)
Texture2D<float4> GrassAlphaTexture : register(t0);
SamplerState GrassAlphaSampler : register(s0);
#	endif

struct PS_OUTPUT
{
	float4 Surface: SV_Target0;
	float Depth: SV_Depth;
};

PS_OUTPUT main(HeightOutput input, bool frontFace : SV_IsFrontFace)
{
	float3 normal = normalize(cross(ddy(input.WorldPosition), ddx(input.WorldPosition)));
#	if defined(GRASS_SURFACE_ALPHA_TEST)
	// Source alpha preserves small leaves in the coarse top-down map.
	clip(GrassAlphaTexture.SampleLevel(GrassAlphaSampler, input.UV, 0).a * input.Alpha - GrassAlphaParams.x);
#	endif
	normal *= normal.z < 0.0f ? -1.0f : 1.0f;
	uint type = frontFace && normal.z > 0.01f ? (uint)GrassSurfaceParams.x : 0u;
	uint density = (uint)round(saturate(GrassSurfaceParams.y) * 255.0f);

	PS_OUTPUT output;
	output.Surface = float4(input.WorldPosition.z, normal.xy, float(type | density << 8));
	// Depth selects the complete highest surface record, including zero-type blockers.
	output.Depth = saturate(1.0f - (input.WorldPosition.z - GrassSurfaceParams.z) * GrassSurfaceParams.w);
	return output;
}

#else
struct PS_OUTPUT
{
	float Highest: SV_Target0;
	float Lowest: SV_Target1;
};

PS_OUTPUT main(HeightOutput input)
{
	PS_OUTPUT output;
	output.Highest = input.WorldPosition.z;
	output.Lowest = input.WorldPosition.z;
	return output;
}
#endif
