//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "MonteCarlo.hlsli"

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

	const float2 xi = getSampleParam(DTid, gridSize, g_sampleIndex, VOX_SAMPLE_COUNT);
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
