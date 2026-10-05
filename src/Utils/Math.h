#pragma once

#include <algorithm>
#include <cstdint>

namespace Util
{
	/** @brief Deterministic 32-bit hash of two unsigned coordinates. */
	inline uint32_t Hash2D(uint32_t x, uint32_t y)
	{
		x ^= y * 0x9E3779B9u;
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		return x ^ (x >> 16);
	}

	/** @brief Cubic smoothstep; equal edges produce a step at edge0. */
	inline float Smoothstep(float edge0, float edge1, float value)
	{
		if (edge0 == edge1)
			return value < edge0 ? 0.0f : 1.0f;
		const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}
}
