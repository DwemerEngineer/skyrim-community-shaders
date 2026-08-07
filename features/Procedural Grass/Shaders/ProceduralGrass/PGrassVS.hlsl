#define VSHADER
#define FRAMEBUFFER

#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"

#define PSHADER
#include "Common/SharedData.hlsli"
#undef PSHADER

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

StructuredBuffer<Blade> blades : register(t0);

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
#if !defined(DEPTH)
	float4 WorldPosition : TEXCOORD0;
	float4 PreviousWorldPosition : TEXCOORD1;
	float2 TexCoord : TEXCOORD2;
	float2 AOSubsurfaceThickness : TEXCOORD3;
	nointerpolation float4 TipMidpoint : TEXCOORD4;
	nointerpolation float3 FacingAndDoubleBladeBlade1 : TEXCOORD5;
	nointerpolation float2 MetalnessSpecular : TEXCOORD6;
#endif
#ifdef VR
	float ClipDistance : SV_ClipDistance0;
	float CullDistance : SV_CullDistance0;
#endif  // VR
};

float bias(float x, float b) {
	// from “GPU Gems” 
	return x / ((1/b - 2)*(1 - x) + 1);
}

VS_OUTPUT main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VS_OUTPUT o;
	Blade b = blades[instanceID];

#if defined(HIGH_VERTEX)
	static const float LEVELS = 7.0f;
	static const float DOUBLE_LEVELS = 4.0f;
	static const float MID_LEVEL = 3.0f;
	bool isBlade1 = vertexID >> 3;
#else  // LOW_VERTEX
	static const float LEVELS = 3.0f;
	static const float DOUBLE_LEVELS = 2.0f;
	static const float MID_LEVEL = 1.0f;
	bool isBlade1 = vertexID >> 2;
#endif
	static const float INV_LEVELS = 1.0f / LEVELS;
	static const float INV_DOUBLE_LEVELS = 1.0f / DOUBLE_LEVELS;

	uint seed = b.hashClumpAndGrassType;

	// float2 randFacing = float2((b.positionZFacing >> 8 & 0xFF) * (1.0f / 255.0f) * 2.0f - 1.0f, (b.positionZFacing & 0xFF) * (1.0f / 255.0f) * 2.0f - 1.0f);
	// float randHeight = f16tof32(b.widthHeight);
	// float randWidth = f16tof32(b.widthHeight >> 16);

	

	GrassType g = grassType[b.hashClumpAndGrassType & 0xFF];

	float2 randFacing = float2(f16tof32(b.facing >> 16), f16tof32(b.facing));
	float randHeight = g.height * (b.posZWidthHeight & 0xFF) * (1.0f / 255.0f);
	float randWidth = g.width * 2.5f * (b.posZWidthHeight >> 8 & 0xFF) * (1.0f / 255.0f);

	bool doubleBlade = randHeight <= 45.0f;
	bool doubleBladeAndIsBlade0 = doubleBlade && !isBlade1;

	float level = abs((vertexID >> 1) - doubleBlade * MID_LEVEL);
	float t = level * (doubleBlade ? INV_DOUBLE_LEVELS : INV_LEVELS);

	float2 facing = randFacing;
	float2 rotFacing;
	static const float cos30 = 0.8660254f;
	static const float sin30 = 0.5f;
	rotFacing.x = randFacing.x * cos30 - randFacing.y * sin30;
	rotFacing.y = randFacing.x * sin30 + randFacing.y * cos30;
	facing = doubleBladeAndIsBlade0 ? rotFacing : facing;

	float3 r3 = Random::f3(seed);

	// float2 base = float2(0, 0);
	// float2 p1 = ;
	// float2 p2 = ;
	// float2 tip = ;
	//
	// float t2 = t * t;
	// float t3 = t2 * t;
	//
	// float bT = 3 * (1 - t) * (1 - t) * t * p1 + 3 * (1 - t) * t * t * p2 + t * t * t * tip;
	
	float randTilt = g.tipWeight * (r3.y + 0.50f);// + (tipSway);
	float randBend = g.stiffness * (r3.z + 0.50f);// + (midSway);
	float xOffset, zOffset;
	sincos(randTilt, xOffset, zOffset);
	
	float2 tip = float2(xOffset, zOffset) * randHeight;
	float2 midPoint = tip * g.mid + float2(-tip.y, tip.x) * randBend;

	float2 pos2d = 2 * (1 - t) * t * midPoint + t * t * tip;
	float3 pos = float3(facing * pos2d.x, pos2d.y);
	
	float3 bitangent = float3(-facing.y, facing.x, 0.0f);
	uint side = vertexID & 1;
	float sideSign = (side * 2.0f - 1.0f) * (1.0f - step(doubleBlade ? DOUBLE_LEVELS : LEVELS, level));
	// float taper = lerp(blade.width, 0.0f, t * t * t * t * t);
	float taper = randWidth;
	pos += bitangent * taper * sideSign;

	float3 originalViewPos = float3(f16tof32(b.posXY >> 16), f16tof32(b.posXY), f16tof32(b.posZWidthHeight >> 16));
	float4 worldPos = float4(originalViewPos + FrameBuffer::CameraPosAdjust[0].xyz + pos, 1.0f);
	float4 viewPos = worldPos - FrameBuffer::CameraPosAdjust[0];
	float4 previousViewPos = worldPos - FrameBuffer::CameraPreviousPosAdjust[0];
	float4 clipPos = mul(FrameBuffer::CameraViewProj[0], viewPos);

	// Face on experiment
	// float dotP = dot(FrameBuffer::CameraView[0][2].xyz, tangent);
	// float faceOnFactor = 1.0f - abs(dotP);
	// pos += normal * faceOnFactor * taper  * sideSign * 2.5f;
	// float faceOnFactor = abs(dot(normalize(viewPos.xyz), normal));
	// float orthogonality = 1.0f - faceOnFactor;

#if defined(DEPTH)
	o.Position = clipPos;
#else
	o.Position = clipPos;
	o.TexCoord = float2((sideSign + 1.0f) * 0.5f, t);
	o.WorldPosition = viewPos;
	o.PreviousWorldPosition = previousViewPos;
	o.TipMidpoint = float4(tip, midPoint);
	// o.TipMidpoint = 0;
	o.FacingAndDoubleBladeBlade1 = float3(facing, doubleBladeAndIsBlade0);
	o.AOSubsurfaceThickness = float2(lerp(g.minAO, 1.0f, t), lerp(g.minMaxSubsurfaceOpacity.x, g.minMaxSubsurfaceOpacity.y, t));
	o.MetalnessSpecular = float2(0, g.specular);
#endif

	return o;
}
