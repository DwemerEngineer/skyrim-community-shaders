#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"

#define PSHADER
#include "Common/SharedData.hlsli"
#undef PSHADER

#include "ProceduralGrass/PGrassCommon.hlsli"

#define VSHADER
#define FRAMEBUFFER

StructuredBuffer<Blade> Blades : register(t0);
#if defined(HIGH_OUTER_VERTEX)
ByteAddressBuffer IndirectArgs : register(t1);
#endif

struct VS_OUTPUT
{
	precise float4 Position : SV_POSITION;
#if defined(DEPTH)
#	if defined(DEPTH_CLIP)
	float BladeHeight : TEXCOORD0;
#	endif
#elif defined(FAR_LOD)
	float4 CameraPositionSide : TEXCOORD0;  // xyz: camera-relative position; w: across-blade coordinate
	float4 BladeTColor : TEXCOORD1;         // x: blade parameter; yzw: base-to-tip colour
	nointerpolation uint4 PackedBladeParams : TEXCOORD2;  // facing/tilt, seed/type, root Z/width/height, continuous Far ramp
#else
	float4 CameraRelativePosition : TEXCOORD0;  // xyz: camera-relative position; w: across-blade coordinate
#	if defined(HIGH_LOD)
	float4 PreviousCameraRelativePosition : TEXCOORD1;  // xyz: previous camera-relative position; w: Bezier t
#	elif defined(LOW_LOD)
	float BladeT : TEXCOORD1;
#	endif
#	if defined(HIGH_LOD)
	nointerpolation float4 WindLodDensity : TEXCOORD2;  // xy: tip wind offset; w: canopy density and shadow
#	elif defined(MID_LOD)
	nointerpolation float4 WindRootPosition : TEXCOORD2;  // xy: tip wind offset; zw: root camera-relative XY
#	elif defined(LOW_LOD)
	nointerpolation float2 RootPosition : TEXCOORD2;  // camera-relative blade root XY
#	endif
#	if !defined(LOW_LOD)
	float4 AOThicknessRoughness : TEXCOORD3;  // xyz: AO, thickness, roughness; w: root-relative height, or Bezier t for Mid
#	endif
	nointerpolation float4 BezierTipAndMid : TEXCOORD4;  // xy: tip; zw: midpoint in facing/up space
	nointerpolation float4 BladeParams : TEXCOORD5;  // xy: facing; z: type; w: two f16 randoms
#	if !defined(LOW_LOD)
	float4 BaseToTipColor : TEXCOORD7;  // xyz: blade colour; w: positive view depth
#	endif
#	if defined(SKYLIGHTING) && !defined(LOW_LOD)
	nointerpolation float4 SkylightingVertexSH : TEXCOORD9;  // Per-blade SH from the generator.
#	endif
#endif
};

VS_OUTPUT main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VS_OUTPUT o;
#if defined(HIGH_OUTER_VERTEX)
	// D3D11 does not add StartInstanceLocation to SV_InstanceID. Outer High lives at the tail of the shared blade buffer.
	instanceID += IndirectArgs.Load(36u);
#endif
	Blade blade = Blades[instanceID];

#if defined(HIGH_OUTER_VERTEX)
	static const float LEVELS = 3.0f;
	static const float DOUBLE_LEVELS = 2.0f;
	static const float MID_LEVEL = 1.0f;
	bool isBlade1 = vertexID >= 4u;
#elif defined(HIGH_VERTEX)
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
	bool isBlade1 = false;
#endif

#if !defined(LOW_VERTEX)
	static const float INV_LEVELS = 1.0f / LEVELS;
	static const float INV_DOUBLE_LEVELS = 1.0f / DOUBLE_LEVELS;
	static const float INV_MID_LEVEL = 1.0f / MID_LEVEL;
#endif

#if defined(FAR_LOD)
	uint grassTypeIndex = blade.seedAndType & 0xFFu;
	uint bladeSeed = (blade.seedAndType >> 8) & 0xFFFFu;
	uint clumpSeed = bladeSeed;
#else
	uint hashClumpAndGrassType = blade.hashClumpAndGrassType;
	uint grassTypeIndex = hashClumpAndGrassType & 0xFFu;
	uint clumpSeed = (hashClumpAndGrassType >> 8) & 0xFFu;
#endif

#if !defined(LOW_LOD) || defined(FAR_LOD)
#	if defined(DEPTH)
	GrassGeneratorType bladeType = generatorGrassType[grassTypeIndex];
#	else
	GrassType bladeType = grassType[grassTypeIndex];
#	endif
#endif
	float3 rootViewPosition = float3(
		f16tof32(blade.posXY >> 16),
		f16tof32(blade.posXY),
		f16tof32(blade.posZWidthHeight >> 16));
#if defined(MID_LOD)
	float rootDistance = float(blade.tipDir >> 16) * (6144.0f / 65535.0f);
#elif defined(FAR_LOD)
	float2 rootLodOffset = rootViewPosition.xy + FrameBuffer::CameraPosAdjust.xy - grassLodOrigin;
	float rootDistance = ApproximateGrassDistance(rootLodOffset);
#endif

#if !defined(DEPTH) && defined(HIGH_LOD)
	uint packedCanopy = hashClumpAndGrassType >> 24;
	uint packedCanopyShadow = packedCanopy | ((blade.tipDir >> 16) & 0xFFu) << 8;
#endif

#if !defined(LOW_LOD) || defined(FAR_LOD)
	float2 tiltDir;
#endif

#if defined(FAR_LOD)
	float4 packedDirections = float4(blade.facingTilt & 0xFF, (blade.facingTilt >> 8) & 0xFF, (blade.facingTilt >> 16) & 0xFF, blade.facingTilt >> 24);
	packedDirections = packedDirections * (2.0f / 255.0f) - 1.0f;
	float2 randFacing = packedDirections.xy;

	tiltDir = packedDirections.zw;
#else
	int2 packedFacing = int2(blade.facingAndWind << 24, blade.facingAndWind << 16) >> 24;
	float2 randFacing = float2(packedFacing) * (1.0f / 127.0f);

#	if defined(LOW_LOD)
	float randWidth = f16tof32(blade.facingAndWind >> 16);
	float2 tip = float2(f16tof32(blade.tipDir >> 16), f16tof32(blade.tipDir));
	float2 midPoint = float2(f16tof32(blade.previousWind >> 16), f16tof32(blade.previousWind));
#	else
	float windDisplacement = f16tof32(blade.facingAndWind >> 16);
#		if defined(HIGH_LOD) && !defined(DEPTH)
	float previousWindDisplacement = f16tof32(blade.previousWind);
#		endif

	uint packedBladeData = blade.previousWind >> 16;
	uint packedBladeColor = packedBladeData & 0xFFFu;
	float randBend = bladeType.stiffness * (0.25f + float(packedBladeData >> 12) * (1.6f / 15.0f));
#		if defined(HIGH_LOD)
	float clumpDensity = float((hashClumpAndGrassType >> 16) & 0xFu) * (1.0f / 15.0f);
#		else
	float clumpDensity = float(hashClumpAndGrassType >> 24) * (1.0f / 255.0f);
#		endif
	uint2 packedTilt = uint2(blade.tipDir & 0xFFu, (blade.tipDir >> 8) & 0xFFu);
	tiltDir = float2(packedTilt) * (2.0f / 255.0f) - 1.0f;
#	endif
#endif
#if defined(LOW_LOD) && !defined(FAR_LOD)
	float widthScale = ((blade.posZWidthHeight >> 8) & 0xFFu) * (1.0f / 255.0f);
#else
	float randHeight = bladeType.height * (blade.posZWidthHeight & 0xFFu) * (1.0f / 255.0f);
	float widthScale = ((blade.posZWidthHeight >> 8) & 0xFFu) * (1.0f / 255.0f);
#if defined(MID_LOD)
	// Evaluate distance widening per vertex to keep it continuous as the camera moves.
	float distanceWidth = lerp(0.4f, 1.0f, saturate((rootDistance - 1024.0f) * (1.0f / 3072.0f)));
	widthScale *= distanceWidth;
#endif
	float randWidth = bladeType.width * 2.5f * lerp(0.45f, 1.3f, widthScale);
#endif

#if defined(FAR_LOD)
	float farWidthT = saturate((rootDistance - farParams.x) * farParams.y);
	float performanceKeep = GetFarPerformanceKeep(rootDistance, FrameBuffer::CameraProj._m00);
	float coverageCompensation = min(rcp(max(performanceKeep, 0.5f)), 2.0f);
	randWidth *= lerp(2.0f, 32.0f, farWidthT) * coverageCompensation;
#elif defined(MID_LOD)
	// Mid keeps half of High's candidate lattice, so double width to preserve projected coverage.
	randWidth *= 2.0f;
#endif

#if defined(FAR_VERTEX)
	bool doubleBlade = false;  // Far renders a single tapered blade.
#elif defined(LOW_LOD)
	bool doubleBlade = (blade.posZWidthHeight & 0xFFu) != 0u;
#else
	bool doubleBlade = randHeight <= 45.0f;
#endif

#if defined(LOW_VERTEX)
	isBlade1 = doubleBlade && vertexID == 3u;
	bool rotateFirstBlade = doubleBlade && !isBlade1;
	float t = doubleBlade ? ((vertexID == 0u || vertexID == 3u) ? 1.0f : 0.0f) : (vertexID >= 2u ? 1.0f : 0.0f);
#else
	bool rotateFirstBlade = doubleBlade && !isBlade1;

	// Double blades run tip-to-base-to-tip, with each half reaching t = 1.
	float rung = vertexID >> 1;
	bool upperHalf = rung >= MID_LEVEL;
	float bladeLevels = doubleBlade ? (upperHalf ? DOUBLE_LEVELS : MID_LEVEL) : LEVELS;
	float invBladeLevels = doubleBlade ? (upperHalf ? INV_DOUBLE_LEVELS : INV_MID_LEVEL) : INV_LEVELS;

	float level = abs(rung - doubleBlade * MID_LEVEL);
	float t = level * invBladeLevels;
#endif
	static const float appearanceStability = 1.0f;
	// Keep some authored height variation around a tapered blade's area-weighted mean.
	static const float STABLE_APPEARANCE_T = 1.0f / 3.0f;
	static const float DISTANT_APPEARANCE_DETAIL = 0.25f;
	float stableAppearanceT = lerp(STABLE_APPEARANCE_T, t, DISTANT_APPEARANCE_DETAIL);
	float appearanceT = lerp(t, stableAppearanceT, appearanceStability);

	static const float COS_30 = 0.8660254f;
	static const float SIN_30 = 0.5f;
	float2 rotatedFacing = float2(dot(randFacing, float2(COS_30, -SIN_30)), dot(randFacing, float2(SIN_30, COS_30)));
	float2 facing = rotateFirstBlade ? rotatedFacing : randFacing;

#if !defined(LOW_LOD) || defined(FAR_LOD)
	float2 tip = tiltDir * randHeight;
#	if !defined(FAR_LOD)
	float2 midPoint = tip * bladeType.mid + float2(-tip.y, tip.x) * randBend;  // Bezier control point used by the PS tangent
#	endif
#endif

#if defined(LOW_VERTEX)
	float sideSign = doubleBlade ? (vertexID == 1u ? -1.0f : (vertexID == 2u ? 1.0f : 0.0f)) : mad(float(vertexID & 1u), 2.0f, -1.0f);
#else
	uint side = vertexID & 1u;
	float sideSign = mad(float(side), 2.0f, -1.0f) * (1.0f - step(bladeLevels, level));
#endif

#if defined(FAR_VERTEX)
	// Far has only base and tip vertices, so its profile is a straight tapered segment.
	float2 bladePosition = t * tip;
	float taper = randWidth * (1.0f - t);
#else
	float t2 = t * t;
	float midWeight = mad(-2.0f, t2, 2.0f * t);
	float2 bladePosition = mad(midPoint, midWeight, t2 * tip);
	// Interpolate t squared to t to the fourth power across the fixed rungs without evaluating pow.
	float taperScale = mad(widthScale, t2 - 1.0f, 1.0f);
	float taper = randWidth * mad(-t2, taperScale, 1.0f);
#if defined(LOW_VERTEX)
	if (!doubleBlade)
		taper = max(taper, randWidth * 0.06f);
#endif
#endif

	// Build the blade in camera-relative space, then apply the animated tip displacement.
	float2 bladeOffsetXY = mad(float2(-facing.y, facing.x), taper * sideSign, facing * bladePosition.x);
	float3 positionOffset = float3(bladeOffsetXY, bladePosition.y);
	float windWeight = t * t;

#if defined(HIGH_LOD) && !defined(DEPTH)
	float3 previousPositionOffset = positionOffset;
#endif

#if defined(HIGH_LOD) || defined(MID_LOD)
	float2 windOffset = windDir * windDisplacement;
	positionOffset.xy += windOffset * windWeight;
#	if defined(HIGH_LOD) && !defined(DEPTH)
	previousPositionOffset.xy += previousWindDir * previousWindDisplacement * windWeight;
#	endif
#endif

	float4 viewPos = float4(rootViewPosition + positionOffset, 1.0f);
#if defined(HIGH_LOD) && !defined(DEPTH)
	float4 previousViewPos = float4(rootViewPosition + previousPositionOffset + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz), 1.0f);
#endif

#if defined(PGRASS_CACHED_COLLISION)
#	if defined(MID_LOD)
	float3 collisionDisplacement = float3(f16tof32(blade.collisionData >> 16), f16tof32(blade.collisionData), f16tof32(blade.previousWind));
#	else
	float3 collisionDisplacement = float3(f16tof32(blade.collisionData.x >> 16), f16tof32(blade.collisionData.x), f16tof32(blade.collisionData.y >> 16));
	float3 previousCollisionDisplacement = float3(f16tof32(blade.collisionData.y), f16tof32(blade.collisionData.z >> 16), f16tof32(blade.collisionData.z));
#	endif
	float collisionWeight = t * t * (3.0f - 2.0f * t);
	viewPos.xyz += collisionDisplacement * collisionWeight;
#	if defined(HIGH_LOD) && !defined(DEPTH)
	previousViewPos.xyz += previousCollisionDisplacement * collisionWeight;
#	endif
#endif

	float4 clipPosition = mul(FrameBuffer::CameraViewProj, viewPos);

#if defined(MID_VERTEX) || defined(LOW_VERTEX) || defined(FAR_VERTEX)
	// Allow terrain LOD depth differences near the loaded-grid edge without moving blades off LAND.
	float2 terrainEdgeOffset = rootViewPosition.xy + FrameBuffer::CameraPosAdjust.xy - grassLodOrigin;
	float terrainEdgeDistance = max(abs(terrainEdgeOffset.x), abs(terrainEdgeOffset.y));
	[branch] if (terrainEdgeDistance > farParams.x - 2048.0f) {
		float terrainLODDepthMargin = 128.0f * smoothstep(farParams.x - 2048.0f, farParams.x, terrainEdgeDistance);
		clipPosition.z += FrameBuffer::CameraProj._m23 * terrainLODDepthMargin / max(clipPosition.w - terrainLODDepthMargin, 1.0f);
	}
#endif

// Widen edge-on blades to keep their silhouette visible.
#if defined(MID_VERTEX) || defined(LOW_VERTEX)
	// Mid/Low pack one 4-bit factor for each double-blade facing.
	uint packedViewThicken = (hashClumpAndGrassType >> 16) & 0xFFu;
	uint viewThickenNibble = rotateFirstBlade ? packedViewThicken >> 4 : packedViewThicken & 0xFu;
	float viewThicken = float(viewThickenNibble) * (1.0f / 15.0f);
	clipPosition.x = mad(FrameBuffer::CameraProj._m00 * viewThicken * sideSign * taper, miscParams.z, clipPosition.x);
#elif defined(HIGH_VERTEX) || defined(HIGH_OUTER_VERTEX)
	float viewThicken = float((hashClumpAndGrassType >> 20) & 0xFu) * (1.0f / 15.0f);
	clipPosition.x = mad(FrameBuffer::CameraProj._m00 * viewThicken * sideSign * taper, miscParams.z, clipPosition.x);
#endif

	o.Position = clipPosition;
#if defined(DEPTH)
#	if defined(DEPTH_CLIP)
	o.BladeHeight = bladePosition.y;
#	endif
#else
#	if defined(FAR_LOD)
	o.CameraPositionSide = float4(viewPos.xyz, mad(sideSign, 0.5f, 0.5f));
	float handoffDistance = max(abs(rootLodOffset.x), abs(rootLodOffset.y));
	float tipMatch = 1.0f - smoothstep(farParams.x, farParams.x + 4096.0f, handoffDistance);
	uint packedFarRamps = f32tof16(farWidthT) | f32tof16(tipMatch) << 16;
	o.PackedBladeParams = uint4(blade.facingTilt, blade.seedAndType, blade.posZWidthHeight, packedFarRamps);
#	else
	// Reuse w components for side, Bezier t, and root-relative height.
	o.CameraRelativePosition = float4(viewPos.xyz, mad(sideSign, 0.5f, 0.5f));
#		if defined(HIGH_LOD)
	o.PreviousCameraRelativePosition = float4(previousViewPos.xyz, t);
#		elif defined(LOW_LOD)
	o.BladeT = t;
#		endif
#		if defined(HIGH_LOD)
	o.WindLodDensity = float4(windOffset, 0.0f, float(packedCanopyShadow));
#		elif defined(MID_LOD)
	o.WindRootPosition = float4(windOffset, rootViewPosition.xy);
#		elif defined(LOW_LOD)
	o.RootPosition = rootViewPosition.xy;
#		endif
	o.BezierTipAndMid = float4(tip, midPoint);
#		if !defined(LOW_LOD)
	// Mid evaluates three t values, so this polynomial preserves the authored curve at each rung.
#			if defined(MID_VERTEX)
	float roughnessT2 = appearanceT * appearanceT;
	float roughness = mad(mad(bladeType.midRoughnessPolynomial.x, appearanceT, bladeType.midRoughnessPolynomial.y), roughnessT2, bladeType.midRoughnessPolynomial.z);
#			else
	float roughness = lerp(bladeType.baseMinTipRoughnessStart.x, bladeType.baseMinTipRoughnessStart.y, smoothstep(0.0f, bladeType.baseMinTipRoughnessStart.w, appearanceT));
	roughness = lerp(roughness, bladeType.baseMinTipRoughnessStart.z, smoothstep(bladeType.baseMinTipRoughnessStart.x, 1.0f, appearanceT));
#			endif

	float bladeAO = lerp(bladeType.minAO, 1.0f, appearanceT);
	float clumpAO = lerp(1.0f, bladeType.minAO, clumpDensity * bladeType.clumpAOStrength);
	float heightOrT = bladePosition.y;
#			if defined(MID_LOD)
	heightOrT = t;
#			endif
	o.AOThicknessRoughness = float4(bladeAO * clumpAO, lerp(bladeType.minMaxSubsurfaceOpacity.x, bladeType.minMaxSubsurfaceOpacity.y, appearanceT), roughness, heightOrT);
#		endif
#	endif

#	if defined(LOW_LOD) && !defined(FAR_LOD)
	// Low shades very few pixels after its depth prepass. Pass packed blade data and reconstruct
	// material appearance in the PS instead of evaluating it for every Low vertex.
	o.BladeParams = float4(facing, float(grassTypeIndex), asfloat(hashClumpAndGrassType));
#	else
#	if defined(FAR_LOD)
	float bladeRand = (float(bladeSeed & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float bladeRand2 = (float(bladeSeed >> 8) + 0.5f) * (1.0f / 256.0f);
#	endif

#	if defined(FAR_LOD)
	float3 hueTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, bladeRand);
	float bladeValue = 1.0f + (bladeRand2 * 2.0f - 1.0f) * bladeType.grassColorVar.y;
	float3 perBladeColor = lerp(1.0f, hueTint, bladeType.grassColorVar.x) * bladeValue;
#	else
	float3 perBladeColor = float3(packedBladeColor & 15u, (packedBladeColor >> 4u) & 15u, (packedBladeColor >> 8u) & 15u) * (2.0f / 15.0f);
#	endif
	// Retain low-frequency Voronoi colour as individual blade variation fades.
	float clumpColorRand = (float(clumpSeed & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float clumpValueRand = (float((clumpSeed * 73u + 41u) & 0xFFu) + 0.5f) * (1.0f / 256.0f);
	float3 clumpTint = lerp(bladeType.grassColorCool.rgb, bladeType.grassColorWarm.rgb, clumpColorRand);
	float clumpValue = 1.0f + (clumpValueRand * 2.0f - 1.0f) * bladeType.grassColorVar.y * 0.75f;
	float3 stableClumpColor = lerp(1.0f, clumpTint * clumpValue, bladeType.clumpColorStrength);
	perBladeColor = lerp(perBladeColor, stableClumpColor, appearanceStability);

	// Apply base shading and tip drying at the stabilized blade sample.
	float3 tipDryMul = lerp(1.0f, bladeType.grassColorTipDry.rgb, smoothstep(0.5f, 1.0f, appearanceT) * bladeType.grassColorVar.z);
	float baseShade = lerp(1.0f - grassLightParams.w, 1.0f, smoothstep(0.0f, 0.5f, appearanceT));

	float3 baseToTipColor = lerp(bladeType.baseColor.rgb, bladeType.tipColor.rgb, appearanceT) * perBladeColor * tipDryMul * baseShade;
#	if defined(FAR_LOD)
		o.BladeTColor = float4(t, baseToTipColor);
#	else
	float detailRand = frac((float)packedBladeColor * 0.61803398875f + 0.17f);
	float detailRand2 = frac((float)packedBladeColor * 0.38196601125f + 0.61f);
	// Pack facing, type, and pixel-shader detail data into one flat interpolator.
#	if defined(MID_LOD)
	uint packedDetail = (uint)round(detailRand * 255.0f) | (uint)round(detailRand2 * 255.0f) << 8 | (blade.tipDir & 0xFFFF0000u);
	o.BladeParams = float4(facing, float(grassTypeIndex), asfloat(packedDetail));
#	else
	o.BladeParams = float4(facing, float(grassTypeIndex),
		asfloat((f32tof16(detailRand) << 16) | f32tof16(detailRand2)));
#	endif
	o.BaseToTipColor = float4(baseToTipColor, clipPosition.w);
#	endif

#	endif

#	if defined(SKYLIGHTING) && !defined(LOW_LOD)
	o.SkylightingVertexSH = float4(f16tof32(blade.skylightingSH0 >> 16), f16tof32(blade.skylightingSH0),
		f16tof32(blade.skylightingSH1 >> 16), f16tof32(blade.skylightingSH1));
#	endif
#endif

	return o;
}
