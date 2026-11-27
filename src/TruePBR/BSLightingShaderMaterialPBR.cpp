#include "BSLightingShaderMaterialPBR.h"

#include "TruePBR.h"

/** @brief Drops textureSet unless its NiRTTI derives from BSTextureSet; malformed meshes can link any block, and calling BSTextureSet virtuals on it is a CTD. */
static void DiscardMislinkedTextureSet(RE::NiPointer<RE::BSTextureSet>& textureSet)
{
	if (textureSet == nullptr) {
		return;
	}

	static const auto* textureSetRTTI = REL::Relocation<const RE::NiRTTI*>{ RE::BSTextureSet::Ni_RTTI }.get();
	const RE::NiRTTI* rtti = textureSet->GetRTTI();
	if (rtti == nullptr || !rtti->IsKindOf(textureSetRTTI)) {
		textureSet.reset();
	}
}

/**
 * @brief Path of the interior window map for a mesh and its window texture, or empty when the material has no room.
 *
 * Window textures are often atlases, and meshes build one window from several pieces that can reuse or mirror the
 * same texels, so the map is per mesh: textures\\interiormaps\\<mesh path>\\<diffuse name>.dds. Each texel holds the
 * whole window's UV rectangle in the frame of the piece covering it. Mesh patchers can set texture slots but cannot
 * add per-window data, hence the side file.
 */
static std::string GetInteriorWindowMapPath(RE::BSTextureSet* textureSet, std::string_view meshPath)
{
	const char* roomPath = textureSet->GetTexturePath(BSLightingShaderMaterialPBR::InteriorTexture);
	const char* diffusePath = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
	if (!roomPath || !roomPath[0] || !diffusePath || !diffusePath[0] || meshPath.empty()) {
		return {};
	}

	const auto normalize = [](std::string_view path) {
		std::string result(path);
		std::ranges::transform(result, result.begin(), [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		return result;
	};
	const std::string mesh = normalize(meshPath);
	const auto meshesFolder = mesh.rfind("meshes\\");
	const auto meshStart = meshesFolder == std::string::npos ? 0 : meshesFolder + 7;
	const auto meshEnd = mesh.rfind('.');
	if (meshEnd == std::string::npos || meshEnd <= meshStart) {
		return {};
	}

	const std::string diffuse = normalize(diffusePath);
	const auto nameStart = diffuse.find_last_of('\\') + 1;
	const auto nameEnd = diffuse.rfind('.');
	if (nameEnd == std::string::npos || nameEnd <= nameStart) {
		return {};
	}

	return std::format("textures\\interiormaps\\{}\\{}.dds", mesh.substr(meshStart, meshEnd - meshStart), diffuse.substr(nameStart, nameEnd - nameStart));
}

/** @brief Loads the interior window map for a mesh, when one was generated for it. */
static RE::NiPointer<RE::NiSourceTexture> LoadInteriorWindowMap(RE::BSTextureSet* textureSet, std::string_view meshPath)
{
	const std::string path = GetInteriorWindowMapPath(textureSet, meshPath);
	if (path.empty() || !RE::BSResourceNiBinaryStream(path).good()) {
		return nullptr;
	}

	RE::NiPointer<RE::NiTexture> texture;
	RE::BSShaderManager::GetTexture(path.c_str(), true, texture, false);
	if (!texture || texture->GetRTTI() != globals::rtti::NiSourceTextureRTTI.get()) {
		return nullptr;
	}
	return RE::NiPointer(static_cast<RE::NiSourceTexture*>(texture.get()));
}

/** @brief Whether a texture path is a room under textures\\interiors, where the interior room export writes. */
static bool IsInteriorRoomPath(const char* path)
{
	if (!path) {
		return false;
	}
	std::string lowered(path);
	std::ranges::transform(lowered, lowered.begin(), [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
	return lowered.starts_with("textures\\interiors\\") || lowered.starts_with("interiors\\") || lowered.find("\\textures\\interiors\\") != std::string::npos;
}

/** @brief Clamps interior mapping parameters to the ranges the shader supports, replacing non-finite values with defaults. */
static InteriorMappingParameters SanitizeInteriorMappingParameters(const InteriorMappingParameters& parameters)
{
	const InteriorMappingParameters defaults;
	const auto clampFinite = [](float value, float low, float high, float fallback) {
		return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
	};

	InteriorMappingParameters result;
	result.enabled = parameters.enabled;
	result.atlasColumns = std::clamp(parameters.atlasColumns, 1, 16);
	result.atlasRows = std::clamp(parameters.atlasRows, 1, 16);
	result.backWallScale = clampFinite(parameters.backWallScale, 0.05f, 0.95f, defaults.backWallScale);
	result.depthScale = clampFinite(parameters.depthScale, 0.1f, 4.f, defaults.depthScale);
	result.daylightScale = clampFinite(parameters.daylightScale, 0.f, 1.f, defaults.daylightScale);
	result.roomSize = clampFinite(parameters.roomSize, 16.f, 4096.f, defaults.roomSize);
	result.cubemap = parameters.cubemap;
	return result;
}

BSLightingShaderMaterialPBR::~BSLightingShaderMaterialPBR()
{
	All.erase(this);
}

BSLightingShaderMaterialPBR* BSLightingShaderMaterialPBR::Make()
{
	auto* scrapHeap = globals::game::memoryManager->GetThreadScrapHeap();
	auto* material = static_cast<BSLightingShaderMaterialPBR*>(scrapHeap->Allocate(sizeof(BSLightingShaderMaterialPBR), 8));
	if (material) {
		std::memset(material, 0, sizeof(BSLightingShaderMaterialPBR));
		std::construct_at(material);
	}
	return material;
}

RE::BSShaderMaterial* BSLightingShaderMaterialPBR::Create()
{
	// Must use regular heap (not scrap heap like Make()). BSLightingShaderProperty::LinkObject
	// calls ScrapHeap::Free() after LinkMaterial — if Create() used scrap heap, it would pop
	// the canonical off the stack, causing immediate use-after-free in property->material.
	return new BSLightingShaderMaterialPBR();
}

void BSLightingShaderMaterialPBR::CopyMembers(RE::BSShaderMaterial* that)
{
	BSLightingShaderMaterialBase::CopyMembers(that);

	auto* pbrThat = static_cast<BSLightingShaderMaterialPBR*>(that);

	loadedWithFeature = pbrThat->loadedWithFeature;
	// Needed to find the mesh's interior window map
	inputFilePath = pbrThat->inputFilePath;
	pbrFlags = pbrThat->pbrFlags;
	nifThinSurfaceFlags = pbrThat->nifThinSurfaceFlags;
	subsurfaceBalance = pbrThat->subsurfaceBalance;
	thinSurfaceParameters = pbrThat->thinSurfaceParameters;
	coatRoughness = pbrThat->coatRoughness;
	coatSpecularLevel = pbrThat->coatSpecularLevel;
	fuzzColor = pbrThat->fuzzColor;
	fuzzWeight = pbrThat->fuzzWeight;
	glintParameters = pbrThat->glintParameters;
	interiorMappingParameters = pbrThat->interiorMappingParameters;
	projectedMaterialBaseColorScale = pbrThat->projectedMaterialBaseColorScale;
	projectedMaterialRoughness = pbrThat->projectedMaterialRoughness;
	projectedMaterialSpecularLevel = pbrThat->projectedMaterialSpecularLevel;
	projectedMaterialGlintParameters = pbrThat->projectedMaterialGlintParameters;

	rmaosTexture = pbrThat->rmaosTexture;
	emissiveTexture = pbrThat->emissiveTexture;
	displacementTexture = pbrThat->displacementTexture;
	featuresTexture0 = pbrThat->featuresTexture0;
	featuresTexture1 = pbrThat->featuresTexture1;
	interiorTexture = pbrThat->interiorTexture;
	interiorWindowMapTexture = pbrThat->interiorWindowMapTexture;
	interiorRoomTexture = pbrThat->interiorRoomTexture;

	All[this] = All[pbrThat];
}

std::uint32_t BSLightingShaderMaterialPBR::ComputeCRC32(uint32_t srcHash)
{
	struct HashContainer
	{
		uint32_t pbrFlags = 0;
		uint32_t nifThinSurfaceFlags = 0;
		uint32_t hasNifThinSurfaceParameters = 0;
		uint32_t hasNifInteriorMappingParameters = 0;
		float subsurfaceBalance = 0.75f;
		std::array<float, 3> thinSurfaceParameters = { 0.5f, 0.5f, 1.f };
		float coatRoughness = 0.f;
		float coatSpecularLevel = 0.f;
		std::array<float, 3> fuzzColor = { 0.f, 0.f, 0.f };
		float fuzzWeight = 0.f;
		float screenSpaceScale = 0.f;
		float logMicrofacetDensity = 0.f;
		float microfacetRoughness = 0.f;
		float densityRandomization = 0.f;
		uint32_t interiorMappingEnabled = 0;
		int32_t interiorMappingAtlasColumns = 0;
		int32_t interiorMappingAtlasRows = 0;
		float interiorMappingBackWallScale = 0.f;
		float interiorMappingDepthScale = 0.f;
		float interiorMappingDaylightScale = 0.f;
		uint32_t interiorMappingCubemap = 0;
		float interiorMappingRoomSize = 0.f;
		std::array<float, 3> projectedMaterialBaseColorScale = { 0.f, 0.f, 0.f };
		float projectedMaterialRoughness = 0.f;
		float projectedMaterialSpecularLevel = 0.f;
		float projectedMaterialScreenSpaceScale = 0.f;
		float projectedMaterialLogMicrofacetDensity = 0.f;
		float projectedMaterialMicrofacetRoughness = 0.f;
		float projectedMaterialDensityRandomization = 0.f;
		uint32_t rmaodHash = 0;
		uint32_t emissiveHash = 0;
		uint32_t displacementHash = 0;
		uint32_t features0Hash = 0;
		uint32_t features1Hash = 0;
		uint32_t interiorHash = 0;
		uint32_t interiorWindowMapHash = 0;
		uint32_t baseHash = 0;
	} hashes;

	hashes.pbrFlags = pbrFlags.underlying();
	hashes.nifThinSurfaceFlags = nifThinSurfaceFlags.underlying();
	if (const auto it = All.find(this); it != All.end()) {
		hashes.hasNifThinSurfaceParameters = it->second.nifThinSurfaceParameters.has_value();
		hashes.hasNifInteriorMappingParameters = it->second.nifInteriorMappingParameters.has_value();
	}
	hashes.subsurfaceBalance = subsurfaceBalance;
	hashes.thinSurfaceParameters = thinSurfaceParameters;
	hashes.coatRoughness = coatRoughness * 100.f;
	hashes.coatSpecularLevel = coatSpecularLevel * 100.f;
	hashes.fuzzColor[0] = fuzzColor[0] * 100.f;
	hashes.fuzzColor[1] = fuzzColor[1] * 100.f;
	hashes.fuzzColor[2] = fuzzColor[2] * 100.f;
	hashes.fuzzWeight = fuzzWeight * 100.f;
	hashes.screenSpaceScale = glintParameters.screenSpaceScale * 100.f;
	hashes.logMicrofacetDensity = glintParameters.logMicrofacetDensity * 100.f;
	hashes.microfacetRoughness = glintParameters.microfacetRoughness * 100.f;
	hashes.densityRandomization = glintParameters.densityRandomization * 100.f;
	hashes.interiorMappingEnabled = interiorMappingParameters.enabled;
	hashes.interiorMappingAtlasColumns = interiorMappingParameters.atlasColumns;
	hashes.interiorMappingAtlasRows = interiorMappingParameters.atlasRows;
	hashes.interiorMappingBackWallScale = interiorMappingParameters.backWallScale * 100.f;
	hashes.interiorMappingDepthScale = interiorMappingParameters.depthScale * 100.f;
	hashes.interiorMappingDaylightScale = interiorMappingParameters.daylightScale * 100.f;
	hashes.interiorMappingCubemap = interiorMappingParameters.cubemap;
	hashes.interiorMappingRoomSize = interiorMappingParameters.roomSize;
	hashes.projectedMaterialBaseColorScale[0] = projectedMaterialBaseColorScale[0] * 100.f;
	hashes.projectedMaterialBaseColorScale[1] = projectedMaterialBaseColorScale[1] * 100.f;
	hashes.projectedMaterialBaseColorScale[2] = projectedMaterialBaseColorScale[2] * 100.f;
	hashes.projectedMaterialRoughness = projectedMaterialRoughness * 100.f;
	hashes.projectedMaterialSpecularLevel = projectedMaterialSpecularLevel * 100.f;
	hashes.projectedMaterialScreenSpaceScale = projectedMaterialGlintParameters.screenSpaceScale * 100.f;
	hashes.projectedMaterialLogMicrofacetDensity = projectedMaterialGlintParameters.logMicrofacetDensity * 100.f;
	hashes.projectedMaterialMicrofacetRoughness = projectedMaterialGlintParameters.microfacetRoughness * 100.f;
	hashes.projectedMaterialDensityRandomization = projectedMaterialGlintParameters.densityRandomization * 100.f;
	DiscardMislinkedTextureSet(textureSet);
	if (textureSet != nullptr) {
		hashes.rmaodHash = RE::BSCRC32<const char*>()(textureSet->GetTexturePath(RmaosTexture));
		hashes.emissiveHash = RE::BSCRC32<const char*>()(textureSet->GetTexturePath(EmissiveTexture));
		hashes.displacementHash = RE::BSCRC32<const char*>()(textureSet->GetTexturePath(DisplacementTexture));
		hashes.features0Hash = RE::BSCRC32<const char*>()(textureSet->GetTexturePath(FeaturesTexture0));
		hashes.features1Hash = RE::BSCRC32<const char*>()(textureSet->GetTexturePath(FeaturesTexture1));
		hashes.interiorHash = RE::BSCRC32<const char*>()(textureSet->GetTexturePath(InteriorTexture));
		// Window maps are per mesh, so interior mapped materials from different meshes must not be shared
		if (const auto windowMapPath = GetInteriorWindowMapPath(textureSet.get(), inputFilePath); !windowMapPath.empty()) {
			hashes.interiorWindowMapHash = RE::BSCRC32<const char*>()(windowMapPath.c_str());
		}
	}

	hashes.baseHash = BSLightingShaderMaterialBase::ComputeCRC32(srcHash);

	return RE::detail::GenerateCRC32({ reinterpret_cast<const std::uint8_t*>(&hashes), sizeof(HashContainer) });
}

RE::BSShaderMaterial::Feature BSLightingShaderMaterialPBR::GetFeature() const
{
	return RE::BSShaderMaterial::Feature::kDefault;
	//return FEATURE;
}

void BSLightingShaderMaterialPBR::ApplyTextureSetData(const TruePBR::PBRTextureSetData& textureSetData)
{
	specularColorScale = textureSetData.roughnessScale;
	specularPower = textureSetData.specularLevel;
	rimLightPower = textureSetData.displacementScale;
	subsurfaceBalance = std::clamp(textureSetData.subsurfaceBalance, 0.f, 1.f);
	pbrFlags.reset(PBRFlags::ThinSurface, PBRFlags::Foliage);
	pbrFlags.set(nifThinSurfaceFlags.get());
	if (textureSetData.thinSurface) {
		pbrFlags.set(PBRFlags::ThinSurface);
	}
	if (textureSetData.foliage) {
		pbrFlags.set(PBRFlags::Foliage);
	}
	thinSurfaceParameters = { std::clamp(textureSetData.thinSurfaceSpecularWeight, 0.f, 1.f),
		std::clamp(textureSetData.thinSurfaceBackSpecularWeight, 0.f, 1.f),
		std::clamp(textureSetData.thinSurfaceBackRoughnessScale, 0.f, 3.f) };

	if (pbrFlags.any(PBRFlags::TwoLayer)) {
		specularColor = textureSetData.coatColor;
		subSurfaceLightRolloff = textureSetData.coatStrength;
		coatRoughness = textureSetData.coatRoughness;
		coatSpecularLevel = textureSetData.coatSpecularLevel;
	} else {
		if (pbrFlags.any(PBRFlags::Subsurface)) {
			specularColor = textureSetData.subsurfaceColor;
			subSurfaceLightRolloff = textureSetData.subsurfaceOpacity;
		}

		if (pbrFlags.any(PBRFlags::Fuzz)) {
			fuzzColor = textureSetData.fuzzColor;
			fuzzWeight = textureSetData.fuzzWeight;
		}
	}

	glintParameters = textureSetData.glintParameters;

	interiorMappingParameters = SanitizeInteriorMappingParameters(textureSetData.interiorMappingParameters);

	if (const auto it = All.find(this); it != All.end()) {
		if (it->second.nifThinSurfaceParameters) {
			ApplyNifThinSurfaceParameters(*it->second.nifThinSurfaceParameters);
		}
		if (it->second.nifInteriorMappingParameters) {
			ApplyNifInteriorMappingParameters(*it->second.nifInteriorMappingParameters);
		}
	}
}

bool BSLightingShaderMaterialPBR::ApplyNifInteriorMappingParameters(std::span<const float> parameters)
{
	if (parameters.size() < 5 || parameters.size() > 7 || std::ranges::any_of(parameters, [](float value) { return !std::isfinite(value); })) {
		return false;
	}

	InteriorMappingParameters nifParameters;
	nifParameters.enabled = interiorMappingParameters.enabled;
	nifParameters.atlasColumns = static_cast<int>(std::round(std::clamp(parameters[0], 1.f, 16.f)));
	nifParameters.atlasRows = static_cast<int>(std::round(std::clamp(parameters[1], 1.f, 16.f)));
	nifParameters.backWallScale = parameters[2];
	nifParameters.depthScale = parameters[3];
	nifParameters.daylightScale = parameters[4];
	// The optional sixth value opts into cubemap rooms; without it the texture set decides
	nifParameters.cubemap = parameters.size() >= 6 ? parameters[5] != 0.f : interiorMappingParameters.cubemap;
	nifParameters.roomSize = parameters.size() >= 7 ? parameters[6] : interiorMappingParameters.roomSize;
	interiorMappingParameters = SanitizeInteriorMappingParameters(nifParameters);

	All[this].nifInteriorMappingParameters = std::vector<float>(parameters.begin(), parameters.end());
	return true;
}

bool BSLightingShaderMaterialPBR::HasInteriorMapping() const
{
	return interiorMappingParameters.enabled || pbrFlags.any(PBRFlags::InteriorMapping);
}

bool BSLightingShaderMaterialPBR::ApplyNifThinSurfaceParameters(std::span<const float> parameters)
{
	if (parameters.size() != 4 || std::ranges::any_of(parameters, [](float value) { return !std::isfinite(value); })) {
		return false;
	}

	subsurfaceBalance = std::clamp(parameters[0], 0.f, 1.f);
	thinSurfaceParameters = { std::clamp(parameters[1], 0.f, 1.f),
		std::clamp(parameters[2], 0.f, 1.f),
		std::clamp(parameters[3], 0.f, 3.f) };
	All[this].nifThinSurfaceParameters = std::array{ subsurfaceBalance, thinSurfaceParameters[0], thinSurfaceParameters[1], thinSurfaceParameters[2] };
	return true;
}

void BSLightingShaderMaterialPBR::ApplyMaterialObjectData(const TruePBR::PBRMaterialObjectData& materialObjectData)
{
	projectedMaterialBaseColorScale = materialObjectData.baseColorScale;
	projectedMaterialRoughness = materialObjectData.roughness;
	projectedMaterialSpecularLevel = materialObjectData.specularLevel;
	projectedMaterialGlintParameters = materialObjectData.glintParameters;
}

void BSLightingShaderMaterialPBR::ClearMaterialObjectData()
{
	projectedMaterialBaseColorScale = { 1.f, 1.f, 1.f };
	projectedMaterialRoughness = 1.f;
	projectedMaterialSpecularLevel = 0.04f;
	projectedMaterialGlintParameters = GlintParameters{};
}

void BSLightingShaderMaterialPBR::OnLoadTextureSet(std::uint64_t arg1, RE::BSTextureSet* inTextureSet)
{
	const auto& stateData = globals::game::graphicsState->GetRuntimeData();

	if (diffuseTexture == nullptr || diffuseTexture == stateData.defaultTextureNormalMap) {
		BSLightingShaderMaterialBase::OnLoadTextureSet(arg1, inTextureSet);

		auto* lock = &unk98;
		while (_InterlockedCompareExchange(lock, 1, 0)) {
			Sleep(0);
		}
		_mm_mfence();

		if (inTextureSet != nullptr) {
			textureSet = RE::NiPointer(inTextureSet);
		}
		DiscardMislinkedTextureSet(textureSet);
		if (textureSet != nullptr) {
			textureSet->SetTexture(RmaosTexture, rmaosTexture);
			textureSet->SetTexture(EmissiveTexture, emissiveTexture);
			textureSet->SetTexture(DisplacementTexture, displacementTexture);
			textureSet->SetTexture(FeaturesTexture0, featuresTexture0);
			textureSet->SetTexture(FeaturesTexture1, featuresTexture1);
			textureSet->SetTexture(InteriorTexture, interiorTexture);
			interiorWindowMapTexture = LoadInteriorWindowMap(textureSet.get(), inputFilePath);
			interiorRoomTexture = IsInteriorRoomPath(textureSet->GetTexturePath(InteriorTexture));

			auto* bgsTextureSet = globals::features::truePBR.currentTextureSet;
			if (bgsTextureSet == nullptr) {
				bgsTextureSet = skyrim_cast<RE::BGSTextureSet*>(inTextureSet);
			}
			if (bgsTextureSet) {
				if (auto* textureSetData = globals::features::truePBR.GetPBRTextureSetData(bgsTextureSet)) {
					ApplyTextureSetData(*textureSetData);
					All[this].textureSetData = textureSetData;
				} else if (const auto it = All.find(this); it != All.end()) {
					it->second.textureSetData = nullptr;
					it->second.nifThinSurfaceParameters.reset();
					it->second.nifInteriorMappingParameters.reset();
				}
			}
		}

		if (lock != nullptr) {
			*lock = 0;
			_mm_mfence();
		}
	}
}

void BSLightingShaderMaterialPBR::ClearTextures()
{
	BSLightingShaderMaterialBase::ClearTextures();
	rmaosTexture.reset();
	emissiveTexture.reset();
	displacementTexture.reset();
	featuresTexture0.reset();
	featuresTexture1.reset();
	interiorTexture.reset();
	interiorWindowMapTexture.reset();
}

void BSLightingShaderMaterialPBR::ReceiveValuesFromRootMaterial(bool skinned, bool rimLighting, bool softLighting, bool backLighting, bool MSN)
{
	BSLightingShaderMaterialBase::ReceiveValuesFromRootMaterial(skinned, rimLighting, softLighting, backLighting, MSN);
	const auto& stateData = globals::game::graphicsState->GetRuntimeData();
	if (rmaosTexture == nullptr) {
		rmaosTexture = stateData.defaultTextureWhite;
	}
	if (emissiveTexture == nullptr) {
		emissiveTexture = stateData.defaultTextureBlack;
	}
	if (displacementTexture == nullptr) {
		displacementTexture = stateData.defaultTextureBlack;
	}
	if (featuresTexture0 == nullptr) {
		featuresTexture0 = stateData.defaultTextureWhite;
	}
	if (featuresTexture1 == nullptr) {
		featuresTexture1 = stateData.defaultTextureWhite;
	}
	if (interiorTexture == nullptr) {
		interiorTexture = stateData.defaultTextureBlack;
	}
}

uint32_t BSLightingShaderMaterialPBR::GetTextures(RE::NiSourceTexture** textures)
{
	uint32_t textureIndex = 0;
	if (diffuseTexture != nullptr) {
		textures[textureIndex++] = diffuseTexture.get();
	}
	if (normalTexture != nullptr) {
		textures[textureIndex++] = normalTexture.get();
	}
	if (rimSoftLightingTexture != nullptr) {
		textures[textureIndex++] = rimSoftLightingTexture.get();
	}
	if (specularBackLightingTexture != nullptr) {
		textures[textureIndex++] = specularBackLightingTexture.get();
	}
	if (rmaosTexture != nullptr) {
		textures[textureIndex++] = rmaosTexture.get();
	}
	if (emissiveTexture != nullptr) {
		textures[textureIndex++] = emissiveTexture.get();
	}
	if (displacementTexture != nullptr) {
		textures[textureIndex++] = displacementTexture.get();
	}
	if (featuresTexture0 != nullptr) {
		textures[textureIndex++] = featuresTexture0.get();
	}
	if (featuresTexture1 != nullptr) {
		textures[textureIndex++] = featuresTexture1.get();
	}
	if (interiorTexture != nullptr) {
		textures[textureIndex++] = interiorTexture.get();
	}
	if (interiorWindowMapTexture != nullptr) {
		textures[textureIndex++] = interiorWindowMapTexture.get();
	}

	return textureIndex;
}

void BSLightingShaderMaterialPBR::LoadBinary(RE::NiStream& stream)
{
	BSLightingShaderMaterialBase::LoadBinary(stream);

	if (loadedWithFeature == RE::BSLightingShaderMaterial::Feature::kMultilayerParallax) {
		std::array<float, 4> parameters;
		stream.iStr->read(parameters.data(), 4);

		coatRoughness = parameters[0];
		coatSpecularLevel = parameters[1];

		fuzzColor = { parameters[0],
			parameters[1],
			parameters[2] };
		fuzzWeight = parameters[3];

		glintParameters.screenSpaceScale = parameters[0];
		glintParameters.logMicrofacetDensity = parameters[1];
		glintParameters.microfacetRoughness = parameters[2];
		glintParameters.densityRandomization = parameters[3];

		if (stream.header.version > 0x4A) {
			float dummy;
			stream.iStr->read(&dummy, 1);
		}
	}
}

float BSLightingShaderMaterialPBR::GetRoughnessScale() const
{
	return specularColorScale;
}

float BSLightingShaderMaterialPBR::GetSpecularLevel() const
{
	return specularPower;
}

float BSLightingShaderMaterialPBR::GetDisplacementScale() const
{
	return rimLightPower;
}

const RE::NiColor& BSLightingShaderMaterialPBR::GetSubsurfaceColor() const
{
	return specularColor;
}

float BSLightingShaderMaterialPBR::GetSubsurfaceOpacity() const
{
	return subSurfaceLightRolloff;
}

float BSLightingShaderMaterialPBR::GetSubsurfaceBalance() const
{
	return subsurfaceBalance;
}

const std::array<float, 3>& BSLightingShaderMaterialPBR::GetThinSurfaceParameters() const
{
	return thinSurfaceParameters;
}

const RE::NiColor& BSLightingShaderMaterialPBR::GetCoatColor() const
{
	return specularColor;
}

float BSLightingShaderMaterialPBR::GetCoatStrength() const
{
	return subSurfaceLightRolloff;
}

float BSLightingShaderMaterialPBR::GetCoatRoughness() const
{
	return coatRoughness;
}

float BSLightingShaderMaterialPBR::GetCoatSpecularLevel() const
{
	return coatSpecularLevel;
}

const std::array<float, 3>& BSLightingShaderMaterialPBR::GetProjectedMaterialBaseColorScale() const
{
	return projectedMaterialBaseColorScale;
}

float BSLightingShaderMaterialPBR::GetProjectedMaterialRoughness() const
{
	return projectedMaterialRoughness;
}

float BSLightingShaderMaterialPBR::GetProjectedMaterialSpecularLevel() const
{
	return projectedMaterialSpecularLevel;
}

const GlintParameters& BSLightingShaderMaterialPBR::GetProjectedMaterialGlintParameters() const
{
	return projectedMaterialGlintParameters;
}

const RE::NiColor& BSLightingShaderMaterialPBR::GetFuzzColor() const
{
	return fuzzColor;
}

float BSLightingShaderMaterialPBR::GetFuzzWeight() const
{
	return fuzzWeight;
}

const GlintParameters& BSLightingShaderMaterialPBR::GetGlintParameters() const
{
	return glintParameters;
}

const InteriorMappingParameters& BSLightingShaderMaterialPBR::GetInteriorMappingParameters() const
{
	return interiorMappingParameters;
}
