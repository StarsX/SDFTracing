//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Compute direction with uniform sphere distribution
//--------------------------------------------------------------------------------------
float3 computeDirectionUS(float2 xi)
{
	const float phi = 2.0 * PI * xi.x;

	// Only near the specular direction according to the roughness for importance sampling
	const float cosTheta = 1.0 - 2.0 * xi.y;;
	const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 computeDirectionHS(float3 normal, float2 xi)
{
	const float3 dir = computeDirectionUS(xi);

	return dir * sign(dot(dir, normal));
}

// Compute local direction first and transform it to world space
float3 computeDirectionCos(float3 normal, float2 xi)
{
	const float3 localDir = computeDirectionUS(xi);

	return normalize(normal + localDir);
}

//--------------------------------------------------------------------------------------
// Get random sample seeds
//--------------------------------------------------------------------------------------
// Quasirandom low-discrepancy sequences
uint Hammersley(uint i)
{
	uint bits = i;
	bits = (bits << 16) | (bits >> 16);
	bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
	bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
	bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
	bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);

	return bits;
}

float2 Hammersley(uint i, uint num)
{
	return float2(i / float(num), Hammersley(i) / float(0x10000));
}

// Morton order generator
uint MortonCode(uint x)
{
	//x &= 0x0000ffff;
	x = (x ^ (x << 8)) & 0x00ff00ff;
	x = (x ^ (x << 4)) & 0x0f0f0f0f;
	x = (x ^ (x << 2)) & 0x33333333;
	x = (x ^ (x << 1)) & 0x55555555;

	return x;
}

uint MortonIndex(uint2 pos)
{
	// Interleaved combination
	return MortonCode(pos.x) | (MortonCode(pos.y) << 1);
}

uint RNG(uint seed)
{
	// Condensed version of pcg_output_rxs_m_xs_32_32
	seed = seed * 747796405 + 1;
	seed = ((seed >> ((seed >> 28) + 4)) ^ seed) * 277803737;
	seed = (seed >> 22) ^ seed;

	return seed;
}

float2 RNG(uint i, uint num)
{
	return float2(i / float(num), (RNG(i) & 0xffff) / float(0x10000));
}

float2 getSampleParam(uint3 index, uint3 dim, uint sampleIdx, uint numSamples = 256)
{
	uint s = index.z * dim.x * dim.y + index.y * dim.x + index.x;
	//uint s = MortonIndex(index);

	s = RNG(s);
	s += sampleIdx;
	s = RNG(s);
	s %= numSamples;

	return RNG(s, numSamples);
	//return Hammersley(s, numSamples);
}

float2 getSampleParam(uint2 index, uint2 dim, uint sampleIdx, uint numSamples = 256)
{
	uint s = index.y * dim.x + index.x;
	//uint s = MortonIndex(index);

	s = RNG(s);
	s += sampleIdx;
	s = RNG(s);
	s %= numSamples;

	return RNG(s, numSamples);
	//return Hammersley(s, numSamples);
}

// Random number [0:1] without sine
#define HASHSCALE1 0.1031
float hash(float p)
{
	float3 p3 = frac(p * HASHSCALE1);
	p3 += dot(p3, p3.yzx + 19.19);

	return frac((p3.x + p3.y) * p3.z);
}
