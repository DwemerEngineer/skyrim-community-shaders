static const uint QUADRANT_GRASS_PITCH = 17;
static const float QUADRANT_GRASS_SPACING = 2048.0f / 16.0f;

StructuredBuffer<float> QuadrantHeights : register(t3);

// Return bilinear LAND height and slope from the same four corners.
bool SampleLandHeightSlope(out float height, out float2 slope, float2 quadLocalPos, uint quadrant, bool hasLand)
{
	height = 0.0f;
	slope = float2(0.0f, 0.0f);

	if (!hasLand)
		return false;

	float2 gridPosition = clamp(quadLocalPos / QUADRANT_GRASS_SPACING, 0.0f, QUADRANT_GRASS_PITCH - 1.001f);
	int2 baseSample = int2(gridPosition);
	float2 sampleFraction = gridPosition - baseSample;

	uint quadrantBase = quadrant * (QUADRANT_GRASS_PITCH * QUADRANT_GRASS_PITCH);
	uint lowerLeftIndex = quadrantBase + baseSample.y * QUADRANT_GRASS_PITCH + baseSample.x;
	float heightLowerLeft = QuadrantHeights[lowerLeftIndex];
	float heightLowerRight = QuadrantHeights[lowerLeftIndex + 1];
	float heightUpperLeft = QuadrantHeights[lowerLeftIndex + QUADRANT_GRASS_PITCH];
	float heightUpperRight = QuadrantHeights[lowerLeftIndex + QUADRANT_GRASS_PITCH + 1];

	height = lerp(lerp(heightLowerLeft, heightLowerRight, sampleFraction.x), lerp(heightUpperLeft, heightUpperRight, sampleFraction.x), sampleFraction.y);
	slope = float2(
				lerp(heightLowerRight - heightLowerLeft, heightUpperRight - heightUpperLeft, sampleFraction.y),
				lerp(heightUpperLeft - heightLowerLeft, heightUpperRight - heightLowerRight, sampleFraction.x)) *
	        (1.0f / QUADRANT_GRASS_SPACING);

	return true;
}
