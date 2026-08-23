#include "Features/ProceduralGrass.h"

#include "TopDownOcclusion.h"
#include "TerrainHeightMap.h"
#include "Utils/Serialize.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralGrass::Settings,
	Enabled,
	Quality)


void ProceduralGrass::DrawSettings()
{
	//  Repack the type tables while the feature panel is active so edits remain live, otherwise use cached tables.
	grassTypesDirty = true;

	ImGui::Checkbox("Enabled", &settings.Enabled);

	if (ImGui::Button("Toggle Vanilla Grass Rendering"))
		ConsoleFunc_ToggleGrass();

	ImGui::Separator();

	if (ImGui::Button("1. Press first")) {
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			player->SetPosition({ 40000.74f, 5069.26f, -4330.91f }, true);
			player->SetAngle({ 0, 0, DirectX::XMConvertToRadians(90.0f) });
		}
	}

	if (ImGui::Button("2. Move to benchmark location")) {
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			player->SetPosition({ 36087.74f, 5069.26f, -4330.91f }, true);
			player->SetAngle({ 0, 0, DirectX::XMConvertToRadians(90.0f) });
		}
	}

	ImGui::Separator();

	ImGui::ColorEdit3("Base Color", reinterpret_cast<float*>(&settings.baseColor));
	ImGui::ColorEdit3("Tip Color", reinterpret_cast<float*>(&settings.tipColor));

	if (ImGui::CollapsingHeader("Colour Variation")) {
		ImGui::SliderFloat("Hue Variation", &settings.grassColorHueVariation, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Brightness Variation", &settings.grassColorValueVariation, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Tip Dry Strength", &settings.grassColorTipDryStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Mottle Strength", &settings.grassColorMottleStrength, 0.0f, 0.5f, "%.2f");

		ImGui::SliderFloat3("Cool/Green Tint", reinterpret_cast<float*>(&settings.grassColorCool), 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat3("Warm/Straw Tint", reinterpret_cast<float*>(&settings.grassColorWarm), 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat3("Dried Tip Tint", reinterpret_cast<float*>(&settings.grassColorTipDry), 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Clump Colour Patches", &settings.grassClumpColorStrength, 0.0f, 1.0f, "%.2f");
	}

	if (ImGui::CollapsingHeader("Blade Detail")) {
		ImGui::SliderFloat("Canopy Base Shading", &settings.grassBaseAO, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Micro Detail", &settings.grassMicroDetail, 0.0f, 1.0f, "%.2f");

		// Surface texture. Grain fades out with distance so it cannot alias into shimmer on the far field.
		ImGui::SliderFloat("Blotch Strength", &settings.grassBlotchStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Blotch Scale", &settings.grassBlotchScale, 0.25f, 4.0f, "%.2f");
		ImGui::SliderFloat("Grain Strength", &settings.grassSpeckleStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Grain Scale", &settings.grassSpeckleScale, 0.25f, 4.0f, "%.2f");

		ImGui::SeparatorText("Veins");
		ImGui::SliderFloat3("Vein Tint", reinterpret_cast<float*>(&settings.grassVeinTint), 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Vein Tint Strength", &settings.grassVeinAlbedoStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Vein Normal Strength", &settings.grassVeinNormalStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Vein Ripple Depth", &settings.grassVeinRippleDepth, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Vein Micro-Wiggle", &settings.grassVeinWiggleAmount, 0.0f, 0.25f, "%.3f");
	}

	if (ImGui::CollapsingHeader("Blade Lighting")) {
		// 0 uses each blade's own normal for ambient (noisy), 1 uses straight up (flat but coherent).
		ImGui::SliderFloat("Ambient Normal Flatten", &settings.grassAmbientFlatten, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Canopy Sky Occlusion", &settings.grassCanopySkyOcclusion, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Density Occlusion", &settings.grassDensityAO, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Terminator Wrap", &settings.grassWrap, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Anisotropic Specular", &settings.grassAniso, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Ground Bounce", &settings.grassBounceStrength, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat3("Ground Bounce Tint", reinterpret_cast<float*>(&settings.grassBounceColor), 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Sun Self-Shadow", &settings.grassSunSelfShadow, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Specular Occlusion", &settings.grassSpecOcclusion, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Ambient Desaturation", &settings.grassAmbientDesat, 0.0f, 1.0f, "%.2f");
	}

	if (ImGui::CollapsingHeader("Terrain Blend")) {
		// Blade bases dither-dissolve into the real terrain in the GBuffer, softening the hard base edge.
		ImGui::SliderFloat("Base Dissolve", &settings.grassTerrainBlendStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Dissolve Height (units)", &settings.grassTerrainBlendHeight, 0.0f, 60.0f, "%.1f");
		ImGui::SliderFloat("Base Normal Flatten", &settings.grassTerrainBlendNormal, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Base Roughness", &settings.grassTerrainBlendRough, 0.0f, 1.0f, "%.2f");
	}

	ImGui::Separator();

	ImGui::SliderFloat("Height", &settings.grassHeight, 0.0f, 150.0f, "%.1f");
	ImGui::SliderFloat("Width", &settings.grassWidth, 0.0f, 10.0f, "%.1f");
	ImGui::SliderFloat("View Thicken", &settings.grassViewThicken, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("K1", &settings.stiffness, -10.0f, 10.0f, "%.2f");
	ImGui::SliderFloat("K2", &settings.tipWeight, -10.0f, 10.0f, "%.2f");
	ImGui::SliderFloat("Mid", &settings.mid, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Rotational Stiffness", &settings.rotationalStiffness, 0.0f, 10.0f, "%.2f");

	ImGui::Separator();

	ImGui::SliderFloat("Baked Min AO", &settings.ao, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat2("Subsurface Opacity (Base>Tip)", reinterpret_cast<float*>(&settings.subsurfaceOpacity), 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat3("Subsurface Color", reinterpret_cast<float*>(&settings.grassSubsurfaceTint), 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("Specular", &settings.specular, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat3("Roughness (Base>Min>Tip)", reinterpret_cast<float*>(&settings.baseMinTipRoughness), 0.0f, 1.0f, "%.2f");
	// Kept off 0 and 1 so neither smoothstep in the vertex shader collapses to a zero-width range.
	ImGui::SliderFloat("Roughness Tip Start", &settings.tipRoughnessStart, 0.05f, 0.95f, "%.2f");
	ImGui::SliderFloat("Clump AO Strength", &settings.clumpAOStrength, 0.0f, 1.0f, "%.2f");

	ImGui::Separator();

	ImGui::SliderFloat("Terrain Shadow Strength", &settings.grassAOStrength, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat("Terrain Shadow Density", &settings.grassAODensity, 1.0f, 64.0f, "%.0f");

	ImGui::Separator();

	ImGui::SliderInt("Clump Grid Size", &settings.voronoiGridSize, 1, 4096);
	ImGui::SliderFloat("Clump Distance Factor", &settings.clumpDistanceFactor, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Clump Facing Factor", &settings.clumpFacingFactor, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Clump Height Factor", &settings.clumpHeightFactor, 0.0f, 2.0f, "%.2f");

	ImGui::Separator();

	// Cull is disabled at 90 degrees, lower trims grass off cliffs first.
	ImGui::SliderFloat("Max Slope (deg)", &settings.grassMaxSlope, 0.0f, 90.0f, "%.0f");
	ImGui::SliderFloat("Min Slope (deg)", &settings.grassMinSlope, 0.0f, 90.0f, "%.0f");
	ImGui::SliderFloat("Slope Facing", &settings.grassSlopeFacing, 0.0f, 1.0f, "%.2f");

	ImGui::Separator();

	ImGui::SeparatorText("Wind and Occlusion");
	if (ImGui::SliderAngle("Wind Direction", &settings.windAngle)) {
		windDirection = float2(cos(settings.windAngle), sin(settings.windAngle));
	}

	ImGui::SliderFloat("Wind Speed", &settings.windSpeed, 0.0f, 1.0f);

	ImGui::SliderFloat("Phase Offset", &settings.phaseOffset, 0.0f, 10.0f);
	ImGui::SliderFloat("Phase Lag", &settings.phaseLag, 0.0f, 1.0f);
	ImGui::SliderFloat("Spatial Freq", &settings.spatialFreq, 0.0f, 100.0f);
	ImGui::Separator();

	ImGui::SliderFloat("Occluder Padding (units)", &settings.occlusionPadding, 0.0f, 128.0f, "%.0f");
	ImGui::SliderFloat("Occluder Height Bias (units)", &settings.occlusionBias, 0.0f, 64.0f, "%.1f");
	// Cull grass only where an occluder's underside is within this height of the ground.
	ImGui::SliderFloat("Occlusion Clearance (units)", &settings.occlusionClearance, 0.0f, 512.0f, "%.0f");
	ImGui::Separator();

	ImGui::SeparatorText("LOD Density");
	if (ImGui::SliderInt("Density (High LOD)", &settings.Quality, 0, static_cast<uint8_t>(Quality::Count) - 1, QualityNames[settings.Quality], ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
		grassRendererHighLOD->SetDensity(QualityDensities[settings.Quality]);
	if (ImGui::SliderInt("Density (Mid LOD)", &settings.midGrassDensity, 8, 320, "%d", ImGuiSliderFlags_AlwaysClamp))
		grassRendererMidLOD->SetDensity(static_cast<uint32_t>(settings.midGrassDensity));
	if (ImGui::SliderInt("Density (Low LOD)", &settings.lowGrassDensity, 8, 320, "%d", ImGuiSliderFlags_AlwaysClamp)) {
		grassRendererLowLOD->SetDensity(static_cast<uint32_t>(settings.lowGrassDensity));
		grassRendererFarLOD->SetDensity(FarPatchDensity());
	}

	if (ImGui::SliderInt("Far Grass Radius (cells)", &settings.grassCellRadius, 0, 15, "%d", ImGuiSliderFlags_AlwaysClamp))
		grassRendererFarLOD->SetBladeQuadrantCapacity(FarBladeQuadrantCapacity());
	if (ImGui::SliderInt("Far Grass Density", &settings.farGrassDensity, 8, 160, "%d", ImGuiSliderFlags_AlwaysClamp))
		grassRendererFarLOD->SetDensity(FarPatchDensity());
	ImGui::SliderFloat("Far Edge Density", &settings.farDensityFalloff, 0.0f, 1.0f, "%.2f");

	ImGui::Separator();

	DrawGrassTypeEditor();

	ImGui::Separator();

	if (ImGui::CollapsingHeader("Debug")) {
		bool invalidate = false;
		invalidate |= ImGui::Checkbox("Ignore grass map (LTEX)", &settings.debugIgnoreGrassMap);
		ImGui::Checkbox("Ignore object occlusion", &settings.debugIgnoreObjectOcclusion);
		ImGui::SliderFloat("Grass map edge noise (units)", &settings.grassMapEdgeNoise, 0.0f, 256.0f, "%.0f");
		if (ImGui::SliderFloat("Occlusion half extent", &settings.occlusionHalfExtent, 1024.0f, 16384.0f, "%.0f"))
			TopDownOcclusion::GetSingleton()->SetHalfExtent(settings.occlusionHalfExtent);
		{
			const auto td = TopDownOcclusion::GetSingleton();
			const auto centre = td->GetWindowCentre();
			ImGui::Text("Occlusion map: %s   %u x %u", td->IsReady() ? "ready" : "NOT READY", td->GetMapDim(), td->GetMapDim());
			ImGui::Text("Window centre: %.0f, %.0f   half extent %.0f   %.1f units/texel",
				centre.x, centre.y, td->GetHalfExtent(), td->GetHalfExtent() * 2.0f / td->GetMapDim());
			ImGui::Text("Occluders drawn: %u", td->GetDrawCount());
		}
		ImGui::Checkbox("Disable ALL generator culls", &settings.debugDisableAllCulls);
		ImGui::Checkbox("Ignore preprocessed-node check", &settings.debugIgnorePreProcessedFlag);
		if (invalidate) {
			grassMapCache.clear();
		}

		size_t grassSet = 0;
		size_t grassTotal = 0;
		for (const auto& entry : grassMapCache) {
			grassTotal += entry.second.ids.size();
			for (const auto id : entry.second.ids)
				grassSet += id != 0;
		}

		const auto heightMap = TerrainHeightMap::GetSingleton();
		const auto cached = heightMap->GetCached();
		ImGui::Text("Height map ready: %s", heightMap->IsReady() ? "yes" : "NO");
		ImGui::Text("Height map worldspace: %s", cached ? cached->worldspace.c_str() : "<none>");
		const auto posRange = heightMap->GetPosRange();
		ImGui::Text("Height map range: %.1f .. %.1f", posRange.x, posRange.y);
		if (const auto pc = RE::PlayerCharacter::GetSingleton()) {
			const auto playerPos = pc->GetPosition();
			ImGui::Text("Player Z: %.1f", playerPos.z);

			if (const auto landZ = GetLandHeightAt(playerPos.x, playerPos.y))
				ImGui::Text("LAND Z at player: %.1f  (delta %.1f)", *landZ, *landZ - playerPos.z);
			else
				ImGui::Text("LAND Z at player: <no cached quadrant>");
		}

		ImGui::Text("LAND raw[0]: %.1f   raw min: %.1f", landHeightDebug.rawFirst, landHeightDebug.rawMin);
		ImGui::Text("LAND heightExtents: %.1f .. %.1f", landHeightDebug.extents.x, landHeightDebug.extents.y);
		ImGui::Text("LAND anchor applied: %.1f   mesh world Z: %.1f", landHeightDebug.anchor, landHeightDebug.meshWorldZ);

		ImGui::Separator();

		ImGui::Text("Blades generated  high: %u  mid: %u  low: %u  far: %u",
			grassRendererHighLOD->ReadBladeCount(),
			grassRendererMidLOD->ReadBladeCount(),
			grassRendererLowLOD->ReadBladeCount(),
			grassRendererFarLOD->ReadBladeCount());
		ImGui::Text("Quadrants  high: %zu  mid: %zu  low: %zu  far: %zu",
			quadrantsHighLOD.size(), quadrantsMidLOD.size(), quadrantsLowLOD.size(), quadrantsFarLOD.size());
		ImGui::Text("cells %u -> exterior %u -> land %u -> loadedData %u -> mesh %u -> preProcessed %u",
			quadrantReject.cells, quadrantReject.withExterior, quadrantReject.withLand,
			quadrantReject.withLoadedData, quadrantReject.withMesh, quadrantReject.preProcessed);

		ImGui::Separator();

		ImGui::Text("Grass quadrants cached: %zu", grassMapCache.size());
		ImGui::Text("Samples growing grass: %zu / %zu (%.1f%%)", grassSet, grassTotal, grassTotal ? 100.0 * grassSet / grassTotal : 0.0);
	}
}

void ProceduralGrass::DrawGrassTypeEditor()
{
	if (!ImGui::CollapsingHeader("Grass Types"))
		return;

	ImGui::TextWrapped(
		"Grass types are per landscape texture. Expand a texture and add one or more type variants; each overrides "
		"only the fields you tick (unticked fields inherit the base settings above) and carries a weight. A texture's "
		"blades are split between its variants in proportion to their weights. A texture with no variants grows the "
		"base type. Total variants across all textures: %zu / %u.",
		typeAllocation.size(), PGrassCommon::MaxGrassTypes - 2);

	const auto& s = settings;

	const auto fFloat = [](nlohmann::json& ov, const char* key, const char* label, float base, float mn, float mx, const char* fmt = "%.2f") {
		bool has = ov.contains(key);
		if (ImGui::Checkbox(std::format("##en_{}", key).c_str(), &has)) {
			if (has)
				ov[key] = base;
			else
				ov.erase(key);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!has);
		float val = has ? ov[key].get<float>() : base;
		if (ImGui::SliderFloat(label, &val, mn, mx, fmt) && has)
			ov[key] = val;
		ImGui::EndDisabled();
	};

	const auto fFloat2 = [](nlohmann::json& ov, const char* key, const char* label, float2 base, float mn, float mx) {
		bool has = ov.contains(key);
		if (ImGui::Checkbox(std::format("##en_{}", key).c_str(), &has)) {
			if (has)
				ov[key] = base;
			else
				ov.erase(key);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!has);
		float2 val = has ? ov[key].get<float2>() : base;
		if (ImGui::SliderFloat2(label, &val.x, mn, mx, "%.2f") && has)
			ov[key] = val;
		ImGui::EndDisabled();
	};

	const auto fFloat3 = [](nlohmann::json& ov, const char* key, const char* label, float3 base, float mn, float mx, bool asColor) {
		bool has = ov.contains(key);
		if (ImGui::Checkbox(std::format("##en_{}", key).c_str(), &has)) {
			if (has)
				ov[key] = base;
			else
				ov.erase(key);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!has);
		float3 val = has ? ov[key].get<float3>() : base;
		bool changed = asColor ? ImGui::ColorEdit3(label, &val.x) : ImGui::SliderFloat3(label, &val.x, mn, mx, "%.2f");
		if (changed && has)
			ov[key] = val;
		ImGui::EndDisabled();
	};

	const auto renderOverrides = [&](nlohmann::json& ov) {
		ImGui::SeparatorText("Shape");
		fFloat(ov, "Height", "Height", s.grassHeight, 0.0f, 150.0f, "%.1f");
		fFloat(ov, "Width", "Width", s.grassWidth, 0.0f, 10.0f, "%.1f");
		fFloat(ov, "Stiffness", "K1", s.stiffness, -10.0f, 10.0f);
		fFloat(ov, "TipWeight", "K2", s.tipWeight, -10.0f, 10.0f);
		fFloat(ov, "Mid", "Mid", s.mid, 0.0f, 1.0f);
		fFloat(ov, "RotationalStiffness", "Rotational Stiffness", s.rotationalStiffness, 0.0f, 10.0f);

		ImGui::SeparatorText("Slope");
		fFloat(ov, "MinSlope", "Min Slope (deg)", s.grassMinSlope, 0.0f, 90.0f, "%.0f");
		fFloat(ov, "MaxSlope", "Max Slope (deg)", s.grassMaxSlope, 0.0f, 90.0f, "%.0f");

		ImGui::SeparatorText("Clump");
		fFloat(ov, "ClumpDistanceFactor", "Clump Distance", s.clumpDistanceFactor, 0.0f, 1.0f);
		fFloat(ov, "ClumpFacingFactor", "Clump Facing", s.clumpFacingFactor, 0.0f, 1.0f);
		fFloat(ov, "ClumpHeightFactor", "Clump Height", s.clumpHeightFactor, 0.0f, 2.0f);
		fFloat(ov, "ClumpAOStrength", "Clump AO", s.clumpAOStrength, 0.0f, 1.0f);
		fFloat(ov, "ClumpColorStrength", "Clump Colour", s.grassClumpColorStrength, 0.0f, 1.0f);

		ImGui::SeparatorText("Colour");
		fFloat3(ov, "BaseColor", "Base Color", s.baseColor, 0.0f, 1.0f, true);
		fFloat3(ov, "TipColor", "Tip Color", s.tipColor, 0.0f, 1.0f, true);
		fFloat3(ov, "ColorTipDry", "Dried Tip Tint", s.grassColorTipDry, 0.0f, 2.0f, false);
		fFloat3(ov, "ColorCool", "Cool/Green Tint", s.grassColorCool, 0.0f, 2.0f, false);
		fFloat3(ov, "ColorWarm", "Warm/Straw Tint", s.grassColorWarm, 0.0f, 2.0f, false);
		fFloat(ov, "HueVariation", "Hue Variation", s.grassColorHueVariation, 0.0f, 1.0f);
		fFloat(ov, "ValueVariation", "Brightness Variation", s.grassColorValueVariation, 0.0f, 1.0f);
		fFloat(ov, "TipDryStrength", "Tip Dry Strength", s.grassColorTipDryStrength, 0.0f, 1.0f);
		fFloat(ov, "MottleStrength", "Mottle Strength", s.grassColorMottleStrength, 0.0f, 0.5f);

		ImGui::SeparatorText("Lighting");
		fFloat(ov, "MinAO", "Baked Min AO", s.ao, 0.0f, 1.0f);
		fFloat(ov, "Specular", "Specular", s.specular, 0.0f, 1.0f);
		fFloat2(ov, "SubsurfaceOpacity", "Subsurface (Base>Tip)", s.subsurfaceOpacity, 0.0f, 1.0f);
		fFloat3(ov, "SubsurfaceTint", "Subsurface Color", s.grassSubsurfaceTint, 0.0f, 2.0f, false);
		fFloat3(ov, "BaseMinTipRoughness", "Roughness (Base>Min>Tip)", s.baseMinTipRoughness, 0.0f, 1.0f, false);
		fFloat(ov, "TipRoughnessStart", "Roughness Tip Start", s.tipRoughnessStart, 0.05f, 0.95f);
		fFloat(ov, "MicroDetail", "Micro Detail", s.grassMicroDetail, 0.0f, 1.0f);
		fFloat(ov, "AmbientFlatten", "Ambient Flatten", s.grassAmbientFlatten, 0.0f, 1.0f);
		fFloat(ov, "Wrap", "Terminator Wrap", s.grassWrap, 0.0f, 1.0f);
		fFloat(ov, "Aniso", "Anisotropic Specular", s.grassAniso, 0.0f, 2.0f);
		fFloat(ov, "BounceStrength", "Ground Bounce", s.grassBounceStrength, 0.0f, 2.0f);
		fFloat3(ov, "BounceColor", "Ground Bounce Tint", s.grassBounceColor, 0.0f, 2.0f, false);
		fFloat(ov, "SpecOcclusion", "Specular Occlusion", s.grassSpecOcclusion, 0.0f, 1.0f);
		fFloat(ov, "AmbientDesat", "Ambient Desaturation", s.grassAmbientDesat, 0.0f, 1.0f);

		ImGui::SeparatorText("Surface & Wind");
		fFloat(ov, "BlotchStrength", "Blotch Strength", s.grassBlotchStrength, 0.0f, 1.0f);
		fFloat(ov, "BlotchScale", "Blotch Scale", s.grassBlotchScale, 0.25f, 4.0f);
		fFloat(ov, "SpeckleStrength", "Grain Strength", s.grassSpeckleStrength, 0.0f, 1.0f);
		fFloat(ov, "SpeckleScale", "Grain Scale", s.grassSpeckleScale, 0.25f, 4.0f);
		fFloat(ov, "SpatialFreq", "Spatial Freq", s.spatialFreq, 0.0f, 100.0f);
		fFloat(ov, "PhaseOffset", "Phase Offset", s.phaseOffset, 0.0f, 10.0f);
		fFloat(ov, "PhaseLag", "Phase Lag", s.phaseLag, 0.0f, 1.0f);

		ImGui::SeparatorText("Veins");
		fFloat3(ov, "VeinTint", "Vein Tint", s.grassVeinTint, 0.0f, 2.0f, false);
		fFloat(ov, "VeinAlbedoStrength", "Vein Tint Strength", s.grassVeinAlbedoStrength, 0.0f, 1.0f);
		fFloat(ov, "VeinNormalStrength", "Vein Normal Strength", s.grassVeinNormalStrength, 0.0f, 1.0f);
		fFloat(ov, "VeinRippleDepth", "Vein Ripple Depth", s.grassVeinRippleDepth, 0.0f, 1.0f);
		fFloat(ov, "VeinWiggleAmount", "Vein Micro-Wiggle", s.grassVeinWiggleAmount, 0.0f, 0.25f);

		ImGui::Spacing();
		if (ImGui::Button("Clear all overrides"))
			ov = nlohmann::json::object();
	};

	static char filter[128] = "";
	ImGui::InputTextWithHint("Filter", "editor id / plugin", filter, sizeof(filter));
	static bool onlyGrass = true;
	ImGui::SameLine();
	ImGui::Checkbox("Only grass-growing", &onlyGrass);

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler) {
		ImGui::TextDisabled("Data handler unavailable (not in a loaded game).");
		return;
	}

	const auto matchesFilter = [](std::string_view haystack, const char* needle) {
		if (!needle[0])
			return true;
		const auto lower = [](std::string v) { std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); }); return v; };
		return lower(std::string(haystack)).find(lower(needle)) != std::string::npos;
	};

	bool typesChanged = false;

	if (ImGui::BeginChild("LandTextureList", ImVec2(0, 360), ImGuiChildFlags_Borders)) {
		for (auto* ltex : dataHandler->GetFormArray<RE::TESLandTexture>()) {
			if (!ltex)
				continue;
			const bool grows = !ltex->textureGrassList.empty();
			if (onlyGrass && !grows)
				continue;

			const char* edid = ltex->GetFormEditorID();
			const std::string name = (edid && edid[0]) ? edid : "<no editor id>";
			const std::string key = LandTextureKey(ltex);
			if (!matchesFilter(name, filter) && !matchesFilter(key, filter))
				continue;

			auto texIt = settings.textureTypes.find(key);
			const size_t count = texIt != settings.textureTypes.end() ? texIt->second.size() : 0;

			if (!ImGui::TreeNode(key.c_str(), "%s   %s[%zu type%s]", name.c_str(), grows ? "" : "(no grass) ", count, count == 1 ? "" : "s"))
				continue;

			ImGui::TextDisabled("%s", key.c_str());

			auto& defs = settings.textureTypes[key]; 
			float totalWeight = 0.0f;
			for (const auto& d : defs)
				totalWeight += std::max(0.0f, d.weight);

			int removeIndex = -1;
			for (uint32_t i = 0; i < defs.size(); i++) {
				ImGui::PushID(static_cast<int>(i));
				auto& def = defs[i];

				const float pct = totalWeight > 0.0f ? 100.0f * std::max(0.0f, def.weight) / totalWeight : 0.0f;
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Variant %u", i + 1);
				ImGui::SameLine();
				ImGui::PushItemWidth(90.0f);
				if (ImGui::DragFloat("Weight", &def.weight, 0.1f, 0.0f, 100.0f, "%.1f"))
					typesChanged = true;
				ImGui::PopItemWidth();
				ImGui::SameLine();
				ImGui::TextDisabled("(%.0f%%)", pct);
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove"))
					removeIndex = static_cast<int>(i);

				if (ImGui::TreeNode("Overrides", "Overrides (%zu set)", def.overrides.is_object() ? def.overrides.size() : 0)) {
					renderOverrides(def.overrides);
					ImGui::TreePop();
				}

				ImGui::PopID();
				ImGui::Separator();
			}

			if (removeIndex >= 0) {
				defs.erase(defs.begin() + removeIndex);
				typesChanged = true;
			}

			if (typeAllocation.size() + 2 < PGrassCommon::MaxGrassTypes) {
				if (ImGui::SmallButton("Add variant")) {
					defs.push_back({});
					typesChanged = true;
				}
			} else {
				ImGui::TextDisabled("Type pool full (%u).", PGrassCommon::MaxGrassTypes - 2);
			}

			if (defs.empty())
				settings.textureTypes.erase(key);  // don't persist textures the user opened but left empty

			ImGui::TreePop();
		}
	}
	ImGui::EndChild();

	if (typesChanged) {
		RebuildTypeAllocation();
		grassContentGeneration++;
		grassMapCache.clear();
	}
}

void ProceduralGrass::LoadSettings(json& o_json)
{
	settings = o_json;

	// Blade shape / material
	settings.grassHeight = o_json.value("Height", settings.grassHeight);
	settings.grassWidth = o_json.value("Width", settings.grassWidth);
	settings.stiffness = o_json.value("Stiffness", settings.stiffness);
	settings.tipWeight = o_json.value("TipWeight", settings.tipWeight);
	settings.mid = o_json.value("Mid", settings.mid);
	settings.rotationalStiffness = o_json.value("RotationalStiffness", settings.rotationalStiffness);
	settings.ao = o_json.value("BakedMinAO", settings.ao);
	settings.specular = o_json.value("Specular", settings.specular);
	settings.subsurfaceOpacity = o_json.value("SubsurfaceOpacity", settings.subsurfaceOpacity);
	settings.grassSubsurfaceTint = o_json.value("SubsurfaceTint", settings.grassSubsurfaceTint);
	settings.baseMinTipRoughness = o_json.value("Roughness", settings.baseMinTipRoughness);
	settings.tipRoughnessStart = o_json.value("RoughnessTipStart", settings.tipRoughnessStart);
	settings.clumpAOStrength = o_json.value("ClumpAOStrength", settings.clumpAOStrength);

	// Colour
	settings.baseColor = o_json.value("BaseColor", settings.baseColor);
	settings.tipColor = o_json.value("TipColor", settings.tipColor);
	settings.grassColorHueVariation = o_json.value("ColorHueVariation", settings.grassColorHueVariation);
	settings.grassColorValueVariation = o_json.value("ColorValueVariation", settings.grassColorValueVariation);
	settings.grassColorTipDryStrength = o_json.value("ColorTipDryStrength", settings.grassColorTipDryStrength);
	settings.grassColorMottleStrength = o_json.value("ColorMottleStrength", settings.grassColorMottleStrength);
	settings.grassColorCool = o_json.value("ColorCool", settings.grassColorCool);
	settings.grassColorWarm = o_json.value("ColorWarm", settings.grassColorWarm);
	settings.grassColorTipDry = o_json.value("ColorTipDry", settings.grassColorTipDry);

	// Detail / lighting
	settings.grassBaseAO = o_json.value("BaseAO", settings.grassBaseAO);
	settings.grassClumpColorStrength = o_json.value("ClumpColorStrength", settings.grassClumpColorStrength);
	settings.grassMicroDetail = o_json.value("MicroDetail", settings.grassMicroDetail);
	settings.grassAmbientFlatten = o_json.value("AmbientFlatten", settings.grassAmbientFlatten);
	settings.grassCanopySkyOcclusion = o_json.value("CanopySkyOcclusion", settings.grassCanopySkyOcclusion);
	settings.grassDensityAO = o_json.value("DensityAO", settings.grassDensityAO);
	settings.grassWrap = o_json.value("Wrap", settings.grassWrap);
	settings.grassAniso = o_json.value("Aniso", settings.grassAniso);
	settings.grassBounceStrength = o_json.value("BounceStrength", settings.grassBounceStrength);
	settings.grassBounceColor = o_json.value("BounceColor", settings.grassBounceColor);
	settings.grassSunSelfShadow = o_json.value("SunSelfShadow", settings.grassSunSelfShadow);
	settings.grassSpecOcclusion = o_json.value("SpecOcclusion", settings.grassSpecOcclusion);
	settings.grassAmbientDesat = o_json.value("AmbientDesat", settings.grassAmbientDesat);

	// Surface texture
	settings.grassBlotchStrength = o_json.value("BlotchStrength", settings.grassBlotchStrength);
	settings.grassBlotchScale = o_json.value("BlotchScale", settings.grassBlotchScale);
	settings.grassSpeckleStrength = o_json.value("SpeckleStrength", settings.grassSpeckleStrength);
	settings.grassSpeckleScale = o_json.value("SpeckleScale", settings.grassSpeckleScale);

	// Vein detail
	settings.grassVeinTint = o_json.value("VeinTint", settings.grassVeinTint);
	settings.grassVeinAlbedoStrength = o_json.value("VeinAlbedoStrength", settings.grassVeinAlbedoStrength);
	settings.grassVeinNormalStrength = o_json.value("VeinNormalStrength", settings.grassVeinNormalStrength);
	settings.grassVeinRippleDepth = o_json.value("VeinRippleDepth", settings.grassVeinRippleDepth);
	settings.grassVeinWiggleAmount = o_json.value("VeinWiggleAmount", settings.grassVeinWiggleAmount);

	// Terrain blend / shadow
	settings.grassTerrainBlendStrength = o_json.value("TerrainBlendStrength", settings.grassTerrainBlendStrength);
	settings.grassTerrainBlendHeight = o_json.value("TerrainBlendHeight", settings.grassTerrainBlendHeight);
	settings.grassTerrainBlendNormal = o_json.value("TerrainBlendNormal", settings.grassTerrainBlendNormal);
	settings.grassTerrainBlendRough = o_json.value("TerrainBlendRough", settings.grassTerrainBlendRough);
	settings.grassAOStrength = o_json.value("TerrainShadowStrength", settings.grassAOStrength);
	settings.grassAODensity = o_json.value("TerrainShadowDensity", settings.grassAODensity);

	// Clump
	settings.voronoiGridSize = o_json.value("ClumpGridSize", settings.voronoiGridSize);
	settings.clumpDistanceFactor = o_json.value("ClumpDistanceFactor", settings.clumpDistanceFactor);
	settings.clumpFacingFactor = o_json.value("ClumpFacingFactor", settings.clumpFacingFactor);
	settings.clumpHeightFactor = o_json.value("ClumpHeightFactor", settings.clumpHeightFactor);

	// Slope
	settings.grassMinSlope = o_json.value("MinSlope", settings.grassMinSlope);
	settings.grassMaxSlope = o_json.value("MaxSlope", settings.grassMaxSlope);
	settings.grassSlopeFacing = o_json.value("SlopeFacing", settings.grassSlopeFacing);

	// Wind / animation
	settings.windAngle = o_json.value("WindAngle", settings.windAngle);
	settings.windSpeed = o_json.value("WindSpeed", settings.windSpeed);
	settings.spatialFreq = o_json.value("SpatialFreq", settings.spatialFreq);
	settings.phaseOffset = o_json.value("PhaseOffset", settings.phaseOffset);
	settings.phaseLag = o_json.value("PhaseLag", settings.phaseLag);
	windDirection = float2(std::cos(settings.windAngle), std::sin(settings.windAngle));

	// Occlusion / misc
	settings.occlusionClearance = o_json.value("OcclusionClearance", settings.occlusionClearance);
	settings.occlusionHalfExtent = o_json.value("OcclusionHalfExtent", settings.occlusionHalfExtent);
	settings.occlusionPadding = o_json.value("OcclusionPadding", settings.occlusionPadding);
	settings.occlusionBias = o_json.value("OcclusionBias", settings.occlusionBias);
	settings.grassMapEdgeNoise = o_json.value("GrassMapEdgeNoise", settings.grassMapEdgeNoise);
	settings.grassViewThicken = o_json.value("ViewThicken", settings.grassViewThicken);

	// Per-LOD densities and far-tier controls (existing keys kept for backward compatibility)
	settings.midGrassDensity = o_json.value("MidDensity", settings.midGrassDensity);
	settings.lowGrassDensity = o_json.value("LowDensity", settings.lowGrassDensity);
	settings.farGrassDensity = o_json.value("FarDensity", settings.farGrassDensity);
	settings.grassCellRadius = o_json.value("FarRadius", settings.grassCellRadius);
	settings.farDensityFalloff = o_json.value("FarEdgeDensity", settings.farDensityFalloff);

	// Debug
	settings.debugIgnoreGrassMap = o_json.value("DebugIgnoreGrassMap", settings.debugIgnoreGrassMap);
	settings.debugIgnoreObjectOcclusion = o_json.value("DebugIgnoreObjectOcclusion", settings.debugIgnoreObjectOcclusion);
	settings.debugDisableAllCulls = o_json.value("DebugDisableAllCulls", settings.debugDisableAllCulls);
	settings.debugIgnorePreProcessedFlag = o_json.value("DebugIgnorePreProcessedFlag", settings.debugIgnorePreProcessedFlag);

	// Per-texture grass types, keyed by "plugin|0xLOCALID". Each entry is an array of { Weight, Overrides }.
	if (auto it = o_json.find("TextureTypes"); it != o_json.end() && it->is_object()) {
		settings.textureTypes.clear();
		for (const auto& [key, variants] : it->items()) {
			std::vector<Settings::GrassTypeDef> defs;
			for (const auto& variant : variants) {
				Settings::GrassTypeDef def;
				def.weight = variant.value("Weight", 1.0f);
				if (auto ov = variant.find("Overrides"); ov != variant.end() && ov->is_object())
					def.overrides = *ov;
				defs.push_back(std::move(def));
			}
			settings.textureTypes[key] = std::move(defs);
		}
	}

	RebuildTypeAllocation();

	if (grassRendererHighLOD)
		grassRendererHighLOD->SetDensity(QualityDensities[settings.Quality]);
	if (grassRendererMidLOD)
		grassRendererMidLOD->SetDensity(static_cast<uint32_t>(settings.midGrassDensity));
	if (grassRendererLowLOD)
		grassRendererLowLOD->SetDensity(static_cast<uint32_t>(settings.lowGrassDensity));
	if (grassRendererFarLOD) {
		grassRendererFarLOD->SetBladeQuadrantCapacity(FarBladeQuadrantCapacity());
		grassRendererFarLOD->SetDensity(FarPatchDensity());
	}

	TopDownOcclusion::GetSingleton()->SetHalfExtent(settings.occlusionHalfExtent);
}

void ProceduralGrass::SaveSettings(json& o_json)
{
	o_json = settings;

	// Per-texture grass types, keyed by "plugin|0xLOCALID"; each variant carries its weight + sparse overrides.
	nlohmann::json textureTypesJson = nlohmann::json::object();
	for (const auto& [key, defs] : settings.textureTypes) {
		nlohmann::json variants = nlohmann::json::array();
		for (const auto& def : defs)
			variants.push_back({ { "Weight", def.weight }, { "Overrides", def.overrides } });
		textureTypesJson[key] = std::move(variants);
	}
	o_json["TextureTypes"] = std::move(textureTypesJson);

	// Blade shape / material
	o_json["Height"] = settings.grassHeight;
	o_json["Width"] = settings.grassWidth;
	o_json["Stiffness"] = settings.stiffness;
	o_json["TipWeight"] = settings.tipWeight;
	o_json["Mid"] = settings.mid;
	o_json["RotationalStiffness"] = settings.rotationalStiffness;
	o_json["BakedMinAO"] = settings.ao;
	o_json["Specular"] = settings.specular;
	o_json["SubsurfaceOpacity"] = settings.subsurfaceOpacity;
	o_json["SubsurfaceTint"] = settings.grassSubsurfaceTint;
	o_json["Roughness"] = settings.baseMinTipRoughness;
	o_json["RoughnessTipStart"] = settings.tipRoughnessStart;
	o_json["ClumpAOStrength"] = settings.clumpAOStrength;

	// Colour
	o_json["BaseColor"] = settings.baseColor;
	o_json["TipColor"] = settings.tipColor;
	o_json["ColorHueVariation"] = settings.grassColorHueVariation;
	o_json["ColorValueVariation"] = settings.grassColorValueVariation;
	o_json["ColorTipDryStrength"] = settings.grassColorTipDryStrength;
	o_json["ColorMottleStrength"] = settings.grassColorMottleStrength;
	o_json["ColorCool"] = settings.grassColorCool;
	o_json["ColorWarm"] = settings.grassColorWarm;
	o_json["ColorTipDry"] = settings.grassColorTipDry;

	// Detail / lighting
	o_json["BaseAO"] = settings.grassBaseAO;
	o_json["ClumpColorStrength"] = settings.grassClumpColorStrength;
	o_json["MicroDetail"] = settings.grassMicroDetail;
	o_json["AmbientFlatten"] = settings.grassAmbientFlatten;
	o_json["CanopySkyOcclusion"] = settings.grassCanopySkyOcclusion;
	o_json["DensityAO"] = settings.grassDensityAO;
	o_json["Wrap"] = settings.grassWrap;
	o_json["Aniso"] = settings.grassAniso;
	o_json["BounceStrength"] = settings.grassBounceStrength;
	o_json["BounceColor"] = settings.grassBounceColor;
	o_json["SunSelfShadow"] = settings.grassSunSelfShadow;
	o_json["SpecOcclusion"] = settings.grassSpecOcclusion;
	o_json["AmbientDesat"] = settings.grassAmbientDesat;

	// Surface texture
	o_json["BlotchStrength"] = settings.grassBlotchStrength;
	o_json["BlotchScale"] = settings.grassBlotchScale;
	o_json["SpeckleStrength"] = settings.grassSpeckleStrength;
	o_json["SpeckleScale"] = settings.grassSpeckleScale;

	// Vein detail
	o_json["VeinTint"] = settings.grassVeinTint;
	o_json["VeinAlbedoStrength"] = settings.grassVeinAlbedoStrength;
	o_json["VeinNormalStrength"] = settings.grassVeinNormalStrength;
	o_json["VeinRippleDepth"] = settings.grassVeinRippleDepth;
	o_json["VeinWiggleAmount"] = settings.grassVeinWiggleAmount;

	// Terrain blend / shadow
	o_json["TerrainBlendStrength"] = settings.grassTerrainBlendStrength;
	o_json["TerrainBlendHeight"] = settings.grassTerrainBlendHeight;
	o_json["TerrainBlendNormal"] = settings.grassTerrainBlendNormal;
	o_json["TerrainBlendRough"] = settings.grassTerrainBlendRough;
	o_json["TerrainShadowStrength"] = settings.grassAOStrength;
	o_json["TerrainShadowDensity"] = settings.grassAODensity;

	// Clump
	o_json["ClumpGridSize"] = settings.voronoiGridSize;
	o_json["ClumpDistanceFactor"] = settings.clumpDistanceFactor;
	o_json["ClumpFacingFactor"] = settings.clumpFacingFactor;
	o_json["ClumpHeightFactor"] = settings.clumpHeightFactor;

	// Slope
	o_json["MinSlope"] = settings.grassMinSlope;
	o_json["MaxSlope"] = settings.grassMaxSlope;
	o_json["SlopeFacing"] = settings.grassSlopeFacing;

	// Wind / animation
	o_json["WindAngle"] = settings.windAngle;
	o_json["WindSpeed"] = settings.windSpeed;
	o_json["SpatialFreq"] = settings.spatialFreq;
	o_json["PhaseOffset"] = settings.phaseOffset;
	o_json["PhaseLag"] = settings.phaseLag;

	// Occlusion / misc
	o_json["OcclusionClearance"] = settings.occlusionClearance;
	o_json["OcclusionHalfExtent"] = settings.occlusionHalfExtent;
	o_json["OcclusionPadding"] = settings.occlusionPadding;
	o_json["OcclusionBias"] = settings.occlusionBias;
	o_json["GrassMapEdgeNoise"] = settings.grassMapEdgeNoise;
	o_json["ViewThicken"] = settings.grassViewThicken;

	// Per-LOD densities and far-tier controls
	o_json["MidDensity"] = settings.midGrassDensity;
	o_json["LowDensity"] = settings.lowGrassDensity;
	o_json["FarDensity"] = settings.farGrassDensity;
	o_json["FarRadius"] = settings.grassCellRadius;
	o_json["FarEdgeDensity"] = settings.farDensityFalloff;

	// Debug
	o_json["DebugIgnoreGrassMap"] = settings.debugIgnoreGrassMap;
	o_json["DebugIgnoreObjectOcclusion"] = settings.debugIgnoreObjectOcclusion;
	o_json["DebugDisableAllCulls"] = settings.debugDisableAllCulls;
	o_json["DebugIgnorePreProcessedFlag"] = settings.debugIgnorePreProcessedFlag;
}

void ProceduralGrass::RestoreDefaultSettings()
{
	settings = {};
	RebuildTypeAllocation(); 
	grassMapCache.clear();
	grassContentGeneration++;

	windDirection = float2(std::cos(settings.windAngle), std::sin(settings.windAngle));
	if (grassRendererMidLOD)
		grassRendererMidLOD->SetDensity(static_cast<uint32_t>(settings.midGrassDensity));
	if (grassRendererLowLOD)
		grassRendererLowLOD->SetDensity(static_cast<uint32_t>(settings.lowGrassDensity));
	if (grassRendererFarLOD) {
		grassRendererFarLOD->SetBladeQuadrantCapacity(FarBladeQuadrantCapacity());
		grassRendererFarLOD->SetDensity(FarPatchDensity());
	}
	if (grassRendererHighLOD)
		grassRendererHighLOD->SetDensity(QualityDensities[settings.Quality]);
	TopDownOcclusion::GetSingleton()->SetHalfExtent(settings.occlusionHalfExtent);
}
