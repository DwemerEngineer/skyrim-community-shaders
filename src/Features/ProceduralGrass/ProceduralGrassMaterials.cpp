#include "Features/ProceduralGrass.h"

#include "Features/LinearLighting.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace PGrassCommon;

namespace
{
	/** @brief Deterministic 32-bit hash of two unsigned coordinates. */
	uint32_t Hash2D(uint32_t x, uint32_t y)
	{
		x ^= y * 0x9E3779B9u;
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		return x ^ (x >> 16);
	}

	/** @brief Cubic smoothstep; equal edges produce a step at edge0. */
	float Smoothstep(float edge0, float edge1, float value)
	{
		if (edge0 == edge1)
			return value < edge0 ? 0.0f : 1.0f;
		const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/** @brief Roughness-dependent coefficients of PBR::FuzzDirectionalAlbedo's MaterialX fit. */
	float4 GetFuzzDirectionalAlbedoParameters(float roughness)
	{
		const float r = std::clamp(roughness, 0.01f, 1.0f);
		const float s = r * (0.0206607f + 1.58491f * r) / (0.0379424f + r * (1.32227f + r));
		const float m = r * (-0.193854f + r * (-1.14885f + r * (1.7932f - 0.95943f * r * r))) / (0.046391f + r);
		const float o = r * (0.000654023f + (-0.0207818f + 0.119681f * r) * r) / (1.26264f + r * (-1.92021f + r));
		return float4(1.0f / s, m, 1.0f / (s * std::sqrt(2.0f * std::numbers::pi_v<float>)), o);
	}

	/** @brief Mean packed vertical height and triangle area, evaluated only on material updates. */
	float2 GetDistantCanopyGeometry(const GrassType& type)
	{
		constexpr uint32_t samples = 32;
		float meanTilt = 0.0f;
		float meanArea = 0.0f;
		float meanAbsoluteTilt = 0.0f;
		for (uint32_t i = 0; i < samples; ++i) {
			const float random = (static_cast<float>(i) + 0.5f) / samples;
			const float tilt = std::cos(type.tipWeight * (random * 1.4f + 0.3f));
			const float packedTilt = std::round((tilt * 0.5f + 0.5f) * 255.0f) * (2.0f / 255.0f) - 1.0f;
			meanTilt += packedTilt / samples;
			meanAbsoluteTilt += std::abs(packedTilt) / samples;
			const float height = 0.45f + std::lerp(random, 0.5f, type.clumpHeightFactor) * 0.55f - 0.5f / 255.0f;
			for (uint32_t j = 0; j < samples; ++j) {
				const float angle = (static_cast<float>(j) + 0.5f) / samples;
				const float widthRandom = random * 1.618f + angle * 0.5f;
				const float width = std::lerp(0.6f, 1.0f, widthRandom - std::floor(widthRandom)) - 0.5f / 255.0f;
				meanArea += height * std::lerp(0.45f, 1.3f, width) / (samples * samples);
			}
		}
		return float2((0.725f - 0.5f / 255.0f) * meanTilt, type.width * 2.5f * type.height * meanArea * meanAbsoluteTilt);
	}

	float GrassMaterialDetailTexel(int32_t x, int32_t y, bool grain)
	{
		const uint32_t wrappedX = static_cast<uint32_t>(x) & (GrassMaterialDetailDim - 1u);
		const uint32_t wrappedY = static_cast<uint32_t>(y) & (GrassMaterialDetailDim - 1u);
		const uint32_t hash = grain ?
		                          Hash2D(wrappedX, wrappedY) :
		                          Hash2D(wrappedX / 8u, wrappedY / 8u);
		return static_cast<float>(hash >> 24) * (1.0f / 255.0f);
	}

	float SampleGrassMaterialDetail(float u, float v, bool grain)
	{
		const float x = u * GrassMaterialDetailDim - 0.5f;
		const float y = v * GrassMaterialDetailDim - 0.5f;
		const int32_t x0 = static_cast<int32_t>(std::floor(x));
		const int32_t y0 = static_cast<int32_t>(std::floor(y));
		const float fx = x - x0;
		const float fy = y - y0;
		const float a = GrassMaterialDetailTexel(x0, y0, grain);
		const float b = GrassMaterialDetailTexel(x0 + 1, y0, grain);
		const float c = GrassMaterialDetailTexel(x0, y0 + 1, grain);
		const float d = GrassMaterialDetailTexel(x0 + 1, y0 + 1, grain);
		return std::lerp(std::lerp(a, b, fx), std::lerp(c, d, fx), fy);
	}

}

std::string ProceduralGrass::LandTextureKey(const RE::TESLandTexture* tex)
{
	if (!tex)
		return {};
	const RE::TESFile* file = tex->GetFile(0);
	if (!file)
		return {};
	return std::format("{}|0x{:06X}", file->GetFilename(), tex->GetLocalFormID());
}

void ProceduralGrass::RebuildTypeAllocation()
{
	RebuildObjectGrassRules();
	typeAllocation.clear();
	textureSelection.clear();
	textureSelectionByTexture.clear();
	grassCellCachePolicyDirty = true;
	grassTypesDirty = true;
	typeAllocation.reserve(PGrassCommon::MaxGrassTypes - 2);
	textureSelection.reserve(settings.textureTypes.size());
	textureSelectionByTexture.reserve(settings.textureTypes.size());

	// Sort keys so cached type ids remain stable across frames.
	std::vector<std::string> keys;
	keys.reserve(settings.textureTypes.size());
	for (const auto& [key, defs] : settings.textureTypes)
		if (!defs.empty())
			keys.push_back(key);

	std::sort(keys.begin(), keys.end());

	// Slots 0 and 1 are reserved for bare and base grass.
	for (const auto& key : keys) {
		const auto& defs = settings.textureTypes[key];
		TextureSelection sel;
		float acc = 0.0f;

		for (uint32_t i = 0; i < defs.size(); i++) {
			if (defs[i].noGrass) {
				sel.ids.push_back(0u);
			} else {
				if (typeAllocation.size() + 2 >= PGrassCommon::MaxGrassTypes)
					break;  // The type pool is full; this texture keeps only the variants allocated so far.
				sel.ids.push_back(static_cast<uint8_t>(typeAllocation.size() + 2));
				typeAllocation.emplace_back(key, i);
			}
			acc += std::max(0.0f, defs[i].weight);
			sel.cumulative.push_back(acc);
		}

		sel.total = acc;
		if (!sel.ids.empty())
			textureSelection[key] = std::move(sel);
	}
}

PGrassCommon::GrassType ProceduralGrass::BuildGrassType(const GrassTypeFields& s)
{
	const auto packColor = [](const float3& c) { return float4(c.x, c.y, c.z, 0.0f); };
	const auto detailScale = [](float value) {
		// Keep grain coordinates within int32_t after frequency and texture-size scaling.
		constexpr float maxScale = static_cast<float>(INT32_MAX / (32u * GrassMaterialDetailDim));
		return std::isfinite(value) ? std::clamp(value, 0.0f, maxScale) : 0.0f;
	};
	constexpr float degreesToRadians = std::numbers::pi_v<float> / 180.0f;

	PGrassCommon::GrassType t{};
	t.height = s.grassHeight;
	t.width = s.grassWidth;
	t.minSlope = std::cos(s.grassMinSlope * degreesToRadians);
	t.maxSlope = std::cos(s.grassMaxSlope * degreesToRadians);
	t.stiffness = s.stiffness;
	t.rotationalStiffness = s.rotationalStiffness;
	t.tipWeight = s.tipWeight;
	t.mid = s.mid;

	// Keep the generator's reciprocal grid size finite and interpolated clump heights within the type bounds.
	t.clumpGridSize = std::clamp(s.clumpGridSize, MinClumpGridSize, MaxClumpGridSize);
	t.clumpDistanceFactor = s.clumpDistanceFactor;
	t.clumpFacingFactor = s.clumpFacingFactor;
	t.clumpLeanFactor = s.clumpLeanFactor;
	t.clumpHeightFactor = std::clamp(s.clumpHeightFactor, 0.0f, 1.0f);
	t.clumpAOStrength = s.clumpAOStrength;
	t.clumpColorStrength = s.grassClumpColorStrength;

	t.minAO = s.ao;
	t.specular = s.specular;
	t.specularAnisotropy = std::clamp(s.specularAnisotropy, 0.0f, 1.0f);
	t.minMaxSubsurfaceOpacity = s.subsurfaceOpacity;
	t.grassSubsurfaceColor = packColor(s.grassSubsurfaceTint);
	t.grassSubsurfaceColor.w = std::clamp(s.sheenTint, 0.0f, 1.0f);
	t.grassSurfParams = float4(s.sheenStrength, std::clamp(s.grassTransmissionStrength, 0.0f, 2.0f), std::clamp(s.grassSlopeFacing, 0.0f, 1.0f), s.sheenRoughness);

	const float3 rough = s.baseMinTipRoughness;
	const float roughnessStart = s.tipRoughnessStart;
	t.baseMinTipRoughnessStart = float4(rough.x, rough.y, rough.z, roughnessStart);

	// Fit Mid roughness at its three vertex positions to avoid evaluating both smoothstep curves in the vertex shader.
	const float roughnessAtMidFirst = std::lerp(rough.x, rough.y, Smoothstep(0.0f, roughnessStart, 0.5f));
	const float roughnessAtMid = std::lerp(roughnessAtMidFirst, rough.z, Smoothstep(rough.x, 1.0f, 0.5f));
	const float baseToMid = roughnessAtMid - rough.x;
	const float baseToTip = rough.z - rough.x;
	t.midRoughnessPolynomial = float4(2.0f * baseToTip - 8.0f * baseToMid, 8.0f * baseToMid - baseToTip, rough.x, 0.0f);

	t.grassTypeLightParams = float4(s.grassBounceStrength, 0.0f, 0.0f, s.grassAmbientDesat);

	t.baseColor = packColor(s.baseColor);
	t.tipColor = packColor(s.tipColor);
	t.grassColorTipDry = packColor(s.grassColorTipDry);
	t.grassColorVar = float4(s.grassColorHueVariation, s.grassColorValueVariation, s.grassColorTipDryStrength, s.grassColorMottleStrength);
	t.grassColorCool = packColor(s.grassColorCool);
	t.grassColorWarm = packColor(s.grassColorWarm);
	t.grassBounceColor = packColor(s.grassBounceColor);
	t.grassTextureParams = float4(s.grassBlotchStrength, detailScale(s.grassBlotchScale), s.grassSpeckleStrength, detailScale(s.grassSpeckleScale));
	t.grassVeinParams = float4(s.grassVeinTint.x, s.grassVeinTint.y, s.grassVeinTint.z, s.grassVeinAlbedoStrength);
	t.grassVeinParams2 = float4(s.grassVeinNormalStrength, s.grassVeinRippleDepth, s.grassVeinWiggleAmount, s.curvedNormalStrength);

	return t;
}

void ProceduralGrass::UpdateGrassMaterialDetailTexture()
{
	if (!grassMaterialDetailTexture)
		return;

	const auto encodeUNorm8 = [](float value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
	};

	std::vector<uint8_t> detailData;

	// Slot 0 is bare; only generated grass needs material detail.
	for (uint32_t typeIndex = 1; typeIndex < ActiveGrassTypeCount(); ++typeIndex) {
		const auto& type = resolvedGrassTypes.grassType[typeIndex];
		const float blotchScale = std::max(type.grassTextureParams.y, 0.0f);
		const float speckleScale = std::max(type.grassTextureParams.w, 0.0f);
		const float veinStrength = type.grassVeinParams2.x;
		const float veinRippleDepth = type.grassVeinParams2.y;
		const float veinWiggleAmount = type.grassVeinParams2.z;

		uint64_t detailHash = GrassHashOffsetBasis;
		GrassHashValue(detailHash, blotchScale);
		GrassHashValue(detailHash, speckleScale);
		GrassHashValue(detailHash, veinStrength);
		GrassHashValue(detailHash, veinRippleDepth);
		GrassHashValue(detailHash, veinWiggleAmount);
		if (grassMaterialDetailValid[typeIndex] && grassMaterialDetailHashes[typeIndex] == detailHash)
			continue;

		detailData.resize(GrassMaterialDetailDim * GrassMaterialDetailDim * 4u);
		for (uint32_t variant = 0; variant < GrassMaterialDetailVariants; ++variant) {
			const uint32_t offsetHash = Hash2D(typeIndex * GrassMaterialDetailVariants + variant, variant);
			const float noiseOffsetX = static_cast<float>(offsetHash & 0xFFFFu) * (1.0f / 65536.0f);
			const float noiseOffsetY = static_cast<float>(offsetHash >> 16) * (1.0f / 65536.0f);
			const float phase = static_cast<float>(Hash2D(variant, typeIndex) >> 8) * (std::numbers::pi_v<float> * 2.0f / 16777216.0f);

			for (uint32_t y = 0; y < GrassMaterialDetailDim; ++y) {
				const float along = (static_cast<float>(y) + 0.5f) * (1.0f / GrassMaterialDetailDim);

				for (uint32_t x = 0; x < GrassMaterialDetailDim; ++x) {
					const float across = (static_cast<float>(x) + 0.5f) * (1.0f / GrassMaterialDetailDim);

					const float blotch = SampleGrassMaterialDetail(across * 0.125f * blotchScale + noiseOffsetX,
						along * 0.5f * blotchScale + noiseOffsetY, false);
					const float grain = SampleGrassMaterialDetail(across * 6.0f * speckleScale + noiseOffsetX * 1.7f,
						along * 26.0f * speckleScale + noiseOffsetY * 1.7f, true);

					const float centreVein = 1.0f - Smoothstep(0.0f, 0.050f, std::abs(across - 0.5f));
					const float sideVeinL = 1.0f - Smoothstep(0.0f, 0.032f, std::abs(across - 0.27f));
					const float sideVeinR = 1.0f - Smoothstep(0.0f, 0.032f, std::abs(across - 0.73f));
					float vein = std::clamp(centreVein + 0.5f * (sideVeinL + sideVeinR), 0.0f, 1.0f);
					vein *= Smoothstep(0.0f, 0.16f, along) * Smoothstep(0.0f, 0.20f, 1.0f - along);
					vein *= (1.0f - veinRippleDepth) + veinRippleDepth * std::sin(along * 26.0f + phase);

					const float normalOffset = (across - 0.5f) * 2.0f * vein * veinStrength +
					                           std::sin(along * 40.0f + phase) * veinWiggleAmount;

					const uint32_t index = (y * GrassMaterialDetailDim + x) * 4u;
					detailData[index] = encodeUNorm8(blotch);
					detailData[index + 1u] = encodeUNorm8(grain);
					detailData[index + 2u] = encodeUNorm8(vein);
					detailData[index + 3u] = encodeUNorm8(normalOffset * (0.5f / GrassMaterialDetailNormalRange) + 0.5f);
				}
			}

			const uint32_t slice = typeIndex * GrassMaterialDetailVariants + variant;
			globals::d3d::context->UpdateSubresource(grassMaterialDetailTexture->resource.get(), D3D11CalcSubresource(0, slice, 1), nullptr,
				detailData.data(), GrassMaterialDetailDim * 4u, 0);
		}
		grassMaterialDetailHashes[typeIndex] = detailHash;
		grassMaterialDetailValid[typeIndex] = true;
	}
}

void ProceduralGrass::ResolveGrassTypes(const bool prelinearizeTypeColors)
{
	// Slot 0 is bare, slot 1 is base grass, and later slots are texture variants.
	resolvedGrassTypes = {};
	resolvedGeneratorTypes = {};
	resolvedGrassTypes.grassType[1] = ResolveGrassType(nlohmann::json::object());

	for (size_t i = 0; i < typeAllocation.size(); i++) {
		const auto& [key, defIndex] = typeAllocation[i];
		resolvedGrassTypes.grassType[i + 2] = ResolveGrassType(settings.textureTypes[key][defIndex].overrides);
	}

	const auto convertTint = [](float4& tint) {
		globals::features::linearLighting.SRGBToWorking(&tint.x);
	};

	for (uint32_t i = 1; i < ActiveGrassTypeCount(); ++i) {
		auto& type = resolvedGrassTypes.grassType[i];
		type.fuzzDirectionalAlbedoParams = GetFuzzDirectionalAlbedoParameters(type.grassSurfParams.w);
		const auto canopyGeometry = GetDistantCanopyGeometry(type);
		type.grassTypeLightParams.y = canopyGeometry.x;
		type.grassTypeLightParams.z = canopyGeometry.y;

		// Anchor the scattering tint at the same area-weighted colour used to stabilize blade appearance.
		float4 referenceColor(
			std::lerp(type.baseColor.x, type.tipColor.x, 1.0f / 3.0f),
			std::lerp(type.baseColor.y, type.tipColor.y, 1.0f / 3.0f),
			std::lerp(type.baseColor.z, type.tipColor.z, 1.0f / 3.0f), 0.0f);

		if (prelinearizeTypeColors) {
			convertTint(type.grassSubsurfaceColor);
			convertTint(type.grassBounceColor);
			convertTint(referenceColor);
		}

		// Precompute the colour ratio on type updates; the pixel shader only multiplies to retain material detail.
		type.grassSubsurfaceColor.x /= std::max(referenceColor.x, 1e-4f);
		type.grassSubsurfaceColor.y /= std::max(referenceColor.y, 1e-4f);
		type.grassSubsurfaceColor.z /= std::max(referenceColor.z, 1e-4f);
	}

	float maxHeight = 0.0f;
	float maxCurveReach = 0.0f;
	float maxNearWidth = 0.0f;
	float maxFarWidth = 0.0f;
	float maxClumpPull = 0.0f;  // Largest grid size times pull factor, in world units.

	for (uint32_t i = 0; i < MaxGrassTypes; ++i) {
		const auto& source = resolvedGrassTypes.grassType[i];
		// Unused slots stay zeroed; give them a valid grid so a stray lookup cannot divide by zero.
		const float clumpGridSize = std::max(source.clumpGridSize, MinClumpGridSize);
		resolvedGeneratorTypes.grassType[i] = GrassGeneratorType{
			source.height, source.width, source.minSlope, source.maxSlope,
			source.stiffness, source.rotationalStiffness, source.tipWeight, source.mid,
			source.clumpDistanceFactor, source.clumpHeightFactor, source.clumpFacingFactor, source.clumpLeanFactor,
			clumpGridSize, 1.0f / clumpGridSize, source.grassSurfParams.z, 0.0f
		};

		maxHeight = std::max(maxHeight, source.height);
		const float maxBend = 1.85f * source.stiffness;
		const float controlReach = source.height * std::sqrt(source.mid * source.mid + maxBend * maxBend);
		maxCurveReach = std::max(maxCurveReach, std::max(source.height, controlReach));
		maxClumpPull = std::max(maxClumpPull, clumpGridSize * std::abs(source.clumpDistanceFactor));
		const float baseWidth = source.width * 2.5f * 1.3f;
		maxNearWidth = std::max(maxNearWidth, baseWidth * 2.0f);        // Low is the widest near tier.
		maxFarWidth = std::max(maxFarWidth, baseWidth * 32.0f * 2.0f);  // Include Far's maximum coverage compensation.
	}
	// Far grows its blades up to this multiple as it thins with distance.
	terrainCanopyMaxHeight = maxHeight * PGrassCommon::FarMaxHeightScale;

	// Rebuild terrain-darkening maps only when slope limits change; a cleared canopy map refills over many frames.
	uint64_t slopeLimitsHash = GrassHashOffsetBasis;
	for (const auto& type : resolvedGeneratorTypes.grassType) {
		GrassHashValue(slopeLimitsHash, type.minSlope);
		GrassHashValue(slopeLimitsHash, type.maxSlope);
	}
	if (slopeLimitsHash != resolvedSlopeLimitsHash) {
		resolvedSlopeLimitsHash = slopeLimitsHash;
		terrainCanopyNeedsClear = true;
		grassPresenceContentHash = (std::numeric_limits<uint64_t>::max)();
	}

	grassTypesArrayCB->Update(resolvedGrassTypes);
	grassGeneratorTypesCB->Update(resolvedGeneratorTypes);
	UpdateGrassMaterialDetailTexture();

	// View thickening scales with blade width.
	nearQuadrantFrustumPadding = maxClumpPull + maxCurveReach + maxNearWidth * (1.0f + settings.grassViewThicken);
	farQuadrantFrustumPadding = maxHeight * PGrassCommon::FarMaxHeightScale + maxFarWidth;
	lowFadeInPositionPadding = maxClumpPull * 0.1125f + 1.0f;
	hiZClumpReach = maxClumpPull * 0.16f;
	nearHiZRadius = maxCurveReach + maxNearWidth * (1.0f + settings.grassViewThicken) + hiZClumpReach + 1.0f;
	grassTypesDirty = false;
}
