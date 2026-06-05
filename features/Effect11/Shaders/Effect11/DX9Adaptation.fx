float4 AdaptationParameters;

Texture2D TextureCurrent;
Texture2D TexturePrevious;

SamplerState Sampler0
{
	Filter = MIN_MAG_MIP_POINT;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VS_INPUT_POST
{
	float3 pos : POSITION;
	float2 txcoord : TEXCOORD0;
};
struct VS_OUTPUT_POST
{
	float4 pos : SV_POSITION;
	float2 txcoord0 : TEXCOORD0;
};

VS_OUTPUT_POST VS_Quad(VS_INPUT_POST IN, uniform float sizeX, uniform float sizeY)
{
	VS_OUTPUT_POST o;
	o.pos = float4(IN.pos.xyz, 1.0);
	o.txcoord0 = IN.txcoord + float2(sizeX, sizeY);
	return o;
}

float4 PS_Downsample(VS_OUTPUT_POST IN) : SV_Target
{
	const float scale  = 1.0 / 16.0;
	const float step   = (1.0 / 16.0) * scale;        // 1/256 UV between taps
	const float origin = (-0.5 + 0.5 / 16.0) * scale; // scale folded in

	float3 sum = 0.0;
	[loop]
	for (int i = 0; i < 256; i++) {
		float2 uv = IN.txcoord0 + origin + float2(i & 15, i >> 4) * step;
		sum += TextureCurrent.SampleLevel(Sampler0, uv, 0).rgb;
	}

	float3 avg = sum * (1.0 / 256.0);
	float luma = max(avg.x, max(avg.y, avg.z));
	return float4(luma.xxx, 1.0);
}

float4 PS_Adaptation(VS_OUTPUT_POST IN) : SV_Target
{
	const float step   = 1.0 / 16.0;
	const float origin = -0.5 + 0.5 / 16.0;

	float prev = TexturePrevious.SampleLevel(Sampler0, IN.txcoord0, 0).x;

	float sum = 0.0;
	float maxLuma = 0.0;
	[loop]
	for (int i = 0; i < 256; i++) {
		float2 uv = IN.txcoord0 + origin + float2(i & 15, i >> 4) * step;
		float s = TextureCurrent.SampleLevel(Sampler0, uv, 0).x;
		sum += s;
		maxLuma = max(maxLuma, s);
	}

	float curr    = lerp(sum * (1.0 / 256.0), maxLuma, AdaptationParameters.z);
	float adapted = clamp(lerp(prev, curr, AdaptationParameters.w), 0.001, 16384.0);
	float result  = clamp(adapted, AdaptationParameters.x, AdaptationParameters.y);
	return float4(result.xxx, 1.0);
}

technique11 Downsample
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad(0.0, 0.0)));
		SetPixelShader(CompileShader(ps_5_0, PS_Downsample()));
	}
}

technique11 Draw
{
	pass p0
	{
		SetVertexShader(CompileShader(vs_5_0, VS_Quad(0.0, 0.0)));
		SetPixelShader(CompileShader(ps_5_0, PS_Adaptation()));
	}
}