#pragma once

namespace PGrassCommon
{
	struct Quadrant
	{
		int cellX;
		int cellY;
		uint quadrantX;
		uint quadrantY;
	};
	
	struct alignas(16) QuadrantData
	{
		float2 quadWorldPos;
		uint quadX;
		uint quadY;
	};

	template <std::size_t N>
	struct alignas(16) QuadrantDataArray
	{
		QuadrantData data[N];
	};

	struct alignas(16) GrassGlobals
	{
		float4 color;
		float2 dynamicResolutionInverted;
		float voronoiGridSize;
		float inverseVoronoiGridSize;
		float cameraViewRow0Sum;
		float cameraViewRow1Sum;
		float windSpeed;
		float windTimer;
		float2 windDir;
		float2 pad0;
	};

	struct alignas(16) GrassType
	{
		float height;
		float width;
		float stiffness;
		float tipWeight;
		float mid;
		float minAO;
		float rotationalStiffness;
		float specular;
		float2 minMaxSubsurfaceOpacity;
		float clumpDistanceFactor;
		float clumpHeightFactor;
		float clumpFacingFactor;
		float spatialFreq;
		float phaseOffset;
		float phaseLag;
	};

	struct GrassTypesArray
	{
		GrassType grassType[2];
	};

	struct Blade
	{
		uint positionXY;
		uint positionZFacing;
		uint widthHeight;
		uint hashClumpAndGrassType;
	};
}
