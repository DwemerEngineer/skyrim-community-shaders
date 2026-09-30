// Eight cells per quadrant plus the nearest-feature border at grid sizes of 256 or more.
#define CLUMP_FEATURE_PITCH 12

static const uint WORK_HAS_LAND = 1u << 16u;

struct ClumpFeature
{
	float2 fraction;
	uint random;
};

struct PackedClumpFeature
{
	uint fraction;
	uint random;
};

struct QuadrantData
{
	float2 quadWorldPos;
	uint quadrantHash;
	uint flags;
};

cbuffer QuadrantData : register(b7)
{
	float4 lodFadeIn;  // x: fade-in start, y: inverse range, z: Far seam-fill retention, w: fade-out endpoint
	float4 lodFadeOut;
	QuadrantData data[QUADRANT_DATA_SIZE];
}
