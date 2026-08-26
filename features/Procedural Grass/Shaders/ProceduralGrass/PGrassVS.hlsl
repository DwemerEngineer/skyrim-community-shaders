#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"

#define PSHADER
#include "Common/SharedData.hlsli"
#undef PSHADER


#if defined(GRASS_COLLISION)
#include "GrassCollision/GrassCollision.hlsli"
#endif

#include "ProceduralGrass/PGrassCommon.hlsli"

#define VSHADER
#define FRAMEBUFFER

StructuredBuffer<Blade> Blades : register(t0);

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
#if defined(DEPTH)
	float BladeHeight : TEXCOORD0;  // Height above the root; depth and colour use the same base dissolve.
#elif defined(FAR_LOD)
	float4 CameraPositionSide : TEXCOORD0;  // xyz: camera-relative position; w: across-blade coordinate
	float4 BladeTColor : TEXCOORD1;         // x: blade parameter; yzw: base-to-tip colour
	nointerpolation uint3 PackedBladeParams : TEXCOORD2;  // facing/tilt, seed/type/width, root Z/width/height
#else
	float4 CameraRelativePosition : TEXCOORD0;  // xyz: camera-relative position; w: across-blade coordinate
	float4 PreviousCameraRelativePosition : TEXCOORD1;  // xyz: previous camera-relative position; w: Bezier t
	float4 AOThicknessRoughness : TEXCOORD3;  // xyz: AO, thickness, roughness; w: root-relative height
	nointerpolation float4 BezierTipAndMid : TEXCOORD4;  // xy: tip; zw: midpoint in facing/up space
	nointerpolation float4 BladeParams : TEXCOORD5;  // xy: facing; z: type; w: two f16 randoms
	float3 BaseToTipColor : TEXCOORD7;  // Per-blade tint multiplied by the base-to-tip colour.
#	if defined(SKYLIGHTING) && defined(HIGH_LOD)
	nointerpolation float4 SkylightingVertexSH : TEXCOORD9;  // Per-blade SH from the generator.
#	endif
#endif
};

VS_OUTPUT main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VS_OUTPUT o;
	Blade blade = Blades[instanceID];

#if defined(HIGH_VERTEX)
	static const float LEVELS = 7.0f;
	static const float DOUBLE_LEVELS = 4.0f;
	static const float MID_LEVEL = 3.0f;
	bool isBlade1 = vertexID >> 3;
#elif defined(FAR_VERTEX)
	// Far uses one tapered triangle.
	static const float LEVELS = 1.0f;
	static const float DOUBLE_LEVELS = 1.0f;
	static const float MID_LEVEL = 1.0f;
	bool isBlade1 = false;
#elif defined(MID_VERTEX)
	// Mid uses base, midpoint, and tip; double blades use one triangle per half.
	static const float LEVELS = 2.0f;
	static const float DOUBLE_LEVELS = 1.0f;
	static const float MID_LEVEL = 1.0f;
	bool isBlade1 = vertexID >> 2;
#else  // LOW_VERTEX
	static const float LEVELS = 3.0f;
	static const float DOUBLE_LEVELS = 2.0f;
	static const float MID_LEVEL = 1.0f;
	bool isBlade1 = vertexID >> 2;
#endif

	static const float INV_LEVELS = 1.0f / LEVELS;
	static const float INV_DOUBLE_LEVELS = 1.0f / DOUBLE_LEVELS;
	static const float INV_MID_LEVEL = 1.0f / MID_LEVEL;

#if defined(FAR_LOD)
	uint grassTypeIndex = blade.seedAndType & 0xFF;
	uint bladeSeed = (blade.seedAndType >> 8) & 0xFFFF;
#elif defined(MID_LOD) || defined(LOW_LOD)
	uint grassTypeIndex = blade.hashClumpAndGrassType & 0xFF;
	uint hashClumpAndGrassType = blade.hashClumpAndGrassType;
	uint bladeSeed = hashClumpAndGrassType >> 20;
#else
	uint grassTypeIndex = blade.hashClumpAndGrassType & 0xFF;
	uint hashClumpAndGrassType = blade.hashClumpAndGrassType;
	uint bladeSeed = hashClumpAndGrassType >> 12;
#endif

	GrassType bladeType = grassType[grassTypeIndex];

	float2 tiltDir;
	
#if defined(FAR_LOD)
	float4 packedDirections = float4(blade.facingTilt & 0xFF, (blade.facingTilt >> 8) & 0xFF, (blade.facingTilt >> 16) & 0xFF, blade.facingTilt >> 24);
	packedDirections = packedDirections * (2.0f / 255.0f) - 1.0f;
	float2 randFacing = packedDirections.xy;
	float2 previousFacing = randFacing;

	float windDisplacement = 0.0f;
	float previousWindDisplacement = 0.0f;

	float clumpDensity = 0.0f;
	tiltDir = packedDirections.zw;
#else
	int2 packedFacing = int2(blade.facingAndWind << 24, blade.facingAndWind << 16) >> 24;
	float2 randFacing = float2(packedFacing) * (1.0f / 127.0f);
	float2 previousFacing = randFacing;

	float windDisplacement = f16tof32(blade.facingAndWind >> 16);
	float previousWindDisplacement = f16tof32(blade.previousWind);

	float clumpDensity = saturate(f16tof32(blade.clumpDensity));
	float randBend = f16tof32(blade.clumpDensity >> 16);
	tiltDir = float2(f16tof32(blade.tipDir >> 16), f16tof32(blade.tipDir));
#endif

	float randHeight = bladeType.height * (blade.posZWidthHeight & 0xFF) * (1.0f / 255.0f);
	// Expand the packed width range so blades can widen as well as thin.
	float widthByte = (blade.posZWidthHeight >> 8 & 0xFF) * (1.0f / 255.0f);
	float randWidth = bladeType.width * 2.5f * lerp(0.45f, 1.3f, widthByte);

#if defined(FAR_LOD)
	float farWidthT = float(blade.seedAndType >> 24) * (1.0f / 255.0f);
	randWidth *= lerp(2.0f, 32.0f, farWidthT);
#elif defined(LOW_LOD)
	randWidth *= 2.0f;
#endif

#if defined(FAR_VERTEX)
	bool doubleBlade = false;  // Far renders a single tapered blade.
#else
	bool doubleBlade = randHeight <= 45.0f;
#endif

	bool doubleBladeAndIsBlade0 = doubleBlade && !isBlade1;

	// Double blades run tip-to-base-to-tip, with each half reaching t = 1.
	float rung = vertexID >> 1;
	bool upperHalf = rung >= MID_LEVEL;
	float bladeLevels = doubleBlade ? (upperHalf ? DOUBLE_LEVELS : MID_LEVEL) : LEVELS;
	float invBladeLevels = doubleBlade ? (upperHalf ? INV_DOUBLE_LEVELS : INV_MID_LEVEL) : INV_LEVELS;

	float level = abs(rung - doubleBlade * MID_LEVEL);
	float t = level * invBladeLevels;

	static const float cos30 = 0.8660254f;
	static const float sin30 = 0.5f;
	float2 facing = doubleBladeAndIsBlade0 ? float2(randFacing.x * cos30 - randFacing.y * sin30, randFacing.x * sin30 + randFacing.y * cos30) : randFacing;
	float2 prevFacing = doubleBladeAndIsBlade0 ? float2(previousFacing.x * cos30 - previousFacing.y * sin30, previousFacing.x * sin30 + previousFacing.y * cos30) : previousFacing;

	float2 tip = tiltDir * randHeight;
#if !defined(FAR_LOD)
	float2 midPoint = tip * bladeType.mid + float2(-tip.y, tip.x) * randBend;  // Bezier control point used by the PS tangent
#endif

	uint side = vertexID & 1;
	float sideSign = (side * 2.0f - 1.0f) * (1.0f - step(bladeLevels, level));

#if defined(FAR_VERTEX)
	// Far has only base and tip vertices, so its profile is a straight tapered segment.
	float2 pos2d = t * tip;
	float taper = randWidth * (1.0f - t);
#else
	float2 pos2d = 2 * (1 - t) * t * midPoint + t * t * tip;
	// Interpolate t squared to t to the fourth power across the fixed rungs instead of evaluating pow.
	float t2 = t * t;
	float taperCurve = lerp(t2, t2 * t2, widthByte);
	float taper = randWidth * (1.0f - taperCurve);
#endif

	float3 originalViewPos = float3(f16tof32(blade.posXY >> 16), f16tof32(blade.posXY), f16tof32(blade.posZWidthHeight >> 16));

	// Build the blade in camera-relative space, then apply the animated tip displacement.
	float3 pos = float3(facing * pos2d.x, pos2d.y) + float3(-facing.y, facing.x, 0.0f) * taper * sideSign;
	float windWeight = t * t;

#if defined(FAR_VERTEX)
	float3 previousPos = pos;
#else
	float3 previousPos = float3(prevFacing * pos2d.x, pos2d.y) + float3(-prevFacing.y, prevFacing.x, 0.0f) * taper * sideSign;
#endif

#if defined(HIGH_LOD) || defined(MID_LOD)
	pos.xy += windDir * windDisplacement * windWeight;
	// Keep motion vectors consistent with the current wind displacement.
	previousPos.xy += windDir * previousWindDisplacement * windWeight;
#endif

	float4 viewPos = float4(originalViewPos + pos, 1.0f);  // The stored root offset is already camera-relative.
#if !defined(FAR_VERTEX)
	float4 previousViewPos = float4(originalViewPos + previousPos + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz), 1.0f);
#endif

#if defined(GRASS_COLLISION) && !defined(FAR_LOD)
	float3 collisionDisplacement, previousCollisionDisplacement;
	// Smoothstep bends from a fixed root to full tip displacement.
	float collisionWeight = t * t * (3.0f - 2.0f * t);
	GrassCollision::GetDisplacedPosition(viewPos.xyz, originalViewPos, collisionWeight, 2048.0, true,
		collisionDisplacement, previousCollisionDisplacement);
	viewPos.xyz += collisionDisplacement;
	previousViewPos.xyz += previousCollisionDisplacement;
#endif

	float4 clipPos = mul(FrameBuffer::CameraViewProj, viewPos);

// Widen edge-on blades to keep their silhouette visible.
#if defined(MID_VERTEX) || defined(LOW_VERTEX)
	// Mid/Low pack one 4-bit factor for each double-blade facing.
	uint packedViewThicken = (hashClumpAndGrassType >> 12) & 0xFFu;
	uint viewThickenNibble = doubleBladeAndIsBlade0 ? packedViewThicken >> 4 : packedViewThicken & 0xFu;
	float viewThicken = float(viewThickenNibble) * (1.0f / 15.0f);
	clipPos.x += FrameBuffer::CameraProj._m00 * viewThicken * sideSign * taper * miscParams.z;
#elif defined(HIGH_VERTEX)
	float viewDotNormal = saturate(dot(facing, normalize(-originalViewPos.xy)));
	float viewDotNormal2 = viewDotNormal * viewDotNormal;
	float viewThicken = (1.0f - viewDotNormal2 * viewDotNormal2) * smoothstep(0.0f, 0.2f, viewDotNormal);
	clipPos.x += FrameBuffer::CameraProj._m00 * viewThicken * sideSign * taper * miscParams.z;
#endif

	// Pack only the data required by the selected pixel-shader tier.
	o.Position = clipPos;
#if defined(DEPTH)
	o.BladeHeight = pos2d.y;
#else
#	if defined(FAR_LOD)
	o.CameraPositionSide = float4(viewPos.xyz, (sideSign + 1.0f) * 0.5f);
	o.PackedBladeParams = uint3(blade.facingTilt, blade.seedAndType, blade.posZWidthHeight);
#	else
	// Pack side, Bezier t, and root-relative height into unused w components.
	o.CameraRelativePosition = float4(viewPos.xyz, (sideSign + 1.0f) * 0.5f);
	o.PreviousCameraRelativePosition = float4(previousViewPos.xyz, t);
	o.BezierTipAndMid = float4(tip, midPoint);
	// Mid evaluates three t values, so this polynomial preserves the authored curve at each rung.
#		if defined(MID_VERTEX)
	float roughnessT2 = t * t;
	float roughness = mad(mad(bladeType.midRoughnessPolynomial.x, t, bladeType.midRoughnessPolynomial.y), roughnessT2, bladeType.midRoughnessPolynomial.z);
#		else
	// Blend from base roughness through the configured midpoint to the tip.
	float roughness = lerp(bladeType.baseMinTipRoughnessStart.x, bladeType.baseMinTipRoughnessStart.y, smoothstep(0.0f, bladeType.baseMinTipRoughnessStart.w, t));
	roughness = lerp(roughness, bladeType.baseMinTipRoughnessStart.z, smoothstep(bladeType.baseMinTipRoughnessStart.x, 1.0f, t));
#		endif

	float bladeAO = lerp(bladeType.minAO, 1.0f, t);
	float clumpAO = lerp(1.0f, bladeType.minAO, clumpDensity * bladeType.clumpAOStrength);
	o.AOThicknessRoughness = float4(bladeAO * clumpAO, lerp(bladeType.minMaxSubsurfaceOpacity.x, bladeType.minMaxSubsurfaceOpacity.y, t), roughness, pos2d.y);
#	endif

	// Decode the generator's packed per-blade random values.
#	if defined(FAR_LOD)
	float bladeRand = (float(bladeSeed & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float bladeRand2 = (float(bladeSeed >> 8) + 0.5f) * (1.0f / 256.0f);
#	elif defined(MID_LOD) || defined(LOW_LOD)
	float bladeRand = (float(bladeSeed & 0x3Fu) + 0.5f) * (1.0f / 64.0f);
	float bladeRand2 = (float(bladeSeed >> 6) + 0.5f) * (1.0f / 64.0f);
#	else
	float bladeRand = float(bladeSeed & 0x3FFu) * (1.0f / 1024.0f);
	float bladeRand2 = float((bladeSeed >> 10) & 0x3FFu) * (1.0f / 1024.0f);
#	endif

	float3 hueTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, bladeRand);
	float bladeValue = 1.0f + (bladeRand2 * 2.0f - 1.0f) * bladeType.grassColorVar.y;
	float3 perBladeColor = lerp(1.0f, hueTint, bladeType.grassColorVar.x) * bladeValue;

#	if !defined(FAR_LOD)
	// Remap the 4-bit clump index with two affine permutations instead of per-vertex hashing.
	uint clumpIndex = (hashClumpAndGrassType >> 8) & 0xFu;
	float clumpRand = (float((clumpIndex * 13u + 5u) & 0xFu) + 0.5f) * (1.0f / 16.0f);
	float clumpRand2 = (float((clumpIndex * 7u + 3u) & 0xFu) + 0.5f) * (1.0f / 16.0f);
	float3 clumpTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, clumpRand);
	float clumpValue = 1.0f + (clumpRand2 * 2.0f - 1.0f) * bladeType.grassColorVar.y * 0.75f;
	perBladeColor *= lerp(1.0f, clumpTint * clumpValue, bladeType.clumpColorStrength);
#	endif

	// Fold smooth base shading and tip drying into the interpolated colour.
#	if defined(FAR_LOD)
	// Far interpolates exact endpoint values, so both factors reduce to t.
	float3 tipDryMul = lerp(1.0f, bladeType.grassColorTipDry.rgb, t * bladeType.grassColorVar.z);
	float baseShade = lerp(1.0f - grassLightParams.w, 1.0f, t);
#	else
	float3 tipDryMul = lerp(1.0f, bladeType.grassColorTipDry.rgb, smoothstep(0.5f, 1.0f, t) * bladeType.grassColorVar.z);
	float baseShade = lerp(1.0f - grassLightParams.w, 1.0f, smoothstep(0.0f, 0.5f, t));
#	endif

	// Pass facing, type, and two f16 randoms in one flat interpolator.
	float3 baseToTipColor = lerp(bladeType.baseColor.rgb, bladeType.tipColor.rgb, t) * perBladeColor * tipDryMul * baseShade;
#	if defined(FAR_LOD)
	o.BladeTColor = float4(t, baseToTipColor);
#	else
	uint packedDetailRandom = blade.previousWind >> 16;
	float detailRand = (float(packedDetailRandom & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float detailRand2 = (float(packedDetailRandom >> 8) + 0.5f) * (1.0f / 256.0f);
	o.BladeParams = float4(facing, float(grassTypeIndex),
		asfloat((f32tof16(detailRand) << 16) | f32tof16(detailRand2)));
	o.BaseToTipColor = baseToTipColor;
#	endif

#	if defined(SKYLIGHTING) && defined(HIGH_LOD)
	// Generator packs four f16 SH coefficients per High blade.
	o.SkylightingVertexSH = float4(f16tof32(blade.skylightingSH0 >> 16), f16tof32(blade.skylightingSH0),
		f16tof32(blade.skylightingSH1 >> 16), f16tof32(blade.skylightingSH1));
#	endif
#endif

	return o;
}
