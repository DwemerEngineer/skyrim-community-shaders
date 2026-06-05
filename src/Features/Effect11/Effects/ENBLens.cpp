#include "ENBLens.h"

#include "../TextureManager.h"

void ENBLens::Execute()
{
	// Get common textures for input/output
	auto& textureManager = TextureManager::GetSingleton();

	auto textureHDRTemp = textureManager.GetCommonTexture("TextureHDRTemp");
	auto textureLens = textureManager.GetCommonTexture("TextureLens");

	if (!textureHDRTemp || !textureLens) {
		return;
	}

	// Set dowsampled texture, typically the one used (use 1024x1024 mip)
	auto downsampledInputSRV = TextureManager::GetSingleton().GetDownsampleTexture();

	if (!downsampledInputSRV) {
		return;
	}

	auto [executed, inOutput] = ExecuteTechniqueSequence(GetSelectedTechnique(), downsampledInputSRV, *textureLens, *textureHDRTemp);

	if (executed && !inOutput) {
		textureManager.SwapTextures("TextureLens", "TextureHDRTemp");
	}
}

void ENBLens::UpdateEffectVariables()
{
	if (!effect)
		return;

	SetShaderResourceVariable("TextureDownsampled", TextureManager::GetSingleton().GetDownsampleTexture());
	SetShaderResourceVariable("TextureOriginal", globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV);

	auto maskSRV = LoadTextureFromFile("enblensmask.png");
	if (!maskSRV)
		maskSRV = LoadTextureFromFile("enblensmask.bmp");
	if (maskSRV)
		SetShaderResourceVariable("TextureMask", maskSRV);

	// DX9 compat: bind bloom textures for lens passes
	if (isDX9Effect) {
		auto downsampledSRV = TextureManager::GetSingleton().GetDownsampleTexture();
		for (int i = 1; i <= 8; ++i)
			SetShaderResourceVariable("texBloom" + std::to_string(i), downsampledSRV);
	}
}