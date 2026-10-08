#include "Features/ProceduralGrass.h"

#include "Globals.h"
#include "TopDownOcclusion.h"
#include "Utils/Format.h"

namespace
{
	std::string ObjectTextureKey(std::string_view path)
	{
		std::string key = Util::FixFilePath(std::string(path));
		if (key.starts_with("data/"))
			key.erase(0, 5);
		if (key.starts_with("textures/"))
			key.erase(0, 9);
		return key;
	}
}

void ProceduralGrass::RebuildObjectGrassRules()
{
	objectGrassTextureRules.clear();
	for (const auto& [path, rule] : settings.objectGrassTextures)
		objectGrassTextureRules.insert_or_assign(ObjectTextureKey(path), rule);
	globals::topDownOcclusion->Invalidate();
}

const ProceduralGrass::Settings::ObjectGrassRule* ProceduralGrass::GetObjectGrassRule(std::string_view texturePath) const
{
	const auto found = objectGrassTextureRules.find(ObjectTextureKey(texturePath));
	return found != objectGrassTextureRules.end() ? &found->second : nullptr;
}

uint8_t ProceduralGrass::GetObjectGrassType(const Settings::ObjectGrassRule& rule) const
{
	if (rule.LandTexture.empty())
		return 1u;
	const auto found = textureSelection.find(rule.LandTexture);
	if (found == textureSelection.end() || rule.Variant >= found->second.ids.size())
		return 0u;
	return found->second.ids[rule.Variant];
}
