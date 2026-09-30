struct BasePlacement
{
	uint3 hash;
	float height;
	float2 slope;
};

uint2 BaseGridPosition(uint2 patchPos, uint bladeIndex)
{
	uint patchHash = Random::iqint3(patchPos);
	uint bladeIndexRandomiser = (patchHash >> 16) & 3u;
	uint randomBladeIndex = bladeIndex ^ bladeIndexRandomiser;
	return patchPos * 2u + uint2(randomBladeIndex >> 1u, randomBladeIndex & 1u);
}

float2 BaseQuadrantPosition(uint2 pos, uint3 hash)
{
	float2 jitter = float2(hash.xy) * UINT_TO_FLOAT;
#if defined(FAR_LOD)
	jitter *= 0.5f;
#endif
	return (float2(pos) + jitter) * BLADE_TO_WORLD;
}
