#ifndef __GRASS_INSTANCE_DEPENDENCY_HLSL__
#define __GRASS_INSTANCE_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"

namespace GrassInstance
{
	static const uint Stride = 32;
	static const uint RecordStride = 16;

	// The packed word contains a 19-bit group index, 9-bit fade and 4-bit flags.
	static const uint RecordGroupBits = 19;
	static const uint RecordFadeBits = 9;
	static const uint RecordGroupMask = (1u << RecordGroupBits) - 1;
	static const uint RecordFadeMask = (1u << RecordFadeBits) - 1;
	static const uint RecordFlagsShift = RecordGroupBits + RecordFadeBits;

	// The flags contain collision range, simple shading and a two-bit LOD tier (0 = full, 1 = middle, 2 = far).
	static const uint FlagCollision = 1;
	static const uint FlagSimpleShading = 2;
	static const uint FlagTierShift = 2;

	// Each group contains up to 64 contiguous instances from one slice
	struct CullGroup
	{
		float3 Origin;
		float FadeStart;
		uint SourceStart;
		uint Count;
	};

	// This group count selects the engine's instance stream.
	static const uint VanillaDrawMarker = 0xFFFFFFFF;

	uint PackRecord(uint groupIndex, float fade, uint flags)
	{
		return groupIndex | ((uint)round(saturate(fade) * RecordFadeMask) << RecordGroupBits) | (flags << RecordFlagsShift);
	}

	void UnpackRecord(uint packed, out uint groupIndex, out float fade, out uint flags)
	{
		groupIndex = packed & RecordGroupMask;
		fade = (float)((packed >> RecordGroupBits) & RecordFadeMask) / RecordFadeMask;
		flags = packed >> RecordFlagsShift;
	}

	float WindScalar(float2 localXY, float timer)
	{
		const float phase = 0.4 * ((localXY.x + localXY.y) * -0.0078125 + timer);
		float sine, cosine;
		sincos(phase, sine, cosine);
		const float cosineTerm = 0.2 * cos(Math::PI * cosine);
		const float sineTerm = sin(Math::PI * sine);
		const float harmonicTerm = sin(Math::TAU * sine);
		return (sineTerm + harmonicTerm) * 0.3 + cosineTerm;
	}

	// Decode the engine's two half-packed instance vectors.
	void Decode(uint4 first, uint4 second, out float4 data1, out float4 data2, out float4 data3, out float4 data4)
	{
		data1 = float4(f16tof32(first.x), f16tof32(first.x >> 16), f16tof32(first.y), f16tof32(first.y >> 16));
		data2 = float4(f16tof32(first.z), f16tof32(first.z >> 16), f16tof32(first.w), f16tof32(first.w >> 16));
		data3 = float4(f16tof32(second.x), f16tof32(second.x >> 16), f16tof32(second.y), f16tof32(second.y >> 16));
		data4 = float4(f16tof32(second.z), f16tof32(second.z >> 16), f16tof32(second.w), f16tof32(second.w >> 16));
	}

}

#endif  // __GRASS_INSTANCE_DEPENDENCY_HLSL__
