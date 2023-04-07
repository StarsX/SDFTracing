//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"

typedef RaytracingAccelerationStructure RaytracingAS;
typedef BuiltInTriangleIntersectionAttributes TriAttributes;

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	matrix g_viewProj;
	float4x3 g_world;
	float4x3 g_worldI;
	uint g_sampleIndex;
};

//--------------------------------------------------------------------------------------
// Textures and buffers
//--------------------------------------------------------------------------------------
// Texture
RWTexture3D<float> g_rwSDF : register (u0);

// TLAS
RaytracingAS g_scene : register (t0);

//SamplerState g_sampler;

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

float2 getSampleParam(uint3 index, uint3 dim, uint numSamples = 256)
{
	uint s = index.z * dim.x * dim.y + index.y * dim.x + index.x;
	//uint s = MortonIndex(index);

	s = RNG(s);
	s += g_sampleIndex;
	s = RNG(s);
	s %= numSamples;

	return RNG(s, numSamples);
	//return Hammersley(s, numSamples);
}

//--------------------------------------------------------------------------------------
// Generate SDF
//--------------------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint3 gridSize;
	g_rwSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	// Instantiate ray query object.
	// Template parameter allows driver to generate a specialized
	// implementation.
	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	RayDesc ray;
	ray.TMin = 0.0;
	ray.TMax = 100.0;

	const float3 uvw = (DTid + 0.5) / gridSize;
	const float3 pos = uvw * 2.0 - 1.0;
	ray.Origin = mul(float4(pos, 1.0), g_world);

	const float2 xi = getSampleParam(DTid, gridSize, VOX_SAMPLE_COUNT);
	ray.Direction = computeDirectionUS(xi);

	q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0, ray);

	// Execute inline ray tracing (ray query)
	q.Proceed();

	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		const float closestSD = g_rwSDF[DTid];

		float signedDist = q.CommittedRayT();
		signedDist = q.CommittedTriangleFrontFace() ? signedDist : -signedDist;

		g_rwSDF[DTid] = abs(signedDist) < abs(closestSD) ? signedDist : closestSD;
	}
}
