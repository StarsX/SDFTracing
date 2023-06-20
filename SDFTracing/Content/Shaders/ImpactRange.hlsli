//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen and Yang, Jiale. All rights reserved.
//--------------------------------------------------------------------------------------

#define DISTANCE_FILTER_MODEL 3

bool angle_filter(float NoL, float threshold = 0.5)
{
	return NoL > threshold;
}

float getImpactDistance(float attenuation = -1.0)
{
	const float c = 1.0;
	const float linear_c = 1.0;
	const float quadratic_c = 1.0;

	if (attenuation < 0.0)
	{
#if DISTANCE_FILTER_MODEL == 1
		attenuation = 0.1667;
#elif DISTANCE_FILTER_MODEL == 2
		attenuation = 0.0323;
#else
		attenuation = 0.0067;
#endif
	}

#if DISTANCE_FILTER_MODEL == 1
	const float dis = (1.0 / attenuation - c) / linear_c;
#elif DISTANCE_FILTER_MODEL == 2
	const float delta = linear_c * linear_c - 4.0 * quadratic_c * (c - 1.0 / attenuation);
	// delta is larger than 0
	const float dis = (delta - linear_c) / (2.0 * quadratic_c);
#else
	const float dis = -log(attenuation) / c;
#endif

	return dis;
}

bool distance_filter(float dis)
{
	return dis < getImpactDistance();
}

bool HasImpact(float NoL, float dis)
{
	return angle_filter(NoL) && distance_filter(dis);
}

//--------------------------------------------------------------------------------------
// Fast tile bit set
//--------------------------------------------------------------------------------------
uint fastTileBitSet(RWTexture2DArray<uint> rwTileMap, uint3 index, uint value)
{
	uint originalVal;

	//const uint4 m = WaveMatch(uint4(index, value));
	//if (firstbitlow(m.x) == WaveGetLaneIndex()) InterlockedOr(rwTileMap[index], value, originalVal);
	InterlockedOr(rwTileMap[index], value, originalVal);

	return originalVal;
}
