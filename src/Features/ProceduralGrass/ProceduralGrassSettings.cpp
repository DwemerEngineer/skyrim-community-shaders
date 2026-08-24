#include "Features/ProceduralGrass.h"

#include "TopDownOcclusion.h"
#include "TerrainHeightMap.h"
#include "Utils/FileSystem.h"
#include "Utils/Serialize.h"

#include <filesystem>
#include <fstream>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralGrass::Settings,
	Enabled,
	Quality)

namespace
{
constexpr auto TextureTypesFilename = "ProceduralGrassTypes.json";

std::filesystem::path TextureTypesPath()
{
	return Util::PathHelpers::GetCommunityShaderPath() / TextureTypesFilename;
}

bool IsNumericArray(const nlohmann::json& value, const size_t size)
{
	if (!value.is_array() || value.size() != size)
		return false;
	return std::all_of(value.begin(), value.end(), [](const auto& component) { return component.is_number(); });
}

bool IsValidGrassTypeOverride(const std::string_view key, const nlohmann::json& value)
{
	if (key == "SubsurfaceOpacity")
		return IsNumericArray(value, 2);

	constexpr std::array float3Keys{
		"BaseColor"sv,
		"TipColor"sv,
		"ColorTipDry"sv,
		"ColorCool"sv,
		"ColorWarm"sv,
		"SubsurfaceTint"sv,
		"BaseMinTipRoughness"sv,
		"BounceColor"sv,
		"VeinTint"sv,
	};
	if (std::ranges::find(float3Keys, key) != float3Keys.end())
		return IsNumericArray(value, 3);

	return value.is_number();
}

void RemoveInvalidGrassTypeOverrides(nlohmann::json& overrides)
{
	for (auto it = overrides.begin(); it != overrides.end();) {
		if (!IsValidGrassTypeOverride(it.key(), it.value()))
			it = overrides.erase(it);
		else
			++it;
	}
}

void DrawSettingDescription(const char* description)
{
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(description);
}
}

void ProceduralGrass::LoadTextureTypes()
{
	settings.textureTypes.clear();

	const auto path = TextureTypesPath();
	std::ifstream input(path);
	if (!input.is_open()) {
		if (std::filesystem::exists(path))
			logger::warn("[Procedural Grass] Failed to open texture types file: {}", path.string());
		return;
	}

	try {
		json textureTypesJson;
		input >> textureTypesJson;
		if (!textureTypesJson.is_object()) {
			logger::warn("[Procedural Grass] Texture types file must contain an object: {}", path.string());
			return;
		}

		for (const auto& [key, variants] : textureTypesJson.items()) {
			if (!variants.is_array()) {
				logger::warn("[Procedural Grass] Ignoring invalid texture type variants for {}", key);
				continue;
			}

			std::vector<Settings::GrassTypeDef> defs;
			for (const auto& variant : variants) {
				if (!variant.is_object())
					continue;

				Settings::GrassTypeDef def;
				if (auto weight = variant.find("Weight"); weight != variant.end() && weight->is_number())
					def.weight = weight->get<float>();
				if (auto noGrass = variant.find("NoGrass"); noGrass != variant.end() && noGrass->is_boolean())
					def.noGrass = noGrass->get<bool>();
				if (auto overrides = variant.find("Overrides"); overrides != variant.end() && overrides->is_object()) {
					def.overrides = *overrides;
					RemoveInvalidGrassTypeOverrides(def.overrides);
				}
				defs.push_back(std::move(def));
			}
			if (!defs.empty())
				settings.textureTypes[key] = std::move(defs);
		}
	} catch (const nlohmann::json::exception& e) {
		logger::warn("[Procedural Grass] Failed to parse texture types file {}: {}", path.string(), e.what());
	}
}

void ProceduralGrass::SaveTextureTypes() const
{
	const auto path = TextureTypesPath();
	try {
		std::filesystem::create_directories(path.parent_path());
	} catch (const std::filesystem::filesystem_error& e) {
		logger::warn("[Procedural Grass] Failed to create texture types directory {}: {}", path.parent_path().string(), e.what());
		return;
	}

	std::ofstream output(path);
	if (!output.is_open()) {
		logger::warn("[Procedural Grass] Failed to open texture types file for saving: {}", path.string());
		return;
	}

	json textureTypesJson = json::object();
	for (const auto& [key, defs] : settings.textureTypes) {
		json variants = json::array();
		for (const auto& def : defs) {
			json variant{ { "Weight", def.weight }, { "Overrides", def.overrides } };
			if (def.noGrass)
				variant["NoGrass"] = true;
			variants.push_back(std::move(variant));
		}
		textureTypesJson[key] = std::move(variants);
	}

	try {
		output << textureTypesJson.dump(1);
	} catch (const nlohmann::json::exception& e) {
		logger::warn("[Procedural Grass] Failed to save texture types file {}: {}", path.string(), e.what());
	}
}


void ProceduralGrass::DrawSettings()
{
	//  Repack the type tables while the feature panel is active so edits remain live, otherwise use cached tables.
	grassTypesDirty = true;

	ImGui::Checkbox(T("feature.procedural_grass.enabled", "Enabled"), &settings.Enabled);
	DrawSettingDescription(T("feature.procedural_grass.enabled_tooltip", "Enables procedural grass rendering."));

	if (ImGui::Button(T("feature.procedural_grass.toggle_vanilla_grass", "Toggle Vanilla Grass Rendering")))
		ConsoleFunc_ToggleGrass();

	ImGui::Separator();

	if (ImGui::CollapsingHeader(T("feature.procedural_grass.base_type_section", "Base Grass Type Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::ColorEdit3(T("feature.procedural_grass.base_color", "Base Color"), reinterpret_cast<float*>(&settings.baseColor));
		DrawSettingDescription(T("feature.procedural_grass.base_color_tooltip", "Sets the color at the base of each blade before texture-specific overrides."));
		ImGui::ColorEdit3(T("feature.procedural_grass.tip_color", "Tip Color"), reinterpret_cast<float*>(&settings.tipColor));
		DrawSettingDescription(T("feature.procedural_grass.tip_color_tooltip", "Sets the color at the tip of each blade before texture-specific overrides."));

		if (ImGui::CollapsingHeader(T("feature.procedural_grass.colour_variation_section", "Colour Variation"))) {
			ImGui::SliderFloat(T("feature.procedural_grass.hue_variation", "Hue Variation"), &settings.grassColorHueVariation, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.hue_variation_tooltip", "Randomly shifts blade hue to reduce uniform coloring."));
			ImGui::SliderFloat(T("feature.procedural_grass.brightness_variation", "Brightness Variation"), &settings.grassColorValueVariation, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.brightness_variation_tooltip", "Randomly varies blade brightness."));
			ImGui::SliderFloat(T("feature.procedural_grass.tip_dry_strength", "Tip Dry Strength"), &settings.grassColorTipDryStrength, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.tip_dry_strength_tooltip", "Controls how strongly the dried-tip tint affects blade tips."));
			ImGui::SliderFloat(T("feature.procedural_grass.mottle_strength", "Mottle Strength"), &settings.grassColorMottleStrength, 0.0f, 0.5f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.mottle_strength_tooltip", "Adds broad color variation across each blade."));

			ImGui::SliderFloat3(T("feature.procedural_grass.cool_tint", "Cool/Green Tint"), reinterpret_cast<float*>(&settings.grassColorCool), 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.cool_tint_tooltip", "Sets the tint used by cooler blade color variation."));
			ImGui::SliderFloat3(T("feature.procedural_grass.warm_tint", "Warm/Straw Tint"), reinterpret_cast<float*>(&settings.grassColorWarm), 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.warm_tint_tooltip", "Sets the tint used by warmer blade color variation."));
			ImGui::SliderFloat3(T("feature.procedural_grass.dried_tip_tint", "Dried Tip Tint"), reinterpret_cast<float*>(&settings.grassColorTipDry), 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.dried_tip_tint_tooltip", "Sets the color applied to dried blade tips."));
			ImGui::SliderFloat(T("feature.procedural_grass.clump_colour_patches", "Clump Colour Patches"), &settings.grassClumpColorStrength, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.clump_color_tooltip", "Varies color between neighboring grass clumps."));
		}

		if (ImGui::CollapsingHeader(T("feature.procedural_grass.blade_detail_section", "Blade Detail"))) {
			ImGui::SliderFloat(T("feature.procedural_grass.canopy_base_shading", "Canopy Base Shading"), &settings.grassBaseAO, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.canopy_base_shading_tooltip", "Darkens blade bases beneath the grass canopy."));
			ImGui::SliderFloat(T("feature.procedural_grass.micro_detail", "Micro Detail"), &settings.grassMicroDetail, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.micro_detail_tooltip", "Controls fine surface detail on individual blades."));

			// Surface texture. Grain fades out with distance so it cannot alias into shimmer on the far field.
			ImGui::SliderFloat(T("feature.procedural_grass.blotch_strength", "Blotch Strength"), &settings.grassBlotchStrength, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.blotch_strength_tooltip", "Controls the intensity of broad surface blotches."));
			ImGui::SliderFloat(T("feature.procedural_grass.blotch_scale", "Blotch Scale"), &settings.grassBlotchScale, 0.25f, 4.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.blotch_scale_tooltip", "Controls the size of broad surface blotches."));
			ImGui::SliderFloat(T("feature.procedural_grass.grain_strength", "Grain Strength"), &settings.grassSpeckleStrength, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.grain_strength_tooltip", "Controls the intensity of fine blade grain."));
			ImGui::SliderFloat(T("feature.procedural_grass.grain_scale", "Grain Scale"), &settings.grassSpeckleScale, 0.25f, 4.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.grain_scale_tooltip", "Controls the size of fine blade grain."));

			ImGui::SeparatorText(T("feature.procedural_grass.veins_section", "Veins"));
			ImGui::SliderFloat3(T("feature.procedural_grass.vein_tint", "Vein Tint"), reinterpret_cast<float*>(&settings.grassVeinTint), 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.vein_tint_tooltip", "Sets the color of blade veins."));
			ImGui::SliderFloat(T("feature.procedural_grass.vein_tint_strength", "Vein Tint Strength"), &settings.grassVeinAlbedoStrength, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.vein_tint_strength_tooltip", "Controls how strongly veins affect blade color."));
			ImGui::SliderFloat(T("feature.procedural_grass.vein_normal_strength", "Vein Normal Strength"), &settings.grassVeinNormalStrength, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.vein_normal_strength_tooltip", "Controls how strongly veins affect blade normals."));
			ImGui::SliderFloat(T("feature.procedural_grass.vein_ripple_depth", "Vein Ripple Depth"), &settings.grassVeinRippleDepth, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.vein_ripple_depth_tooltip", "Controls the depth of the rippled vein profile."));
			ImGui::SliderFloat(T("feature.procedural_grass.vein_micro_wiggle", "Vein Micro-Wiggle"), &settings.grassVeinWiggleAmount, 0.0f, 0.25f, "%.3f");
			DrawSettingDescription(T("feature.procedural_grass.vein_wiggle_tooltip", "Adds small irregular bends along blade veins."));
		}

		if (ImGui::CollapsingHeader(T("feature.procedural_grass.blade_lighting_section", "Blade Lighting"))) {
			// 0 uses each blade's own normal for ambient (noisy), 1 uses straight up (flat but coherent).
			ImGui::SliderFloat(T("feature.procedural_grass.ambient_normal_flatten", "Ambient Normal Flatten"), &settings.grassAmbientFlatten, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.ambient_normal_flatten_tooltip", "Blends blade normals toward vertical for smoother ambient lighting."));
			ImGui::SliderFloat(T("feature.procedural_grass.canopy_sky_occlusion", "Canopy Sky Occlusion"), &settings.grassCanopySkyOcclusion, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.canopy_sky_occlusion_tooltip", "Reduces skylight beneath dense grass canopies."));
			ImGui::SliderFloat(T("feature.procedural_grass.density_occlusion", "Density Occlusion"), &settings.grassDensityAO, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.density_occlusion_tooltip", "Darkens areas containing more overlapping blades."));
			ImGui::SliderFloat(T("feature.procedural_grass.terminator_wrap", "Terminator Wrap"), &settings.grassWrap, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.terminator_wrap_tooltip", "Wraps direct light around blades to soften the light-shadow boundary."));
			ImGui::SliderFloat(T("feature.procedural_grass.anisotropic_specular", "Anisotropic Specular"), &settings.grassAniso, 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.anisotropic_specular_tooltip", "Controls elongated highlights along the blade direction."));
			ImGui::SliderFloat(T("feature.procedural_grass.ground_bounce", "Ground Bounce"), &settings.grassBounceStrength, 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.ground_bounce_tooltip", "Controls indirect light reflected from the ground onto blades."));
			ImGui::SliderFloat3(T("feature.procedural_grass.ground_bounce_tint", "Ground Bounce Tint"), reinterpret_cast<float*>(&settings.grassBounceColor), 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.ground_bounce_tint_tooltip", "Tints indirect light reflected from the ground."));
			ImGui::SliderFloat(T("feature.procedural_grass.sun_self_shadow", "Sun Self-Shadow"), &settings.grassSunSelfShadow, 0.0f, 2.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.sun_self_shadow_tooltip", "Controls direct-light shadowing within the grass canopy."));
			ImGui::SliderFloat(T("feature.procedural_grass.specular_occlusion", "Specular Occlusion"), &settings.grassSpecOcclusion, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.specular_occlusion_tooltip", "Suppresses highlights in occluded parts of the canopy."));
			ImGui::SliderFloat(T("feature.procedural_grass.ambient_desaturation", "Ambient Desaturation"), &settings.grassAmbientDesat, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.ambient_desaturation_tooltip", "Removes color from ambient light on grass."));
		}

		if (ImGui::CollapsingHeader(T("feature.procedural_grass.terrain_blend_section", "Terrain Blend"))) {
			// Blade bases dither-dissolve into the real terrain in the GBuffer, softening the hard base edge.
			ImGui::SliderFloat(T("feature.procedural_grass.base_dissolve", "Base Dissolve"), &settings.grassTerrainBlendStrength, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.base_dissolve_tooltip", "Dissolves blade bases into the terrain to hide their intersection."));
			ImGui::SliderFloat(T("feature.procedural_grass.dissolve_height", "Dissolve Height (units)"), &settings.grassTerrainBlendHeight, 0.0f, 60.0f, "%.1f");
			DrawSettingDescription(T("feature.procedural_grass.dissolve_height_tooltip", "Sets how far the terrain blend extends up each blade."));
			ImGui::SliderFloat(T("feature.procedural_grass.base_normal_flatten", "Base Normal Flatten"), &settings.grassTerrainBlendNormal, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.base_normal_flatten_tooltip", "Blends blade-base normals toward the terrain normal."));
			ImGui::SliderFloat(T("feature.procedural_grass.base_roughness", "Base Roughness"), &settings.grassTerrainBlendRough, 0.0f, 1.0f, "%.2f");
			DrawSettingDescription(T("feature.procedural_grass.base_roughness_tooltip", "Sets blade roughness near the terrain intersection."));
		}

		ImGui::Separator();

		ImGui::SliderFloat(T("feature.procedural_grass.height", "Height"), &settings.grassHeight, 0.0f, 150.0f, "%.1f");
		DrawSettingDescription(T("feature.procedural_grass.height_tooltip", "Sets the default blade height in world units."));
		ImGui::SliderFloat(T("feature.procedural_grass.width", "Width"), &settings.grassWidth, 0.0f, 10.0f, "%.1f");
		DrawSettingDescription(T("feature.procedural_grass.width_tooltip", "Sets the default blade width."));
		ImGui::SliderFloat(T("feature.procedural_grass.view_thicken", "View Thicken"), &settings.grassViewThicken, 0.0f, 2.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.view_thicken_tooltip", "Widens blades viewed edge-on to keep them visible."));
		ImGui::SliderFloat(T("feature.procedural_grass.k1", "K1"), &settings.stiffness, -10.0f, 10.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.k1_tooltip", "Controls random sideways curvature through the middle of each blade."));
		ImGui::SliderFloat(T("feature.procedural_grass.k2", "K2"), &settings.tipWeight, -10.0f, 10.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.k2_tooltip", "Controls the random tilt applied to blade tips."));
		ImGui::SliderFloat(T("feature.procedural_grass.mid", "Mid"), &settings.mid, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.mid_tooltip", "Positions the middle control point along the blade to shape its curve."));
		ImGui::SliderFloat(T("feature.procedural_grass.rotational_stiffness", "Rotational Stiffness"), &settings.rotationalStiffness, 0.0f, 10.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.rotational_stiffness_tooltip", "Controls how strongly blades resist rotating to face the wind."));

		ImGui::Separator();

		ImGui::SliderFloat(T("feature.procedural_grass.baked_min_ao", "Baked Min AO"), &settings.ao, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.baked_min_ao_tooltip", "Sets the minimum ambient occlusion baked into each blade."));
		ImGui::SliderFloat2(T("feature.procedural_grass.subsurface_opacity", "Subsurface Opacity (Base>Tip)"), reinterpret_cast<float*>(&settings.subsurfaceOpacity), 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.subsurface_opacity_tooltip", "Sets blade opacity at the base and tip. Higher values transmit less light."));
		ImGui::SliderFloat3(T("feature.procedural_grass.subsurface_color", "Subsurface Color"), reinterpret_cast<float*>(&settings.grassSubsurfaceTint), 0.0f, 2.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.subsurface_color_tooltip", "Tints light transmitted through blades."));
		ImGui::SliderFloat(T("feature.procedural_grass.specular", "Specular"), &settings.specular, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.specular_tooltip", "Controls the strength of blade highlights."));
		ImGui::SliderFloat3(T("feature.procedural_grass.roughness", "Roughness (Base>Min>Tip)"), reinterpret_cast<float*>(&settings.baseMinTipRoughness), 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.roughness_tooltip", "Sets roughness at the blade base, minimum point, and tip."));
		// Kept off 0 and 1 so neither smoothstep in the vertex shader collapses to a zero-width range.
		ImGui::SliderFloat(T("feature.procedural_grass.roughness_tip_start", "Roughness Tip Start"), &settings.tipRoughnessStart, 0.05f, 0.95f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.roughness_tip_start_tooltip", "Sets where roughness begins transitioning toward the tip value."));
		ImGui::SliderFloat(T("feature.procedural_grass.clump_ao_strength", "Clump AO Strength"), &settings.clumpAOStrength, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.clump_ao_tooltip", "Controls ambient occlusion between blades in a clump."));
	}

	if (ImGui::CollapsingHeader(T("feature.procedural_grass.global_settings_section", "Global Grass Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T("feature.procedural_grass.terrain_shadow_strength", "Terrain Shadow Strength"), &settings.grassAOStrength, 0.0f, 2.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.terrain_shadow_strength_tooltip", "Controls how strongly grass darkens the terrain beneath it."));
		ImGui::SliderFloat(T("feature.procedural_grass.terrain_shadow_density", "Terrain Shadow Density"), &settings.grassAODensity, 1.0f, 64.0f, "%.0f");
		DrawSettingDescription(T("feature.procedural_grass.terrain_shadow_density_tooltip", "Controls how quickly terrain darkening builds with grass density."));

		ImGui::Separator();

		ImGui::SliderInt(T("feature.procedural_grass.clump_grid_size", "Clump Grid Size"), &settings.voronoiGridSize, 1, 4096);
		DrawSettingDescription(T("feature.procedural_grass.clump_grid_size_tooltip", "Sets the average spacing between generated grass clumps."));
		ImGui::SliderFloat(T("feature.procedural_grass.clump_distance_factor", "Clump Distance Factor"), &settings.clumpDistanceFactor, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.clump_distance_tooltip", "Pulls blades toward their clump center."));
		ImGui::SliderFloat(T("feature.procedural_grass.clump_facing_factor", "Clump Facing Factor"), &settings.clumpFacingFactor, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.clump_facing_tooltip", "Turns blades toward their clump center."));
		ImGui::SliderFloat(T("feature.procedural_grass.clump_height_factor", "Clump Height Factor"), &settings.clumpHeightFactor, 0.0f, 2.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.clump_height_tooltip", "Varies blade height between grass clumps."));

		ImGui::Separator();

		// Cull is disabled at 90 degrees, lower trims grass off cliffs first.
		ImGui::SliderFloat(T("feature.procedural_grass.max_slope", "Max Slope (deg)"), &settings.grassMaxSlope, 0.0f, 90.0f, "%.0f");
		DrawSettingDescription(T("feature.procedural_grass.max_slope_tooltip", "Stops normal grass from growing on slopes above this angle. 90 disables the limit."));
		ImGui::SliderFloat(T("feature.procedural_grass.min_slope", "Min Slope (deg)"), &settings.grassMinSlope, 0.0f, 90.0f, "%.0f");
		DrawSettingDescription(T("feature.procedural_grass.min_slope_tooltip", "Stops grass from growing on slopes below this angle."));
		ImGui::SliderFloat(T("feature.procedural_grass.slope_facing", "Slope Facing"), &settings.grassSlopeFacing, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.slope_facing_tooltip", "Leans blades downhill based on terrain steepness."));

		ImGui::SeparatorText(T("feature.procedural_grass.wind_section", "Wind"));
		if (ImGui::SliderAngle(T("feature.procedural_grass.wind_direction", "Wind Direction"), &settings.windAngle)) {
			windDirection = float2(cos(settings.windAngle), sin(settings.windAngle));
		}
		DrawSettingDescription(T("feature.procedural_grass.wind_direction_tooltip", "Sets the horizontal direction of grass movement."));

		ImGui::SliderFloat(T("feature.procedural_grass.wind_speed", "Wind Speed"), &settings.windSpeed, 0.0f, 1.0f);
		DrawSettingDescription(T("feature.procedural_grass.wind_speed_tooltip", "Controls how quickly wind waves move through the grass."));

		ImGui::SliderFloat(T("feature.procedural_grass.phase_offset", "Phase Offset"), &settings.phaseOffset, 0.0f, 10.0f);
		DrawSettingDescription(T("feature.procedural_grass.phase_offset_tooltip", "Legacy wind control retained for configuration compatibility; currently unused."));
		ImGui::SliderFloat(T("feature.procedural_grass.phase_lag", "Phase Lag"), &settings.phaseLag, 0.0f, 1.0f);
		DrawSettingDescription(T("feature.procedural_grass.phase_lag_tooltip", "Legacy wind control retained for configuration compatibility; currently unused."));
		ImGui::SliderFloat(T("feature.procedural_grass.spatial_frequency", "Spatial Freq"), &settings.spatialFreq, 0.0f, 100.0f);
		DrawSettingDescription(T("feature.procedural_grass.spatial_frequency_tooltip", "Legacy wind control retained for configuration compatibility; currently unused."));

		ImGui::SeparatorText(T("feature.procedural_grass.occlusion_section", "Occlusion"));

		ImGui::SliderFloat(T("feature.procedural_grass.occluder_padding", "Occluder Padding (units)"), &settings.occlusionPadding, 0.0f, 128.0f, "%.0f");
		DrawSettingDescription(T("feature.procedural_grass.occluder_padding_tooltip", "Expands occluder footprints to remove grass around object edges."));
		ImGui::SliderFloat(T("feature.procedural_grass.occluder_height_bias", "Occluder Height Bias (units)"), &settings.occlusionBias, 0.0f, 64.0f, "%.1f");
		DrawSettingDescription(T("feature.procedural_grass.occluder_bias_tooltip", "Sets how far an occluder must extend above a blade position before suppressing grass."));
		// Cull grass only where an occluder's underside is within this height of the ground.
		ImGui::SliderFloat(T("feature.procedural_grass.occlusion_clearance", "Occlusion Clearance (units)"), &settings.occlusionClearance, 0.0f, 512.0f, "%.0f");
		DrawSettingDescription(T("feature.procedural_grass.occlusion_clearance_tooltip", "Sets the maximum gap between terrain and an object that can suppress grass."));
		ImGui::Separator();

		ImGui::SeparatorText(T("feature.procedural_grass.lod_density_section", "LOD Density"));
		if (ImGui::SliderInt(T("feature.procedural_grass.high_density", "Density (High LOD)"), &settings.Quality, 0, static_cast<uint8_t>(Quality::Count) - 1, QualityNames[settings.Quality], ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
			grassRendererHighLOD->SetDensity(QualityDensities[settings.Quality]);
		DrawSettingDescription(T("feature.procedural_grass.high_density_tooltip", "Sets blade density in the closest, highest-detail grass tier."));
		if (ImGui::SliderInt(T("feature.procedural_grass.mid_density", "Density (Mid LOD)"), &settings.midGrassDensity, 8, 320, "%d", ImGuiSliderFlags_AlwaysClamp))
			grassRendererMidLOD->SetDensity(static_cast<uint32_t>(settings.midGrassDensity));
		DrawSettingDescription(T("feature.procedural_grass.mid_density_tooltip", "Sets blade density in the middle-distance grass tier."));
		if (ImGui::SliderInt(T("feature.procedural_grass.low_density", "Density (Low LOD)"), &settings.lowGrassDensity, 8, 320, "%d", ImGuiSliderFlags_AlwaysClamp)) {
			grassRendererLowLOD->SetDensity(static_cast<uint32_t>(settings.lowGrassDensity));
			grassRendererFarLOD->SetDensity(FarPatchDensity());
		}
		DrawSettingDescription(T("feature.procedural_grass.low_density_tooltip", "Sets blade density in the low-detail grass tier and scales far-tier density."));

		if (ImGui::SliderInt(T("feature.procedural_grass.far_radius", "Far Grass Radius (cells)"), &settings.grassCellRadius, 0, 15, "%d", ImGuiSliderFlags_AlwaysClamp))
			grassRendererFarLOD->SetBladeQuadrantCapacity(FarBladeQuadrantCapacity());
		DrawSettingDescription(T("feature.procedural_grass.far_radius_tooltip", "Sets how many exterior cells beyond loaded grass receive the far grass tier."));
		if (ImGui::SliderInt(T("feature.procedural_grass.far_density", "Far Grass Density"), &settings.farGrassDensity, 8, 160, "%d", ImGuiSliderFlags_AlwaysClamp))
			grassRendererFarLOD->SetDensity(FarPatchDensity());
		DrawSettingDescription(T("feature.procedural_grass.far_density_tooltip", "Sets blade density in the far grass tier."));
		ImGui::SliderFloat(T("feature.procedural_grass.far_edge_density", "Far Edge Density"), &settings.farDensityFalloff, 0.0f, 1.0f, "%.2f");
		DrawSettingDescription(T("feature.procedural_grass.far_edge_density_tooltip", "Sets the remaining grass density at the outer edge of the far tier."));
	}

	ImGui::Separator();

	DrawGrassTypeEditor();

	ImGui::Separator();

	if (ImGui::CollapsingHeader("Debug")) {
		bool invalidate = false;
		invalidate |= ImGui::Checkbox("Ignore grass map (LTEX)", &settings.debugIgnoreGrassMap);
		DrawSettingDescription(T("feature.procedural_grass.debug_ignore_grass_map_tooltip", "Generates grass without consulting landscape texture grass assignments."));
		ImGui::Checkbox("Ignore object occlusion", &settings.debugIgnoreObjectOcclusion);
		DrawSettingDescription(T("feature.procedural_grass.debug_ignore_occlusion_tooltip", "Disables removal of grass beneath or inside occluding objects."));
		ImGui::SliderFloat("Grass map edge noise (units)", &settings.grassMapEdgeNoise, 0.0f, 256.0f, "%.0f");
		DrawSettingDescription(T("feature.procedural_grass.debug_edge_noise_tooltip", "Jitters grass-map sampling near texture boundaries to soften distribution edges."));
		if (ImGui::SliderFloat("Occlusion half extent", &settings.occlusionHalfExtent, 1024.0f, 16384.0f, "%.0f"))
			TopDownOcclusion::GetSingleton()->SetHalfExtent(settings.occlusionHalfExtent);
		DrawSettingDescription(T("feature.procedural_grass.debug_occlusion_extent_tooltip", "Sets the half-width of the world-space object-occlusion window."));
		{
			const auto td = TopDownOcclusion::GetSingleton();
			const auto centre = td->GetWindowCentre();
			ImGui::Text("Occlusion map: %s   %u x %u", td->IsReady() ? "ready" : "NOT READY", td->GetMapDim(), td->GetMapDim());
			ImGui::Text("Window centre: %.0f, %.0f   half extent %.0f   %.1f units/texel",
				centre.x, centre.y, td->GetHalfExtent(), td->GetHalfExtent() * 2.0f / td->GetMapDim());
			ImGui::Text("Occluders drawn: %u", td->GetDrawCount());
		}
		ImGui::Checkbox("Disable ALL generator culls", &settings.debugDisableAllCulls);
		DrawSettingDescription(T("feature.procedural_grass.debug_disable_culls_tooltip", "Disables generator rejection tests for debugging."));
		ImGui::Checkbox("Ignore preprocessed-node check", &settings.debugIgnorePreProcessedFlag);
		DrawSettingDescription(T("feature.procedural_grass.debug_ignore_preprocessed_tooltip", "Includes LAND meshes that are not marked as preprocessed."));
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
	if (!ImGui::CollapsingHeader(T("feature.procedural_grass.grass_types_section", "Grass Types")))
		return;

	const auto allocatedGrassVariants = typeAllocation.size();
	const auto maxGrassVariants = PGrassCommon::MaxGrassTypes - 2;
	const auto grassTypesDescription = std::vformat(
		T("feature.procedural_grass.grass_types_description",
			"Grass types are per landscape texture. Expand a texture and add one or more type variants; each overrides "
			"only the fields you tick (unticked fields inherit the base settings above) and carries a weight. A texture's "
			"blades are split between its variants in proportion to their weights. A texture with no variants grows the "
			"base type when it supports vanilla grass. Configured variants also enable procedural grass on textures with "
			"no vanilla grass. No Grass variants suppress their weighted share without consuming a type slot. Allocated "
			"grass variants: {} / {}."),
		std::make_format_args(allocatedGrassVariants, maxGrassVariants));
	ImGui::TextWrapped("%s", grassTypesDescription.c_str());

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
		float val = has && ov[key].is_number() ? ov[key].get<float>() : base;
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
		float2 val = base;
		if (has && IsNumericArray(ov[key], 2))
			ov[key].get_to(val);
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
		float3 val = base;
		if (has && IsNumericArray(ov[key], 3))
			ov[key].get_to(val);
		bool changed = asColor ? ImGui::ColorEdit3(label, &val.x) : ImGui::SliderFloat3(label, &val.x, mn, mx, "%.2f");
		if (changed && has)
			ov[key] = val;
		ImGui::EndDisabled();
	};

	const auto renderOverrides = [&](nlohmann::json& ov) {
		ImGui::SeparatorText(T("feature.procedural_grass.shape_section", "Shape"));
		fFloat(ov, "Height", T("feature.procedural_grass.height", "Height"), s.grassHeight, 0.0f, 150.0f, "%.1f");
		fFloat(ov, "Width", T("feature.procedural_grass.width", "Width"), s.grassWidth, 0.0f, 10.0f, "%.1f");
		fFloat(ov, "Stiffness", T("feature.procedural_grass.k1", "K1"), s.stiffness, -10.0f, 10.0f);
		fFloat(ov, "TipWeight", T("feature.procedural_grass.k2", "K2"), s.tipWeight, -10.0f, 10.0f);
		fFloat(ov, "Mid", T("feature.procedural_grass.mid", "Mid"), s.mid, 0.0f, 1.0f);
		fFloat(ov, "RotationalStiffness", T("feature.procedural_grass.rotational_stiffness", "Rotational Stiffness"), s.rotationalStiffness, 0.0f, 10.0f);

		ImGui::SeparatorText(T("feature.procedural_grass.slope_section", "Slope"));
		fFloat(ov, "MinSlope", T("feature.procedural_grass.min_slope", "Min Slope (deg)"), s.grassMinSlope, 0.0f, 90.0f, "%.0f");
		fFloat(ov, "MaxSlope", T("feature.procedural_grass.max_slope", "Max Slope (deg)"), s.grassMaxSlope, 0.0f, 90.0f, "%.0f");

		ImGui::SeparatorText(T("feature.procedural_grass.clump_section", "Clump"));
		fFloat(ov, "ClumpDistanceFactor", T("feature.procedural_grass.clump_distance", "Clump Distance"), s.clumpDistanceFactor, 0.0f, 1.0f);
		fFloat(ov, "ClumpFacingFactor", T("feature.procedural_grass.clump_facing", "Clump Facing"), s.clumpFacingFactor, 0.0f, 1.0f);
		fFloat(ov, "ClumpHeightFactor", T("feature.procedural_grass.clump_height", "Clump Height"), s.clumpHeightFactor, 0.0f, 2.0f);
		fFloat(ov, "ClumpAOStrength", T("feature.procedural_grass.clump_ao", "Clump AO"), s.clumpAOStrength, 0.0f, 1.0f);
		fFloat(ov, "ClumpColorStrength", T("feature.procedural_grass.clump_colour", "Clump Colour"), s.grassClumpColorStrength, 0.0f, 1.0f);

		ImGui::SeparatorText(T("feature.procedural_grass.colour_section", "Colour"));
		fFloat3(ov, "BaseColor", T("feature.procedural_grass.base_color", "Base Color"), s.baseColor, 0.0f, 1.0f, true);
		fFloat3(ov, "TipColor", T("feature.procedural_grass.tip_color", "Tip Color"), s.tipColor, 0.0f, 1.0f, true);
		fFloat3(ov, "ColorTipDry", T("feature.procedural_grass.dried_tip_tint", "Dried Tip Tint"), s.grassColorTipDry, 0.0f, 2.0f, false);
		fFloat3(ov, "ColorCool", T("feature.procedural_grass.cool_tint", "Cool/Green Tint"), s.grassColorCool, 0.0f, 2.0f, false);
		fFloat3(ov, "ColorWarm", T("feature.procedural_grass.warm_tint", "Warm/Straw Tint"), s.grassColorWarm, 0.0f, 2.0f, false);
		fFloat(ov, "HueVariation", T("feature.procedural_grass.hue_variation", "Hue Variation"), s.grassColorHueVariation, 0.0f, 1.0f);
		fFloat(ov, "ValueVariation", T("feature.procedural_grass.brightness_variation", "Brightness Variation"), s.grassColorValueVariation, 0.0f, 1.0f);
		fFloat(ov, "TipDryStrength", T("feature.procedural_grass.tip_dry_strength", "Tip Dry Strength"), s.grassColorTipDryStrength, 0.0f, 1.0f);
		fFloat(ov, "MottleStrength", T("feature.procedural_grass.mottle_strength", "Mottle Strength"), s.grassColorMottleStrength, 0.0f, 0.5f);

		ImGui::SeparatorText(T("feature.procedural_grass.lighting_section", "Lighting"));
		fFloat(ov, "MinAO", T("feature.procedural_grass.baked_min_ao", "Baked Min AO"), s.ao, 0.0f, 1.0f);
		fFloat(ov, "Specular", T("feature.procedural_grass.specular", "Specular"), s.specular, 0.0f, 1.0f);
		fFloat2(ov, "SubsurfaceOpacity", T("feature.procedural_grass.subsurface_base_tip", "Subsurface (Base>Tip)"), s.subsurfaceOpacity, 0.0f, 1.0f);
		fFloat3(ov, "SubsurfaceTint", T("feature.procedural_grass.subsurface_color", "Subsurface Color"), s.grassSubsurfaceTint, 0.0f, 2.0f, false);
		fFloat3(ov, "BaseMinTipRoughness", T("feature.procedural_grass.roughness", "Roughness (Base>Min>Tip)"), s.baseMinTipRoughness, 0.0f, 1.0f, false);
		fFloat(ov, "TipRoughnessStart", T("feature.procedural_grass.roughness_tip_start", "Roughness Tip Start"), s.tipRoughnessStart, 0.05f, 0.95f);
		fFloat(ov, "MicroDetail", T("feature.procedural_grass.micro_detail", "Micro Detail"), s.grassMicroDetail, 0.0f, 1.0f);
		fFloat(ov, "AmbientFlatten", T("feature.procedural_grass.ambient_flatten", "Ambient Flatten"), s.grassAmbientFlatten, 0.0f, 1.0f);
		fFloat(ov, "Wrap", T("feature.procedural_grass.terminator_wrap", "Terminator Wrap"), s.grassWrap, 0.0f, 1.0f);
		fFloat(ov, "Aniso", T("feature.procedural_grass.anisotropic_specular", "Anisotropic Specular"), s.grassAniso, 0.0f, 2.0f);
		fFloat(ov, "BounceStrength", T("feature.procedural_grass.ground_bounce", "Ground Bounce"), s.grassBounceStrength, 0.0f, 2.0f);
		fFloat3(ov, "BounceColor", T("feature.procedural_grass.ground_bounce_tint", "Ground Bounce Tint"), s.grassBounceColor, 0.0f, 2.0f, false);
		fFloat(ov, "SpecOcclusion", T("feature.procedural_grass.specular_occlusion", "Specular Occlusion"), s.grassSpecOcclusion, 0.0f, 1.0f);
		fFloat(ov, "AmbientDesat", T("feature.procedural_grass.ambient_desaturation", "Ambient Desaturation"), s.grassAmbientDesat, 0.0f, 1.0f);

		ImGui::SeparatorText(T("feature.procedural_grass.surface_wind_section", "Surface & Wind"));
		fFloat(ov, "BlotchStrength", T("feature.procedural_grass.blotch_strength", "Blotch Strength"), s.grassBlotchStrength, 0.0f, 1.0f);
		fFloat(ov, "BlotchScale", T("feature.procedural_grass.blotch_scale", "Blotch Scale"), s.grassBlotchScale, 0.25f, 4.0f);
		fFloat(ov, "SpeckleStrength", T("feature.procedural_grass.grain_strength", "Grain Strength"), s.grassSpeckleStrength, 0.0f, 1.0f);
		fFloat(ov, "SpeckleScale", T("feature.procedural_grass.grain_scale", "Grain Scale"), s.grassSpeckleScale, 0.25f, 4.0f);
		fFloat(ov, "SpatialFreq", T("feature.procedural_grass.spatial_frequency", "Spatial Freq"), s.spatialFreq, 0.0f, 100.0f);
		fFloat(ov, "PhaseOffset", T("feature.procedural_grass.phase_offset", "Phase Offset"), s.phaseOffset, 0.0f, 10.0f);
		fFloat(ov, "PhaseLag", T("feature.procedural_grass.phase_lag", "Phase Lag"), s.phaseLag, 0.0f, 1.0f);

		ImGui::SeparatorText(T("feature.procedural_grass.veins_section", "Veins"));
		fFloat3(ov, "VeinTint", T("feature.procedural_grass.vein_tint", "Vein Tint"), s.grassVeinTint, 0.0f, 2.0f, false);
		fFloat(ov, "VeinAlbedoStrength", T("feature.procedural_grass.vein_tint_strength", "Vein Tint Strength"), s.grassVeinAlbedoStrength, 0.0f, 1.0f);
		fFloat(ov, "VeinNormalStrength", T("feature.procedural_grass.vein_normal_strength", "Vein Normal Strength"), s.grassVeinNormalStrength, 0.0f, 1.0f);
		fFloat(ov, "VeinRippleDepth", T("feature.procedural_grass.vein_ripple_depth", "Vein Ripple Depth"), s.grassVeinRippleDepth, 0.0f, 1.0f);
		fFloat(ov, "VeinWiggleAmount", T("feature.procedural_grass.vein_micro_wiggle", "Vein Micro-Wiggle"), s.grassVeinWiggleAmount, 0.0f, 0.25f);

		ImGui::Spacing();
		if (ImGui::Button(T("feature.procedural_grass.clear_overrides", "Clear all overrides")))
			ov = nlohmann::json::object();
	};

	static char filter[128] = "";
	ImGui::InputTextWithHint(T("feature.procedural_grass.filter", "Filter"), T("feature.procedural_grass.filter_hint", "editor id / plugin"), filter, sizeof(filter));
	static bool onlyGrass = true;
	ImGui::SameLine();
	ImGui::Checkbox(T("feature.procedural_grass.only_grass_growing", "Only grass-growing"), &onlyGrass);

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler) {
		ImGui::TextDisabled("%s", T("feature.procedural_grass.data_handler_unavailable", "Data handler unavailable (not in a loaded game)."));
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

			const char* edid = ltex->GetFormEditorID();
			const std::string name = (edid && edid[0]) ? edid : T("feature.procedural_grass.no_editor_id", "<no editor id>");
			const std::string key = LandTextureKey(ltex);
			auto texIt = settings.textureTypes.find(key);
			const size_t count = texIt != settings.textureTypes.end() ? texIt->second.size() : 0;
			const bool growsVanillaGrass = !ltex->textureGrassList.empty();
			if (onlyGrass && !growsVanillaGrass && count == 0)
				continue;
			if (!matchesFilter(name, filter) && !matchesFilter(key, filter))
				continue;

			const auto vanillaGrassStatus = growsVanillaGrass ? "" : T("feature.procedural_grass.no_vanilla_grass", "(no vanilla grass) ");
			const auto typeLabel = count == 1 ? T("feature.procedural_grass.type_singular", "type") : T("feature.procedural_grass.type_plural", "types");
			const auto textureSummary = std::vformat(
				T("feature.procedural_grass.texture_type_summary", "{}   {}[{} {}]"),
				std::make_format_args(name, vanillaGrassStatus, count, typeLabel));
			if (!ImGui::TreeNode(key.c_str(), "%s", textureSummary.c_str()))
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
				const auto variantIndex = i + 1;
				const auto variantLabel = std::vformat(T("feature.procedural_grass.variant", "Variant {}"), std::make_format_args(variantIndex));
				ImGui::TextUnformatted(variantLabel.c_str());
				ImGui::SameLine();
				ImGui::PushItemWidth(90.0f);
				if (ImGui::DragFloat(T("feature.procedural_grass.weight", "Weight"), &def.weight, 0.1f, 0.0f, 100.0f, "%.1f"))
					typesChanged = true;
				ImGui::PopItemWidth();
				ImGui::SameLine();
				ImGui::TextDisabled("(%.0f%%)", pct);
				ImGui::SameLine();
				if (ImGui::SmallButton(T("feature.procedural_grass.remove", "Remove")))
					removeIndex = static_cast<int>(i);

				if (ImGui::Checkbox(T("feature.procedural_grass.no_grass", "No Grass"), &def.noGrass))
					typesChanged = true;
				DrawSettingDescription(T("feature.procedural_grass.no_grass_tooltip", "Makes this weighted variant produce bare terrain instead of grass."));

				ImGui::BeginDisabled(def.noGrass);
				const auto overrideCount = def.overrides.is_object() ? def.overrides.size() : 0;
				const auto overridesLabel = std::vformat(
					T("feature.procedural_grass.overrides_count", "Overrides ({} set)"),
					std::make_format_args(overrideCount));
				if (ImGui::TreeNode("Overrides", "%s", overridesLabel.c_str())) {
					renderOverrides(def.overrides);
					ImGui::TreePop();
				}
				ImGui::EndDisabled();

				ImGui::PopID();
				ImGui::Separator();
			}

			if (removeIndex >= 0) {
				defs.erase(defs.begin() + removeIndex);
				typesChanged = true;
			}

			if (typeAllocation.size() + 2 < PGrassCommon::MaxGrassTypes) {
				if (ImGui::SmallButton(T("feature.procedural_grass.add_variant", "Add variant"))) {
					defs.push_back({});
					typesChanged = true;
				}
			} else {
				const auto typePoolCapacity = PGrassCommon::MaxGrassTypes - 2;
				const auto typePoolFull = std::vformat(
					T("feature.procedural_grass.type_pool_full", "Type pool full ({})."),
					std::make_format_args(typePoolCapacity));
				ImGui::TextDisabled("%s", typePoolFull.c_str());
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
	settings.Quality = std::clamp(settings.Quality, 0, static_cast<int32_t>(Quality::Count) - 1);

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

	LoadTextureTypes();
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
	SaveTextureTypes();
	o_json = settings;

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
