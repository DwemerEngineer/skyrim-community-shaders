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
	precise float4 Position : SV_POSITION;
#if defined(DEPTH)
	float BladeHeight : TEXCOORD0;  // Height above the root for depth-fade clipping.
#elif defined(FAR_LOD)
	float4 CameraPositionSide : TEXCOORD0;  // xyz: camera-relative position; w: across-blade coordinate
	float4 BladeTColor : TEXCOORD1;         // x: blade parameter; yzw: base-to-tip colour
	nointerpolation uint4 PackedBladeParams : TEXCOORD2;  // facing/tilt, seed/type, root Z/width/height, continuous Far ramp
#else
	float4 CameraRelativePosition : TEXCOORD0;  // xyz: camera-relative position; w: across-blade coordinate
	float4 PreviousCameraRelativePosition : TEXCOORD1;  // xyz: previous camera-relative position; w: Bezier t
#	if defined(HIGH_LOD) || defined(MID_LOD)
	nointerpolation float2 WindOffset : TEXCOORD2;  // Current tip offset used to reconstruct the wind-bent tangent.
#	endif
	float4 AOThicknessRoughness : TEXCOORD3;  // xyz: AO, thickness, roughness; w: root-relative height
	nointerpolation float4 BezierTipAndMid : TEXCOORD4;  // xy: tip; zw: midpoint in facing/up space
	nointerpolation float4 BladeParams : TEXCOORD5;  // xy: facing; z: type; w: two f16 randoms
	float4 BaseToTipColor : TEXCOORD7;  // xyz: blade colour; w: positive view depth
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
	// Mid uses base, midpoint, and tip. Double blades use one triangle per half.
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
	uint grassTypeIndex = blade.seedAndType & 0xFFu;
	uint bladeSeed = (blade.seedAndType >> 8) & 0xFFFFu;
	uint clumpSeed = bladeSeed;
#else
	uint hashClumpAndGrassType = blade.hashClumpAndGrassType;
	uint grassTypeIndex = hashClumpAndGrassType & 0xFFu;
#	if defined(MID_LOD) || defined(LOW_LOD)
	uint bladeSeed = hashClumpAndGrassType >> 20;
	uint clumpSeed = ((hashClumpAndGrassType >> 8) & 0xFu) | ((hashClumpAndGrassType >> 16) & 0xFFF0u);
#	else
	uint bladeSeed = hashClumpAndGrassType >> 12;
	uint clumpSeed = (hashClumpAndGrassType >> 8) & 0xFFFFu;
#	endif
#endif

	GrassType bladeType = grassType[grassTypeIndex];
	float3 rootViewPosition = float3(
		f16tof32(blade.posXY >> 16),
		f16tof32(blade.posXY),
		f16tof32(blade.posZWidthHeight >> 16));
	// Use the stable LOD origin for all distance-driven appearance changes.
	float2 rootLodOffset = rootViewPosition.xy + FrameBuffer::CameraPosAdjust.xy - grassLodOrigin;
	float rootDistance = ApproximateGrassDistance(rootLodOffset);

	float2 tiltDir;
	
#if defined(FAR_LOD)
	float4 packedDirections = float4(blade.facingTilt & 0xFF, (blade.facingTilt >> 8) & 0xFF, (blade.facingTilt >> 16) & 0xFF, blade.facingTilt >> 24);
	packedDirections = packedDirections * (2.0f / 255.0f) - 1.0f;
	float2 randFacing = packedDirections.xy;

	tiltDir = packedDirections.zw;
#else
	int2 packedFacing = int2(blade.facingAndWind << 24, blade.facingAndWind << 16) >> 24;
	float2 randFacing = float2(packedFacing) * (1.0f / 127.0f);

	float windDisplacement = f16tof32(blade.facingAndWind >> 16);
	float previousWindDisplacement = f16tof32(blade.previousWind);

	float clumpDensity = saturate(f16tof32(blade.clumpDensity));
	float randBend = f16tof32(blade.clumpDensity >> 16);
	tiltDir = float2(f16tof32(blade.tipDir >> 16), f16tof32(blade.tipDir));
#endif
	float randHeight = bladeType.height * (blade.posZWidthHeight & 0xFFu) * (1.0f / 255.0f);
	float widthScale = ((blade.posZWidthHeight >> 8) & 0xFFu) * (1.0f / 255.0f);
#if defined(HIGH_LOD) || defined(MID_LOD)
	// Evaluate distance widening per vertex to keep it continuous as the camera moves.
	float distanceWidth = lerp(0.4f, 1.0f, saturate((rootDistance - 1024.0f) * (1.0f / 3072.0f)));
	widthScale *= distanceWidth;
#endif
	float randWidth = bladeType.width * 2.5f * lerp(0.45f, 1.3f, widthScale);

#if defined(FAR_LOD)
	float farWidthT = saturate((rootDistance - farParams.x) * farParams.y);
	float performanceKeep = lerp(1.0f, farParams.w, farWidthT);
	float coverageCompensation = min(rsqrt(max(performanceKeep, 0.25f)), 1.6f);
	randWidth *= lerp(2.0f, 32.0f, farWidthT) * coverageCompensation;
#elif defined(MID_LOD)
	// Mid keeps half of High's candidate lattice. Compensate in projected blade width so
	// silhouettes against rocks and cliffs retain approximately the same coverage at the handoff.
	randWidth *= 1.41421356f;
#elif defined(LOW_LOD)
	randWidth *= 2.0f;
#endif

#if defined(FAR_VERTEX)
	bool doubleBlade = false;  // Far renders a single tapered blade.
#else
	bool doubleBlade = randHeight <= 45.0f;
#endif

	bool rotateFirstBlade = doubleBlade && !isBlade1;

	// Double blades run tip-to-base-to-tip, with each half reaching t = 1.
	float rung = vertexID >> 1;
	bool upperHalf = rung >= MID_LEVEL;
	float bladeLevels = doubleBlade ? (upperHalf ? DOUBLE_LEVELS : MID_LEVEL) : LEVELS;
	float invBladeLevels = doubleBlade ? (upperHalf ? INV_DOUBLE_LEVELS : INV_MID_LEVEL) : INV_LEVELS;

	float level = abs(rung - doubleBlade * MID_LEVEL);
	float t = level * invBladeLevels;
#if defined(FAR_LOD) || defined(MID_LOD) || defined(LOW_LOD)
	static const float appearanceStability = 1.0f;
#else
	// Fade High to the representative sample before individual blades become sub-pixel.
	float appearanceStability = smoothstep(512.0f, 1536.0f, rootDistance);
#endif
	// Keep some authored height variation around a tapered blade's area-weighted mean.
	static const float STABLE_APPEARANCE_T = 1.0f / 3.0f;
	static const float DISTANT_APPEARANCE_DETAIL = 0.25f;
	float stableAppearanceT = lerp(STABLE_APPEARANCE_T, t, DISTANT_APPEARANCE_DETAIL);
	float appearanceT = lerp(t, stableAppearanceT, appearanceStability);

	static const float COS_30 = 0.8660254f;
	static const float SIN_30 = 0.5f;
	float2 facing = rotateFirstBlade ? float2(randFacing.x * COS_30 - randFacing.y * SIN_30, randFacing.x * SIN_30 + randFacing.y * COS_30) : randFacing;

	float2 tip = tiltDir * randHeight;
#if !defined(FAR_LOD)
	float2 midPoint = tip * bladeType.mid + float2(-tip.y, tip.x) * randBend;  // Bezier control point used by the PS tangent
#endif

	uint side = vertexID & 1u;
	float sideSign = (side * 2.0f - 1.0f) * (1.0f - step(bladeLevels, level));

#if defined(FAR_VERTEX)
	// Far has only base and tip vertices, so its profile is a straight tapered segment.
	float2 bladePosition = t * tip;
	float taper = randWidth * (1.0f - t);
#else
	float2 bladePosition = 2.0f * (1.0f - t) * t * midPoint + t * t * tip;
	// Interpolate t squared to t to the fourth power across the fixed rungs instead of evaluating pow.
	float t2 = t * t;
	float taperCurve = lerp(t2, t2 * t2, widthScale);
	float taper = randWidth * (1.0f - taperCurve);
#endif

	// Build the blade in camera-relative space, then apply the animated tip displacement.
	float3 positionOffset = float3(facing * bladePosition.x, bladePosition.y) + float3(-facing.y, facing.x, 0.0f) * taper * sideSign;
	float windWeight = t * t;

	float3 previousPositionOffset = positionOffset;

#if defined(HIGH_LOD) || defined(MID_LOD)
	float2 windOffset = windDir * windDisplacement;
	positionOffset.xy += windOffset * windWeight;
	previousPositionOffset.xy += previousWindDir * previousWindDisplacement * windWeight;
#endif

	float4 viewPos = float4(rootViewPosition + positionOffset, 1.0f);
#if !defined(FAR_VERTEX)
	float4 previousViewPos = float4(rootViewPosition + previousPositionOffset + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz), 1.0f);
#endif

#if defined(PGRASS_CACHED_COLLISION)
	float3 collisionDisplacement = float3(
		f16tof32(blade.collisionData.x >> 16),
		f16tof32(blade.collisionData.x),
		f16tof32(blade.collisionData.y >> 16));
	float3 previousCollisionDisplacement = float3(
		f16tof32(blade.collisionData.y),
		f16tof32(blade.collisionData.z >> 16),
		f16tof32(blade.collisionData.z));
	float collisionWeight = t * t * (3.0f - 2.0f * t);
	viewPos.xyz += collisionDisplacement * collisionWeight;
	previousViewPos.xyz += previousCollisionDisplacement * collisionWeight;
#elif defined(GRASS_COLLISION) && !defined(FAR_LOD)
	float3 collisionDisplacement, previousCollisionDisplacement;
	// Smoothstep bends from a fixed root to full tip displacement.
	float collisionWeight = t * t * (3.0f - 2.0f * t);
	GrassCollision::GetDisplacedPosition(viewPos.xyz, rootViewPosition, collisionWeight, 2048.0, true, 0.75,
		collisionDisplacement, previousCollisionDisplacement);
	viewPos.xyz += collisionDisplacement;
	previousViewPos.xyz += previousCollisionDisplacement;
#endif

	float4 clipPosition = mul(FrameBuffer::CameraViewProj, viewPos);

// Widen edge-on blades to keep their silhouette visible.
#if defined(MID_VERTEX) || defined(LOW_VERTEX)
	// Mid/Low pack one 4-bit factor for each double-blade facing.
	uint packedViewThicken = (hashClumpAndGrassType >> 12) & 0xFFu;
	uint viewThickenNibble = rotateFirstBlade ? packedViewThicken >> 4 : packedViewThicken & 0xFu;
	float viewThicken = float(viewThickenNibble) * (1.0f / 15.0f);
	clipPosition.x += FrameBuffer::CameraProj._m00 * viewThicken * sideSign * taper * miscParams.z;
#elif defined(HIGH_VERTEX)
	// Match view thickening to the same stable origin used by the LOD transitions.
	float2 stableViewDirection = -rootLodOffset * rsqrt(max(dot(rootLodOffset, rootLodOffset), 1.0e-4f));
	float viewDotNormal = saturate(dot(facing, stableViewDirection));
	float viewDotNormal2 = viewDotNormal * viewDotNormal;
	float viewThicken = (1.0f - viewDotNormal2 * viewDotNormal2) * smoothstep(0.0f, 0.2f, viewDotNormal);
	clipPosition.x += FrameBuffer::CameraProj._m00 * viewThicken * sideSign * taper * miscParams.z;
#endif

	o.Position = clipPosition;
#if defined(DEPTH)
	o.BladeHeight = bladePosition.y;
#else
#	if defined(FAR_LOD)
	o.CameraPositionSide = float4(viewPos.xyz, (sideSign + 1.0f) * 0.5f);
	o.PackedBladeParams = uint4(blade.facingTilt, blade.seedAndType, blade.posZWidthHeight, asuint(farWidthT));
#	else
	// Reuse w components for side, Bezier t, and root-relative height.
	o.CameraRelativePosition = float4(viewPos.xyz, (sideSign + 1.0f) * 0.5f);
	o.PreviousCameraRelativePosition = float4(previousViewPos.xyz, t);
#		if defined(HIGH_LOD) || defined(MID_LOD)
	o.WindOffset = windOffset;
#		endif
	o.BezierTipAndMid = float4(tip, midPoint);
	// Mid evaluates three t values, so this polynomial preserves the authored curve at each rung.
#		if defined(MID_VERTEX)
	float roughnessT2 = appearanceT * appearanceT;
	float roughness = mad(mad(bladeType.midRoughnessPolynomial.x, appearanceT, bladeType.midRoughnessPolynomial.y), roughnessT2, bladeType.midRoughnessPolynomial.z);
#		else

	float roughness = lerp(bladeType.baseMinTipRoughnessStart.x, bladeType.baseMinTipRoughnessStart.y, smoothstep(0.0f, bladeType.baseMinTipRoughnessStart.w, appearanceT));
	roughness = lerp(roughness, bladeType.baseMinTipRoughnessStart.z, smoothstep(bladeType.baseMinTipRoughnessStart.x, 1.0f, appearanceT));
#		endif

	float bladeAO = lerp(bladeType.minAO, 1.0f, appearanceT);
	float clumpAO = lerp(1.0f, bladeType.minAO, clumpDensity * bladeType.clumpAOStrength);
	o.AOThicknessRoughness = float4(bladeAO * clumpAO, lerp(bladeType.minMaxSubsurfaceOpacity.x, bladeType.minMaxSubsurfaceOpacity.y, appearanceT), roughness, bladePosition.y);
#	endif

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

#	if defined(FAR_LOD)
	float3 hueTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, bladeRand);
	float bladeValue = 1.0f + (bladeRand2 * 2.0f - 1.0f) * bladeType.grassColorVar.y;
	float3 perBladeColor = lerp(1.0f, hueTint, bladeType.grassColorVar.x) * bladeValue;
#	else
	uint packedBladeColor = blade.previousWind >> 16;
	float3 perBladeColor = float3(packedBladeColor & 31u, (packedBladeColor >> 5u) & 63u, (packedBladeColor >> 11u) & 31u) * float3(2.0f / 31.0f, 2.0f / 63.0f, 2.0f / 31.0f);
#	endif
	// Retain low-frequency Voronoi colour as individual blade variation fades.
	float clumpColorRand = (float(clumpSeed & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float clumpValueRand = (float((clumpSeed >> 8) & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float3 clumpTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, clumpColorRand);
	float clumpValue = 1.0f + (clumpValueRand * 2.0f - 1.0f) * bladeType.grassColorVar.y * 0.75f;
	float3 stableClumpColor = lerp(1.0f, clumpTint * clumpValue, bladeType.clumpColorStrength);
	perBladeColor = lerp(perBladeColor, stableClumpColor, appearanceStability);

	// Apply base shading and tip drying at the stabilized blade sample.
	float3 tipDryMul = lerp(1.0f, bladeType.grassColorTipDry.rgb, smoothstep(0.5f, 1.0f, appearanceT) * bladeType.grassColorVar.z);
	float baseShade = lerp(1.0f - grassLightParams.w, 1.0f, smoothstep(0.0f, 0.5f, appearanceT));

	float3 baseToTipColor = lerp(bladeType.baseColor.rgb, bladeType.tipColor.rgb, appearanceT) * perBladeColor * tipDryMul * baseShade;
#	if defined(FAR_LOD)
	o.BladeTColor = float4(appearanceT, baseToTipColor);
#	else
	float detailRand = frac((float)packedBladeColor * 0.61803398875f + 0.17f);
	float detailRand2 = frac((float)packedBladeColor * 0.38196601125f + 0.61f);
	// Pack facing, type, and two detail seeds into one flat interpolator.
	o.BladeParams = float4(facing, float(grassTypeIndex),
		asfloat((f32tof16(detailRand) << 16) | f32tof16(detailRand2)));
	o.BaseToTipColor = float4(baseToTipColor, clipPosition.w);
#	endif

#	if defined(SKYLIGHTING) && defined(HIGH_LOD)
	// Generator packs four f16 SH coefficients per High blade.
	o.SkylightingVertexSH = float4(f16tof32(blade.skylightingSH0 >> 16), f16tof32(blade.skylightingSH0),
		f16tof32(blade.skylightingSH1 >> 16), f16tof32(blade.skylightingSH1));
#	endif
#endif

	return o;
}
