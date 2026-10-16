#include "GrassOptimizations.h"
#include "GrassLighting.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"

#define I18N_KEY_PREFIX "feature.grass_optimizations."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassOptimizations::Settings,
	MinPixelSize,
	FullDetailPixelSize,
	MinDensity,
	MeshCostBias,
	CostBiasStartDistance,
	InvisibleFadeCull,
	RenderDistanceOverride,
	EdgeFadeStart,
	EnableOcclusionCulling,
	OcclusionBias,
	SimpleShadingPixelSize,
	CollisionDistance,
	EnableMeshLOD,
	EnableMidLOD,
	MidLODPixelSize,
	EnableFarLOD,
	FarLODPixelSize,
	MeshLODBandPixels)

void GrassOptimizations::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassOptimizations::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassOptimizations::RestoreDefaultSettings()
{
	settings = {};
}

void GrassOptimizations::DrawSettings()
{
	ImGui::SeparatorText(T(TKEY("culling"), "Culling & LOD"));

	ImGui::SliderFloat(T(TKEY("full_detail_pixel_size"), "Full-Detail Pixel Size"), &settings.FullDetailPixelSize, 4.0f, 128.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("full_detail_pixel_size_tooltip"),
							  "Instances whose on-screen radius is above this render at full density. Below it, density is increasingly thinned down to Minimum Density at Min Pixel Size. Increasing this setting improves performance by removing closer grass."));
	}

	ImGui::SliderFloat(T(TKEY("min_pixel_size"), "Min Pixel Size"), &settings.MinPixelSize, 1.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_pixel_size_tooltip"),
							  "Individual grass instances that visibly take up less space on the screen than this are dropped entirely. Higher values improve performance by removing far-away grass instances sooner."));
	}

	Util::PercentageSlider(T(TKEY("min_density"), "Minimum Density"), &settings.MinDensity);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_density_tooltip"),
							  "The percentage of grass that remains at the smallest (Min Pixel Size) LOD level before culling."));
	}

	Util::PercentageSlider(T(TKEY("mesh_cost_bias"), "Mesh Cost Bias"), &settings.MeshCostBias);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_cost_bias_tooltip"),
							  "Culls or removes grass meshes based on their complexity (performance impact). At 0, removal is identical between all grass types regardless of complexity. At 1, heavier and more complex meshes are culled 2-6x sooner than simple ones. Only applies beyond the Cost Bias Start Distance, so nearby grass is never thinned."));
	}

	ImGui::SliderFloat(T(TKEY("cost_bias_start_distance"), "Cost Bias Start Distance"), &settings.CostBiasStartDistance, 0.0f, 20000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("cost_bias_start_distance_tooltip"),
				"Distance at which Mesh Cost Bias starts taking effect, ramping to full over the same distance again. Nearer than this, all grass types are treated identically no matter how complex. Zero applies the bias everywhere, including right in front of the player."),
			Util::Units::FormatDistance(settings.CostBiasStartDistance)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::SliderFloat(T(TKEY("render_distance_override"), "Grass Render Distance"), &settings.RenderDistanceOverride, 0.0f, 100000.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("render_distance_override_tooltip"),
							  "Max grass render distance in units. 0 = use the game's INI cap (fGrassStartFadeDistance + fGrassFadeRange). Any grass beyond the vanilla range or this range will be removed."));
	}

	Util::PercentageSlider(T(TKEY("edge_fade_start"), "Edge Fade Start"), &settings.EdgeFadeStart);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("edge_fade_start_tooltip"),
				"Percent of the grass render distance at which grass starts fading out. The default of 0.85 fades over the last 15%. A lower value results in a longer, smoother fade out, while a value of 1.0 disables the fade and grass pops out at the render distance."),
			Util::Units::FormatDistance(maxGrassDistance * settings.EdgeFadeStart)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::SliderFloat(T(TKEY("invisible_fade_cull"), "Invisible Fade Cull"), &settings.InvisibleFadeCull, 0.0f, 0.5f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("invisible_fade_cull_tooltip"),
							  "Skip drawing grass whose transparency is below this threshold. Grass with a fade value of zero is completely invisible and thus is removed early for performance reasons."));
	}

	ImGui::SliderFloat(T(TKEY("simple_shading_px"), "Simple Shading Below"), &settings.SimpleShadingPixelSize, 0.0f, 32.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("simple_shading_px_tooltip"),
							  "Grass instances smaller than this size on screen will skip barely visible detail including contact shadows, specular highlights, and other complex grass visual elements. Zero disables this feature."));
	}

	ImGui::SliderFloat(T(TKEY("collision_distance"), "Collision Distance"), &settings.CollisionDistance, 0.0f, 8192.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::vector<std::string> tooltipLines = {
			T(TKEY("collision_distance_tooltip"),
				"Grass beyond this distance skips any collision detection. Zero disables collision on all grass. Requires the Grass Collision feature."),
			Util::Units::FormatDistance(settings.CollisionDistance)
		};
		Util::DrawMultiLineTooltip(tooltipLines);
	}

	ImGui::Spacing();

	ImGui::Checkbox(T(TKEY("occlusion_culling"), "Occlusion Culling"), &settings.EnableOcclusionCulling);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("occlusion_culling_tooltip"),
							  "Skips grass hidden behind rocks, buildings and NPCs. Depending on how much grass is not visible, this may cost more than its benefits. If you see grass flickering when moving, try disabling this. Terrain such as hills is not treated as an occluder when Terrain Blending is enabled, which reduces the benefit."));
	}

	ImGui::SliderFloat(T(TKEY("occlusion_bias"), "Occlusion Bias"), &settings.OcclusionBias, 0.0f, 0.05f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("occlusion_bias_tooltip"),
							  "How far behind an occluder grass must sit before Occlusion Culling removes it. Raise this if grass disappears around the edges of rocks and hills, lower it to reclaim more performance. Has no effect unless Occlusion Culling is enabled."));
	}

	ImGui::SeparatorText(T(TKEY("mesh_lod"), "Mesh LOD"));

	ImGui::Checkbox(T(TKEY("enable_mesh_lod"), "Enable Mesh LOD"), &settings.EnableMeshLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_mesh_lod_tooltip"),
							  "Improves performance by swapping distant grass instances for a simpler LOD mesh, in two bands. Requires an LOD .nif per grass type at meshes\\LOD\\Grass\\<source-mesh-name>_LOD0.nif, plus an optional _LOD1.nif for the far band. Grass with no LOD mesh keeps its full mesh."));
	}

	ImGui::BeginDisabled(!settings.EnableMeshLOD);

	ImGui::Checkbox(T(TKEY("enable_mid_lod"), "Enable Middle LOD"), &settings.EnableMidLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_mid_lod_tooltip"),
							  "Swaps mid-distance grass to the _LOD0.nif mesh. With this off, grass stays on its full mesh until the far band takes over."));
	}

	ImGui::SliderFloat(T(TKEY("mid_lod_pixel_size"), "Middle LOD Pixel Size"), &settings.MidLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mid_lod_pixel_size_tooltip"),
							  "Instances whose on-screen radius is below this but above the Far LOD Pixel Size swap to the _LOD0.nif mesh."));
	}

	ImGui::Checkbox(T(TKEY("enable_far_lod"), "Enable Far LOD"), &settings.EnableFarLOD);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_far_lod_tooltip"),
							  "Swaps the most distant grass to the _LOD1.nif mesh. Grass types without that file reuse their _LOD0.nif, so the far band still gets its own brightness."));
	}

	ImGui::SliderFloat(T(TKEY("far_lod_pixel_size"), "Far LOD Pixel Size"), &settings.FarLODPixelSize, 1.0f, 64.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("far_lod_pixel_size_tooltip"),
							  "Instances whose on-screen radius is below this but above Min Pixel Size swap to the _LOD1.nif mesh. Values above the Middle LOD Pixel Size are clamped to it, since the far band is always the more distant of the two."));
	}

	ImGui::SliderFloat(T(TKEY("mesh_lod_band"), "Mesh LOD Transition Band"), &settings.MeshLODBandPixels, 0.0f, 16.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("mesh_lod_band_tooltip"),
							  "The range of on-screen sizes over which a random amount of meshes are swapped out before completely transitioning to the next LOD. Applies to both transitions. A wider range results in a smoother transition."));
	}

	ImGui::EndDisabled();
}

void GrassOptimizations::PostPostLoad()
{
	Hooks::Install();
}

bool GrassOptimizations::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Grass:
		return true;
	default:
		return false;
	}
}

void GrassOptimizations::Reset()
{
	std::scoped_lock lk(bucketStore.bucketMutex);
	bucketStore.ProcessRemovals();
	bucketStore.meshLibrary.CommitLoadedLODMeshes();
}

void GrassOptimizations::ClearShaderCache()
{
	auto release = [](ID3D11ComputeShader*& shader) {
		if (shader)
			shader->Release();
		shader = nullptr;
	};
	release(cullCS);
	cullCSFailed = false;
	hiZ.ClearShaderCache();
	bucketStore.ClearShaderCache();
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_dtor::thunk(RE::BSMultiStreamInstanceTriShape* shape)
{
	globals::features::grassOptimizations.bucketStore.StageRemoval(shape);
	func(shape);
}

void GrassOptimizations::Hooks::BSMultiStreamInstanceTriShape_OnVisible::thunk(RE::BSMultiStreamInstanceTriShape* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex)
{
	auto prop = This->GetGeometryRuntimeData().shaderProperty;
	if (prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		auto& self = globals::features::grassOptimizations;

		// Only queue one representative shape per frame for each bucket to skip redundant setup.
		if (!self.bucketStore.ClaimQueueSlot(This, globals::game::graphicsState->frameCount))
			return;

		// Skips redundant and costly frustum checks since they are now handled by the coarse slice cull and CS.
		auto& shape = *This;
		process->AppendVirtual(shape, alphaGroupIndex);
		return;
	}

	func(This, process, alphaGroupIndex);
}

void GrassOptimizations::Hooks::DoneAddingInstances::thunk(RE::BSMultiStreamInstanceTriShape* shape,
	RE::BSTArray<std::uint32_t>& a_instances)
{
	auto& self = globals::features::grassOptimizations;

	auto& rt = shape->GetMultiStreamTrishapeRuntimeData();
	auto prop = shape->GetGeometryRuntimeData().shaderProperty;
	if (rt.groupAlloc && prop && prop->GetRTTI() == globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		if (auto* tex = prop->GetBaseTexture()) {
			const uint64_t descVal = *reinterpret_cast<uint64_t*>(&shape->GetGeometryRuntimeData().vertexDesc);
			self.bucketStore.StageCapture(shape, rt.groupAlloc, rt.instanceCount,
				2u * rt.instanceSize, descVal, tex);
		}
	}
	func(shape, a_instances);
}

void GrassOptimizations::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags)
{
	auto& self = globals::features::grassOptimizations;

	const auto frame = globals::game::graphicsState->frameCount;
	if (self.lastFrame != frame) {
		self.UpdateGrass();
		self.lastFrame = frame;
	}

	func(This, a2, flags);

	auto* shadowState = globals::game::shadowState;
	shadowState->SetPSTextureAddressMode(0, RE::BSGraphics::TextureAddressMode::kClampSClampT);
	shadowState->SetPSTextureFilterMode(0, RE::BSGraphics::TextureFilterMode::kAnisotropic);
}

void GrassOptimizations::Hooks::BSGrassShader_SetupMaterial::thunk([[maybe_unused]] RE::BSShader* This, const RE::BSLightingShaderMaterialBase* material)
{
	globals::features::grassOptimizations.pendingMaterial = material;
}

bool GrassOptimizations::SetupGrassPass(RE::BSShader* shader, RE::BSRenderPass* pass)
{
	const auto* material = std::exchange(pendingMaterial, nullptr);

	if (loaded && SetupOptimizedPass(pass))
		return true;

	if (material)
		Hooks::BSGrassShader_SetupMaterial::func(shader, material);
	return false;
}

bool GrassOptimizations::SetupOptimizedPass(RE::BSRenderPass* pass)
{
	auto* geometry = pass->geometry;
	auto* shaderProperty = pass->shaderProperty;
	if (!geometry || !shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get())
		return false;
	auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(shaderProperty->material);
	if (!material || !material->diffuseTexture || !material->diffuseTexture->rendererTexture)
		return false;

	auto* smState = globals::game::smState;
	auto* sceneNode = smState->shadowSceneNode[0];
	auto* sunLight = sceneNode && sceneNode->GetRuntimeData().sunLight ? skyrim_cast<RE::NiDirectionalLight*>(sceneNode->GetRuntimeData().sunLight->light.get()) : nullptr;
	if (smState->liteBrite || !sunLight)
		return false;

	const bool pbrTechnique = static_cast<SIE::ShaderCache::GrassShaderTechniques>(globals::state->currentPixelDescriptor & 0b1111) ==
	                          SIE::ShaderCache::GrassShaderTechniques::TruePbr;

	std::scoped_lock lk(bucketStore.bucketMutex);
	auto* b = bucketStore.FindBucket(static_cast<RE::BSMultiStreamInstanceTriShape*>(geometry));
	if (!b || !b->totalInstances || !b->optimizedReady.load(std::memory_order_relaxed) || (pbrTechnique && !b->pbrMaterial))
		return false;

	// The draw hook skips this pass, so it needs no setup.
	if (!b->cullVisible || (b->drawnFrame == globals::game::graphicsState->frameCount && b->drawnPassKey == GetPassKey(pass)))
		return true;

	if (!BindPerGeometry(geometry, shaderProperty, sunLight))
		return false;
	auto* shadowState = globals::game::shadowState;
	shadowState->SetPSTexture(0, material->diffuseTexture->rendererTexture);
	shadowState->SetPSTextureAddressMode(0, RE::BSGraphics::TextureAddressMode::kClampSClampT);
	if (pbrTechnique) {
		TruePBR::SetGrassPBRTextures(static_cast<const BSLightingShaderMaterialPBR*>(material));

		ID3D11Buffer* bucketCB = cullBucketCB->CB();
		UINT first = (b->cullSlot * kSlotBytes + kSlotMaterialOffset) / 16;
		UINT num = 16;
		ctx1->PSSetConstantBuffers1(1, 1, &bucketCB, &first, &num);
	}
	return true;
}

bool GrassOptimizations::BindPerGeometry(const RE::BSGeometry* geometry, const RE::BSShaderProperty* shaderProperty, RE::NiDirectionalLight* sunLight)
{
	// SetFogNearColor stores the fog color at c16 in vanilla PerGeometry constants.
	static const auto* fogNearColor = reinterpret_cast<const float*>(REL::RelocationID(527805, 414756).address() + 0x100);
	static const auto* grassWindAmount = reinterpret_cast<const float*>(REL::RelocationID(527802, 414753).address());
	static const auto* grassWindAmountMax = reinterpret_cast<const float*>(REL::RelocationID(504665, 373550).address());
	static const auto* windGrassMultiplier = reinterpret_cast<const float*>(REL::RelocationID(513337, 391112).address());

	if (!perGeometryCB || !ctx1)
		return false;

	GrassPerGeometry data{};
	std::memcpy(data.fogNearColor, fogNearColor, sizeof(data.fogNearColor));

	// The wind blows along the model's Y axis.
	RE::NiPoint3 windDirection{ geometry->world.rotate.entry[1][0], geometry->world.rotate.entry[1][1], geometry->world.rotate.entry[1][2] };
	windDirection.Unitize();
	data.windVector[0] = windDirection.x;
	data.windVector[1] = windDirection.y;
	data.windVector[2] = std::min(*grassWindAmount, *grassWindAmountMax) * *windGrassMultiplier;

	const auto& sunDirection = sunLight->GetWorldDirection();
	data.dirLightDirection[0] = -sunDirection.x;
	data.dirLightDirection[1] = -sunDirection.y;
	data.dirLightDirection[2] = -sunDirection.z;

	const auto& sun = sunLight->GetLightRuntimeData();
	data.dirLightColor[0] = sun.diffuse.red;
	data.dirLightColor[1] = sun.diffuse.green;
	data.dirLightColor[2] = sun.diffuse.blue;
	data.ambientColor[0] = sun.ambient.red;
	data.ambientColor[1] = sun.ambient.green;
	data.ambientColor[2] = sun.ambient.blue;

	const bool uniformScale = shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kUniformScale);
	data.scaleMask[0] = uniformScale ? 1.0f : 0.0f;
	data.scaleMask[1] = uniformScale ? 1.0f : 0.0f;
	data.scaleMask[2] = 1.0f;

	auto variant = std::ranges::find_if(perGeometryVariants, [&](const GrassPerGeometry& v) { return std::memcmp(&v, &data, sizeof(data)) == 0; });
	if (variant == perGeometryVariants.end()) {
		if (perGeometryVariants.size() == kMaxPerGeometryVariants)
			perGeometryVariants.clear();
		perGeometryVariants.push_back(data);

		auto* ctx = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(ctx->Map(perGeometryCB->CB(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			perGeometryVariants.clear();
			return false;
		}
		auto* bytes = static_cast<uint8_t*>(m.pData);
		for (size_t i = 0; i < perGeometryVariants.size(); ++i)
			std::memcpy(bytes + (i + 1) * kPerGeometrySlotBytes, &perGeometryVariants[i], sizeof(GrassPerGeometry));
		ctx->Unmap(perGeometryCB->CB(), 0);
		variant = perGeometryVariants.end() - 1;
	}
	ID3D11Buffer* cb = perGeometryCB->CB();
	UINT first = (UINT)(variant - perGeometryVariants.begin()) * (kPerGeometrySlotBytes / 16);
	UINT num = 2 * (kPerGeometrySlotBytes / 16);
	ctx1->VSSetConstantBuffers1(2, 1, &cb, &first, &num);
	ctx1->PSSetConstantBuffers1(2, 1, &cb, &first, &num);
	return true;
}

uint64_t GrassOptimizations::GetPassKey(const RE::BSRenderPass* pass)
{
	uint32_t descriptor = 0;
	if (globals::game::currentPixelShader && *globals::game::currentPixelShader)
		descriptor = (*globals::game::currentPixelShader)->id;
	return (static_cast<uint64_t>(pass->passEnum) << 32) | descriptor;
}

static size_t GIDGroupBytes(const RE::BSMultiStreamInstanceTriShape::GroupHeader* header)
{
	if (!header || !header->numShortsPerInstance)
		return 0;
	return (size_t)header->groupInstanceCount * header->numShortsPerInstance * sizeof(std::uint16_t);
}

std::uint32_t GrassOptimizations::Hooks::AddGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3)
{
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2));
	return func(a1, a2, a3);
}

std::uint32_t GrassOptimizations::Hooks::AddQueuedGroupGIDBuffer::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSMultiStreamInstanceTriShape::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4)
{
	globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, a2, a3, GIDGroupBytes(a2));
	return func(a1, a2, a3, a4);
}

thread_local RE::BSMultiStreamInstanceTriShape::GroupHeader tl_lastFileGroupHeader{};
thread_local std::vector<uint16_t> tl_lastFileInstanceData;
thread_local bool tl_haveFileGroup = false;

void GrassOptimizations::Hooks::ReadGroupHeaderStreamTraits::thunk(RE::BSStreamHeader* streamHeader, RE::BSMultiStreamInstanceTriShape::GroupHeader* groupHeader, uint32_t size)
{
	func(streamHeader, groupHeader, size);
	std::memcpy(&tl_lastFileGroupHeader, groupHeader, std::min<uint32_t>(size, sizeof(tl_lastFileGroupHeader)));
}

void GrassOptimizations::Hooks::ReadInstanceGroupStreamTraits::thunk(RE::BSStreamHeader* streamHeader, uint16_t* instanceData, uint32_t size)
{
	func(streamHeader, instanceData, size);
	tl_lastFileInstanceData.resize((size + sizeof(uint16_t) - 1) / sizeof(uint16_t));
	std::memcpy(tl_lastFileInstanceData.data(), instanceData, size);
	tl_haveFileGroup = true;
}

void GrassOptimizations::Hooks::AddGroupQueuedGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3)
{
	tl_haveFileGroup = false;
	func(a1, a2, a3);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

void GrassOptimizations::Hooks::AddGroupGIDFile::thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2)
{
	tl_haveFileGroup = false;
	func(a1, a2);

	if (tl_haveFileGroup) {
		globals::features::grassOptimizations.bucketStore.CaptureGIDGroup(a1, &tl_lastFileGroupHeader,
			tl_lastFileInstanceData.data(), tl_lastFileInstanceData.size() * sizeof(uint16_t));
		tl_haveFileGroup = false;
	}
}

RE::BSMultiStreamInstanceTriShape* GrassOptimizations::Hooks::LoadGrassType::thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath)
{
	auto* shape = func(grassManager, a_param, CellXDivided, CellYDivided, typeKey, modelPath);

	if (shape && modelPath)
		globals::features::grassOptimizations.bucketStore.meshLibrary.RecordModelPath(shape, modelPath->c_str());

	return shape;
}

void VanillaDrawInstanceTriShape(RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto* ctx = globals::d3d::context;
	auto& groups = geometry->GetMultiStreamTrishapeRuntimeData().instanceGroups;

	for (uint32_t i = 0; i < groups.size(); ++i) {
		auto curInstanceGroup = groups[i];
		if (!curInstanceGroup || !curInstanceGroup->isVisible)
			continue;

		uint32_t indexCount = 0;
		uint32_t* indexCountPtr = &indexCount;
		static REL::Relocation<ID3D11Buffer** (*)(RE::BSGraphics::Renderer*, uint64_t, uint32_t**, uint32_t)> MapDynamicBuffer{ REL::RelocationID(75561, 77362) };
		auto buffer = MapDynamicBuffer(globals::game::renderer, 1, &indexCountPtr, 7);
		if (buffer && indexCountPtr) {
			*indexCountPtr = i;
			if (*buffer)
				ctx->Unmap(*buffer, 0);
			ctx->VSSetConstantBuffers(7u, 1u, buffer);
		}

		static REL::Relocation<void (*)(RE::BSGraphics::Renderer*, RE::BSGraphics::TriShape*, uint32_t, uint32_t, uint32_t, RE::BSGraphics::VertexDesc, RE::BSGraphics::VertexBuffer*)> DrawInstancedTriShape{ REL::RelocationID(75479, 77265) };
		DrawInstancedTriShape(globals::game::renderer, geometry->GetGeometryRuntimeData().rendererData, 0, geometry->GetTrishapeRuntimeData().triangleCount, curInstanceGroup->instanceCount, geometry->GetGeometryRuntimeData().vertexDesc, curInstanceGroup->vertexBuffer);
	}
}

void GrassOptimizations::Hooks::DrawInstanceTriShape::thunk(RE::BSRenderPass* pass, RE::BSMultiStreamInstanceTriShape* geometry)
{
	auto& self = globals::features::grassOptimizations;
	auto* ctx = globals::d3d::context;

	auto shaderProperty = geometry->GetGeometryRuntimeData().shaderProperty;
	if (!shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get()) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}
	RE::NiSourceTexture* diffuseTexture = shaderProperty->GetBaseTexture();
	if (!diffuseTexture) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	const uint64_t descVal = *reinterpret_cast<uint64_t*>(&geometry->GetGeometryRuntimeData().vertexDesc);
	const uint32_t frame = globals::game::graphicsState->frameCount;

	// Keep the bucket and its GPU resources alive through every draw.
	std::scoped_lock lk(self.bucketStore.bucketMutex);
	// Look up the bucket by shape because a key rebuilt from engine state can miss the capture.
	auto* b = self.bucketStore.FindBucket(geometry);
	if (!b || !b->totalInstances || !b->optimizedReady.load(std::memory_order_relaxed)) {
		VanillaDrawInstanceTriShape(geometry);
		return;
	}

	// Multiple shapes share a bucket; draw it once per frame and technique.
	const uint64_t passKey = GetPassKey(pass);

	if (b->drawnFrame == frame && b->drawnPassKey == passKey)
		return;
	b->drawnFrame = frame;
	b->drawnPassKey = passKey;

	if (!b->cullVisible) {
		return;
	}

	auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData)
		return;
	auto* meshVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	auto* indexB = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
	if (!meshVB || !indexB)
		return;

	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	if (shadowState.vertexDesc != descVal) {
		shadowState.vertexDesc = descVal;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
	}
	if (shadowState.topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
		shadowState.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
	}
	static REL::Relocation<void (*)(uint32_t)> SetDirtyStates{ REL::RelocationID(75580, 77386) };

	// SetDirtyStates binds each dirty pixel texture on its own; bind the material's slots in one call instead.
	constexpr uint32_t kMaterialTextureBits = 0x1F;
	const uint32_t materialTextures = shadowState.PSResourceModifiedBits & kMaterialTextureBits;
	shadowState.PSResourceModifiedBits &= ~kMaterialTextureBits;
	SetDirtyStates(0);
	if (materialTextures) {
		const uint32_t firstTexture = std::countr_zero(materialTextures);
		const uint32_t textureCount = std::bit_width(materialTextures) - firstTexture;
		ctx->PSSetShaderResources(firstTexture, textureCount, reinterpret_cast<ID3D11ShaderResourceView* const*>(&shadowState.PSTexture[firstTexture]));
	}

	ctx->IASetIndexBuffer(indexB, DXGI_FORMAT_R16_UINT, 0);

	ID3D11Buffer* vbs[2] = { meshVB, nullptr };
	UINT strides[2] = { VertexStrideFromDesc(descVal), kGrassStride };
	UINT offsets[2] = { 0, 0 };
	if (!strides[0])
		return;

	vbs[1] = b->compactedBuf;
	ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
	ctx->VSSetShaderResources(2, 1, &b->extrasSRV);
	ctx->DrawIndexedInstancedIndirect(b->argsBuf, argsByteOffset);

	for (uint32_t tier = 0; tier < (uint32_t)GrassMeshLibrary::LODTier::kCount; ++tier) {
		GrassBucket::LODBin& bin = b->lodBins[tier];
		if (!bin.active)
			continue;

		const auto* lod = self.bucketStore.meshLibrary.GetLODMesh(b->meshId, (GrassMeshLibrary::LODTier)tier);
		if (!lod || !lod->vertexBuffer || !lod->indexBuffer)
			continue;

		if (bin.argsIndexCount != lod->indexCount) {
			const D3D11_BOX argBox{ argsByteOffset, 0, 0, argsByteOffset + sizeof(uint32_t), 1, 1 };
			ctx->UpdateSubresource(bin.argsBuf, 0, &argBox, &lod->indexCount, 0, 0);
			bin.argsIndexCount = lod->indexCount;
		}

		if (shadowState.vertexDesc != lod->descVal) {
			shadowState.vertexDesc = lod->descVal;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VERTEX_DESC);
			SetDirtyStates(0);
		}

		ctx->IASetIndexBuffer(lod->indexBuffer, DXGI_FORMAT_R16_UINT, 0);

		vbs[0] = lod->vertexBuffer;
		vbs[1] = bin.compactedBuf;
		strides[0] = lod->meshStride;
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->VSSetShaderResources(2, 1, &bin.extrasSRV);
		ctx->DrawIndexedInstancedIndirect(bin.argsBuf, argsByteOffset);
	}
}
