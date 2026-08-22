// source unpadded heights
Texture2D<float> InHigh : register(t0); 
Texture2D<float> InLow : register(t1);
// output padded heights
RWTexture2D<float> OutHigh : register(u0);
RWTexture2D<float> OutLow : register(u1);

cbuffer PadCB : register(b0)
{
	int4 padParams;  // xy: pass axis (1,0) horizontal / (0,1) vertical; z: radius in texels
	uint4 padDim;
}

static const float EMPTY_HIGH = -1.0e30f;
static const float EMPTY_LOW = 1.0e30f;

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	uint2 dim = padDim.xy;
	if (id.x >= dim.x || id.y >= dim.y)
		return;

	int2 axis = padParams.xy;
	int radius = padParams.z;
	int2 maxCoord = int2(dim) - 1;

	float hi = EMPTY_HIGH;
	float lo = EMPTY_LOW;
	// Read every texel in the horizontal or vertical padding span, clamping at map edges.
	[loop] for (int offset = -radius; offset <= radius; offset++) {
		int2 sampleCoord = clamp(int2(id.xy) + axis * offset, int2(0, 0), maxCoord);
		hi = max(hi, InHigh[sampleCoord]);
		lo = min(lo, InLow[sampleCoord]);
	}

	OutHigh[id.xy] = hi;
	OutLow[id.xy] = lo;
}
