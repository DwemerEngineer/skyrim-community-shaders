#include "ENBBloom.h"

#include "../TextureManager.h"

void ENBBloom::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto textureA = textureManager.GetCommonTexture("TextureBloom");
	auto textureB = textureManager.GetCommonTexture("TextureBloomTemp");

	if (!textureA || !textureB)
		return;

	auto downsampledInputSRV = TextureManager::GetSingleton().GetDownsampleTexture();
	if (!downsampledInputSRV)
		return;

	bool isDX9Bloom = techniques.contains("BloomPrePass") &&
	                  techniques.contains("BloomTexture1") &&
	                  techniques.contains("BloomTexture2") &&
	                  techniques.contains("BloomPostPass");

	if (isDX9Bloom) {
		auto bindBloomInputs = [&](ID3D11ShaderResourceView* srv) {
			for (int i = 1; i <= 8; ++i)
				SetShaderResourceVariable("texBloom" + std::to_string(i), srv);
		};

		// TempParameters: xy=0 (no DX9 half-pixel offset in DX11), z=texel size, w=1+passNumber
		constexpr float texelSize = 1.0f / 1024.0f;

		// Pass 0: BloomPrePass — downsample/threshold
		float4 tempParams = { 0.0f, 0.0f, texelSize, 1.0f };
		SetVectorVariable("TempParameters", &tempParams, sizeof(tempParams));
		bindBloomInputs(downsampledInputSRV);
		ExecuteTechnique("BloomPrePass", *textureA);

		for (int cycle = 0; cycle < 4; ++cycle) {
			float passW = static_cast<float>(1 + cycle);

			tempParams = { 0.0f, 0.0f, texelSize, passW };
			SetVectorVariable("TempParameters", &tempParams, sizeof(tempParams));
			bindBloomInputs(textureA->srv.get());
			ExecuteTechnique("BloomTexture1", *textureB);

			tempParams.w = passW + 0.5f;
			SetVectorVariable("TempParameters", &tempParams, sizeof(tempParams));
			bindBloomInputs(textureB->srv.get());
			ExecuteTechnique("BloomTexture2", *textureA);
		}

		// Final pass: BloomPostPass
		tempParams = { 0.0f, 0.0f, texelSize, 1.0f };
		SetVectorVariable("TempParameters", &tempParams, sizeof(tempParams));
		bindBloomInputs(textureA->srv.get());
		ExecuteTechnique("BloomPostPass", *textureB);

		textureManager.SwapTextures("TextureBloom", "TextureBloomTemp");
	} else {
		auto [executed, inOutput] = ExecuteTechniqueSequence(GetSelectedTechnique(), downsampledInputSRV, *textureA, *textureB);
		if (executed && !inOutput)
			textureManager.SwapTextures("TextureBloom", "TextureBloomTemp");
	}
}

void ENBBloom::UpdateEffectVariables()
{
	auto downsampledSRV = TextureManager::GetSingleton().GetDownsampleTexture();

	SetShaderResourceVariable("TextureDownsampled", downsampledSRV);
	SetShaderResourceVariable("TextureOriginal", globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV);

	if (isDX9Effect) {
		for (int i = 1; i <= 8; ++i)
			SetShaderResourceVariable("texBloom" + std::to_string(i), downsampledSRV);
	}
}
