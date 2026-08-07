#define CSHADER
#define FRAMEBUFFER

#if defined(__RESHARPER__)
#	define THREADGROUP_SIZE 8
#	define DENSITY 192
#endif

#include "Common/FastMath.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

#define PSHADER
#include "Common/SharedData.hlsli"
#undef PSHADER

static const int TG_DIM_X = THREADGROUP_SIZE;
static const int TG_DIM_Y = 1;
static const uint BLADES_PER_ROW = DENSITY;

#if defined(HIGH_LOD)
static const uint QUADRANT_DATA_SIZE = 9;
#elif defined(MID_LOD)
static const uint QUADRANT_DATA_SIZE = 16;
#else  // LOW_LOD
static const uint QUADRANT_DATA_SIZE = 75;
#endif

static const int TG_SIZE = TG_DIM_X * TG_DIM_Y;
static const uint PATCHES_PER_QUADRANT = BLADES_PER_ROW * BLADES_PER_ROW / 4;
static const float BLADES_PER_ROW_INV = 1.0f / BLADES_PER_ROW;
static const float BLADE_TO_WORLD = 2048.0f / BLADES_PER_ROW;
static const float WORLD_TO_HEIGHT_TEXEL = (16.0f / BLADES_PER_ROW) / BLADE_TO_WORLD;
static const float HEIGHT_TEXEL_TO_UV = 4096.0f;
static const float INV_HEIGHT_TEXEL_TO_UV = 1.0f / HEIGHT_TEXEL_TO_UV;

static const uint H1 = 0x9E3779B1u;
static const uint H2 = 0x85EBCA77u;
static const uint H3 = 0x45D9F3Bu;
static const uint H4 = 0x27D4EB2Fu;
static const uint H5 = 0x165667B1u;

#include "Common/FrameBuffer.hlsli"

struct QuadrantData
{
	float2 quadWorldPos;
	uint quadX;
	uint quadY;
};

cbuffer QuadrantData : register(b7)
{
	QuadrantData data[QUADRANT_DATA_SIZE];
}

cbuffer GrassGlobals : register(b8)
{
	float4 color;
	float2 dynamicResolutionInverted;
	float voronoiGridSize;
	float inverseVoronoiGridSize;
	float cameraViewRow0Sum;
	float cameraViewRow1Sum;
	float windSpeed;
	float windTimer;
	float2 windDir;
	float2 padgg0;
}

struct GrassType
{
	float height;
	float width;
	float stiffness;
	float tipWeight;
	float mid;
	float minAO;
	float rotationalStiffness;
	float specular;
	float2 minMaxSubsurfaceOpacity;
	float clumpDistanceFactor;
	float clumpHeightFactor;
	float clumpFacingFactor;
	float spatialFreq;
	float phaseOffset;
	float phaseLag;
};

cbuffer GrassTypes : register(b9)
{
	GrassType grassType[2];
}

struct Blade
{
	uint posXY;
	uint posZWidthHeight;
	uint facing;
	uint hashClumpAndGrassType;
};

AppendStructuredBuffer<Blade> gBladeAppendBuffer : register(u0);

Texture2D<float> gHeightTex : register(t0);
Texture2D<uint> gGrassTex : register(t1);

SamplerState gLinearSampler : register(s0);

void computeClump(out uint clumpRand, out float clumpDist, out float2 clumpDir, float2 pos, float gridSize, float inverseGridSize)
{
	float2 gridPos = pos * inverseGridSize;
	int2 gridCell = int2(gridPos);

	clumpDist = 1e30;
	clumpRand = 0;

	for (int y = gridCell.y - 1; y <= gridCell.y + 1; y++) {
		for (int x = gridCell.x - 1; x <= gridCell.x + 1; x++) {
			uint seed = x * H1 ^ y * H2;
			// seed = (seed ^ seed >> 16) * H3;
			// uint seed = x << 16 | y;

			uint rand;
			float2 jitter = Random::f2(seed, rand);
			float2 jitteredPos = float2(x, y) + jitter;

			float2 diff = jitteredPos - gridPos;
			float distDq = dot(diff, diff);

			if (distDq < clumpDist) {
				clumpDist = distDq;
				clumpDir = diff;
				clumpRand = rand;
			}
		}
	}

	float invLen = rsqrt(clumpDist);
	clumpDist = clumpDist * invLen * gridSize;
	clumpDir *= invLen;
}

void computeGrassType(out uint type, out float pctNoGrass, float2 pos, float rand)
{
	float2 grassTexel = (pos + float2(61.0f, 60.0f) * 4096.0f) / 524288.0f * 16384.0f;

	int2 baseTexel = int2(grassTexel);
	float2 frac = grassTexel - float2(baseTexel);

	uint4 ids = gGrassTex.GatherRed(gLinearSampler, (baseTexel + 0.5f) / 16384.0f);

	float4 weights;
	weights.x = (1 - frac.x) * (1 - frac.y);
	weights.y = frac.x * (1 - frac.y);
	weights.z = (1 - frac.x) * frac.y;
	weights.w = frac.x * frac.y;

	pctNoGrass = 0;
	[unroll] for (int i = 0; i < 4; ++i)
	{
		if (ids[i] == 0)
			pctNoGrass += weights[i];
	}

	float weightSum = 0;
	type = ids[3];

	[unroll] for (int i = 0; i < 4; i++)
	{
		weightSum += weights[i];
		if (rand < weightSum) {
			type = ids[i];
			break;
		}
	}
}

[numthreads(TG_DIM_X, TG_DIM_Y, 1)] void main(uint3 dispatch : SV_DispatchThreadID) {
	uint patch = dispatch.x;
	uint bladeIndex = dispatch.y;
	uint quadrant = dispatch.z;

	if (patch >= PATCHES_PER_QUADRANT)
		return;

	QuadrantData q = data[quadrant];

	// BASE BLADE XY
	uint2 patchPos = uint2(patch % (BLADES_PER_ROW / 2), patch / (BLADES_PER_ROW / 2));

	uint patchHash = patchPos.x * H1 ^ patchPos.y * H2;
	uint bladeIndexRandomiser = (patchHash >> 16) & 3;
	uint randomBladeIndex = bladeIndex ^ bladeIndexRandomiser;

	uint2 pos = patchPos * 2u + uint2(randomBladeIndex >> 1u, randomBladeIndex & 1u);

	// BLADE SEED
	uint seed = pos.x * H1 ^ pos.y * H2 ^ q.quadX * H3 ^ q.quadY * H4;

	// JITTERED BLADE XY
	float4 rand4 = Random::f4(seed);
	float2 jitter = rand4.xy;
#if defined(LOW_LOD)
	jitter *= 0.5f;
#endif
	float2 bladePos2D = float2(pos) + jitter;
	float2 bladeQuadPos2D = bladePos2D * BLADE_TO_WORLD;
	float2 bladeWorldPos2D = bladeQuadPos2D + q.quadWorldPos;

	// CLUMP
	uint clumpRand;
	float clumpDist;
	float2 clumpDir;
	computeClump(clumpRand, clumpDist, clumpDir, bladeWorldPos2D, voronoiGridSize, inverseVoronoiGridSize);

	// GRASS TYPE
	uint type;
	float pctNoGrass;
	// computeGrassType(type, pctGrass, bladeWorldPos2D + clumpDir * clumpDist);
	computeGrassType(type, pctNoGrass, bladeWorldPos2D, rand4.z);
	if (type == 0)
		return;
	type = 1;

	GrassType g = grassType[type];

	// BLADE XYZ INC. CLUMP & HEIGHT
	bladeWorldPos2D += clumpDir * clumpDist * saturate(rand4.w - 0.5f) * g.clumpDistanceFactor;
	float2 heightUV = ((bladeWorldPos2D + float2(61.0f, 60.0f) * 4096.0f) * WORLD_TO_HEIGHT_TEXEL + 0.5f) * INV_HEIGHT_TEXEL_TO_UV;
	float bladeWorldZ = gHeightTex.SampleLevel(gLinearSampler, heightUV, 0);
	float3 worldPos = float3(bladeWorldPos2D, bladeWorldZ);
	float3 viewPos = worldPos - FrameBuffer::CameraPosAdjust[0].xyz;

	// HEIGHT
	rand4 = Random::f4(seed);
	float randClumpHeight = asfloat((clumpRand << 11 | clumpRand >> 21) & 0x007FFFFFu | 0x3F800000u) - 1.0f;
	float unscaledHeight = (0.66f + rand4.x * (1.0f - 0.66f)) - randClumpHeight * g.clumpHeightFactor;
	unscaledHeight *= (1.0f - pctNoGrass);
	float randHeight = g.height * unscaledHeight;

	// FRUSTUM CULL
	float4 clip = mul(FrameBuffer::CameraViewProj[0], float4(viewPos, 1));
	float extraHeight = randHeight * 2.0f;
	float padX = cameraViewRow0Sum * extraHeight;
	float padY = cameraViewRow1Sum * extraHeight;
	bool outsideFrustum = clip.x < -(clip.w + padX) || clip.x > clip.w + padX || clip.y < -(clip.w + padY) || clip.y > clip.w + padY;  // || clip.z < 0 || clip.z > clip.w;
	if (outsideFrustum)
		return;

#if !defined(LOW_LOD)
	// DISTANCE CULL
	float dist = length(viewPos.xy);
	static const float4 cullDists = float4(8192.0f, 4096.0f, 2048.0f, 1024.0f);
	// float cullDist = cullDists[bladeIndex];
	float cullDist = 8192u >> bladeIndex;
	if (dist >= cullDist)
		return;
#endif

	// WIDTH
	// float randWidth = g.width * (rand4.y * 0.5f + 1.0f);
#if defined(LOW_LOD)
	// randWidth *= 2.5f;
	float unscaledWidth = 1.0f;
#else
	float gain = saturate((dist - cullDists[3]) * rcp((cullDists[1] - cullDists[3])));
	float fadeOut = saturate((cullDist - dist) * (1.0f / 768.0f));
	// randWidth *= lerp(1.0f, 2.5f, gain) * fadeOut;  // TODO could treat this as a 0-1 value and pack in a byte if more space needed, same with height
	float unscaledWidth = lerp(0.4f, 1.0f, gain) * fadeOut;
#endif
	float scaledWidth = unscaledWidth * g.width * 2.5f;

	// SLOPE CULLING
	// TODO

	// FACING
	float randAngle = rand4.z * Math::TAU;
	// float2 randFacing = rand4.yz * 2.0f - 1.0f;

	// CLUMP FACING
	float clumpAngle = atan2(-clumpDir.y, -clumpDir.x);
	if (clumpAngle < 0)
		clumpAngle += Math::TAU;
	float delta = clumpAngle - randAngle;
	
	// delta = fmod(delta + Math::PI, Math::TAU) - Math::PI;
	if (delta < 0)      delta += Math::TAU;
	else if (delta >= Math::TAU) delta -= Math::TAU;
	
	float clumpedAngle = randAngle + delta * g.clumpFacingFactor;
	
	// clumpedAngle = fmod(clumpedAngle + Math::TAU, Math::TAU);
	if (clumpedAngle < 0)      clumpedAngle += Math::TAU;
	else if (clumpedAngle >= Math::TAU) clumpedAngle -= Math::TAU;

	// SLOPE FACING
	// TODO

	// WIND FACING
	// Step 1 - calculate the wind angle TODO pass this from CPU
	float windAngle = atan2(windDir.y, windDir.x); // 298
	// float windAngle = FastMath::ATan(windDir.y * rcp(windDir.x)); // 288
	// float windAngle = FastMath::atanFast4(windDir.y * rcp(windDir.x)); // 282
	if (windAngle < 0.0f)
		windAngle += Math::TAU;

	// Step 2 - calculate the angular difference
	float diff = windAngle - clumpedAngle;
	if (diff > Math::PI)
		diff -= Math::TAU;
	else if (diff < -Math::PI)
		diff += Math::TAU;

	// Step 3 - calculate the dot product
	float2 clumpedFacing = float2(cos(clumpedAngle), sin(clumpedAngle));
	float dotProd = dot(windDir, clumpedFacing);

	// Step 4 - calculate the alignment factor, 0 when facing away, 1 when aligned with the wind
	float alignment = dotProd * 0.5f + 0.5f;

	// Step 5 - use the factor as the input to the rotation amount calculation
	float rawRotation = alignment * 0.5f;

	// Step 6 - adjust the raw rotation factor to a minimum
	float rotationFactor = lerp(0.2f, 1.0f, rawRotation);

	// Step 7 - multiply the rotation factor by the wind force to get the total rotation
	float totalRotation = rotationFactor * windSpeed * windSpeed * windSpeed * 0.5f * scaledWidth * randHeight;

	// Step 8 - reduce the totalRotation the higher it is, the further it rotates the harder it should be to rotate further
	float reducedRotation = totalRotation * rcp(g.rotationalStiffness * totalRotation + 1.0f);

	// Step 9 - clamp the total rotation to the diff to prevent bypassing the wind direction
	float clampedRotation = min(reducedRotation, abs(diff)) * sign(diff);

	// Step 10 - calculate the windAdjustedAngle
	float windAdjustedAngle = clumpedAngle + clampedRotation;
	
	// Step 10 - rotational flutter, could maybe add perlin here as well
	// Get angular difference
	float angularDiff = abs(windAdjustedAngle - windAngle);
	if (angularDiff > Math::PI)
		angularDiff = Math::TAU - angularDiff;
	float n     = angularDiff * 1.0f / Math::PI;
	float ninetyFactor = 4.0f * n * (1.0f - n);
	float remapped = lerp(0.5f, 1.0f, ninetyFactor);
	float phase = rand4.w * Math::TAU + SharedData::Timer * Math::TAU * 1.5f * windSpeed;
	// phase = fmod(phase, Math::TAU);
	float rotationalFlutter = sin(phase) * windSpeed * 0.3f * (Random::f1(seed) * 0.5f - 0.25f + 1.0f) * remapped;

	
	float flutterAdjustedAngle = windAdjustedAngle + rotationalFlutter;
	

	float sinOut, cosOut;
	sincos(flutterAdjustedAngle, sinOut, cosOut);
	float2 randFacing = float2(cosOut, sinOut);
	// randFacing = saturate(randFacing * 0.5f + 0.5f);

	// OUTPUT
	Blade b;
	b.posXY = f32tof16(viewPos.x) << 16 | f32tof16(viewPos.y);
	b.posZWidthHeight = f32tof16(viewPos.z) << 16 | (uint)(unscaledWidth * 255.0f) << 8 | (uint)(unscaledHeight * 255.0f);
	b.facing = f32tof16(randFacing.x) << 16 | f32tof16(randFacing.y); // could pack tighter if required, only really need maybe 10 bits each
	b.hashClumpAndGrassType = (seed << 12) | ((clumpRand & 15) << 8) | type;

	gBladeAppendBuffer.Append(b);
}