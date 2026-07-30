#pragma once

#include "Buffer.h"

/* @brief Max-depth mip pyramid over the scene depth copy, used to occlusion-cull grass instances. A texel at level N is exactly the farthest depth of everything beneath it */
class HiZPyramid
{
public:
	/**
	 * @brief Rebuilds the pyramid from the current scene depth copy.
	 * @param device Device used to create the per-mip views when the size changes.
	 * @param ctx Immediate context the reduction dispatches are issued on.
	 * @return True when the pyramid is valid for this frame.
	 */
	bool Build(ID3D11Device* device, ID3D11DeviceContext* ctx);

	/* @brief Marks the pyramid unusable for this frame without releasing anything. */
	void Invalidate() { valid = false; }

	/** @brief Returns the full-chain SRV, or nullptr when the pyramid is not valid this frame. */
	ID3D11ShaderResourceView* GetSRV() const { return valid && texture ? texture->srv.get() : nullptr; }
	/** @brief Returns true when Build succeeded for the current frame. */
	bool IsValid() const { return valid; }
	/** @brief Returns the base level width in texels. */
	uint32_t GetWidth() const { return width; }
	/** @brief Returns the base level height in texels. */
	uint32_t GetHeight() const { return height; }
	/** @brief Returns the number of trustworthy mip levels; 1 when the reduction chain is absent. */
	uint32_t GetMipCount() const { return mipCS ? mipCount : 1u; }
	/** @brief Returns how many screen pixels one base-level texel covers. */
	static constexpr uint32_t GetTileSize() { return downsampleFactor; }

	/** @brief Creates the parameter constant buffer. Called from the feature's SetupResources. */
	void SetupResources();
	/** @brief Releases the cached compute shaders so they recompile on next use. */
	void ClearShaderCache();

private:
	struct alignas(16) Params
	{
		uint32_t srcWidth;
		uint32_t srcHeight;
		uint32_t dstWidth;
		uint32_t dstHeight;
	};
	STATIC_ASSERT_ALIGNAS_16(Params);

	/** @brief Returns the SRV of the depth copy that is current at grass draw time. */
	static ID3D11ShaderResourceView* GetSourceDepthSRV();

	/** @brief (Re)creates the pyramid texture and its per-mip views for a new size. */
	bool CreateTexture(ID3D11Device* device, uint32_t dstW, uint32_t dstH);

	std::unique_ptr<Texture2D> texture;
	std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> mipUAVs;
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> mipSRVs;
	std::unique_ptr<ConstantBuffer> paramsCB;

	ID3D11ComputeShader* baseCS = nullptr;
	ID3D11ComputeShader* mipCS = nullptr;

	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mipCount = 1;
	bool valid = false;

	static constexpr uint32_t downsampleFactor = 4;
};
