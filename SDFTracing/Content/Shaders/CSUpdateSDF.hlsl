//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "DecodeVisibility.hlsli"
#include "MonteCarlo.hlsli"
#include "ImpactRange.hlsli"

#define SAMPLE_COUNT 128

typedef RaytracingAccelerationStructure RaytracingAS;
typedef BuiltInTriangleIntersectionAttributes TriAttributes;

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
struct DynamicMesh
{
	uint MeshId;
};

struct Bound
{
	float3 Pos;
	float Radius;
};

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
RWTexture3D<float> g_rwSDF		: register (u0);
RWTexture3D<uint> g_rwIds		: register (u1);
RWTexture3D<float2> g_rwBaryc	: register (u2);

// TLAS
RaytracingAS g_scene : register (t0);

// Mesh info buffers
StructuredBuffer<uint> g_dynamicMeshIds : register (t2);
StructuredBuffer<DynamicMesh> g_dynamicMeshes : register (t3);
StructuredBuffer<Bound> g_bounds : register (t4);

//SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Generate SDF
//--------------------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	// Impact by the dynamic meshes of the last frame
	const uint lastHitMesh = g_rwIds[DTid];
	bool needUpdate = g_dynamicMeshIds[lastHitMesh] != 0xffffffff;

	uint3 gridSize;
	g_rwSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
	const float3 uvw = (DTid + 0.5) / gridSize;
	const float3 pos = mul(float4(uvw * 2.0 - 1.0, 1.0), g_world);

	if (!needUpdate)
	{
		// Impact by the dynamic meshes of the current frame
		uint dynamicMeshCount, stride;
		g_dynamicMeshes.GetDimensions(dynamicMeshCount, stride);
		for (uint i = 0; i < dynamicMeshCount; ++i)
		{
			const DynamicMesh dynamicMesh = g_dynamicMeshes[i];
			Bound bound = g_bounds[dynamicMesh.MeshId];
			const PerObject matrices = g_matrices[dynamicMesh.MeshId];

			// Generate AABB
			float3 minAABB = bound.Pos - bound.Radius;
			float3 maxAABB = bound.Pos + bound.Radius;

			// To world space
			minAABB = mul(float4(minAABB, 1.0), matrices.World);
			maxAABB = mul(float4(maxAABB, 1.0), matrices.World);

			// Sphere in world space
			const float3 radii = (maxAABB - minAABB) * 0.5;
			bound.Pos = (minAABB + maxAABB) * 0.5;
			bound.Radius = max(radii.x, max(radii.y, radii.z));
			bound.Radius += getImpactDistance();

			needUpdate = distance(bound.Pos, pos) <= bound.Radius;
		}
	}

	if (!needUpdate) return;

	// Instantiate ray query object.
	// Template parameter allows driver to generate a specialized
	// implementation.
	RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	RayDesc ray;
	ray.TMin = 0.0;
	ray.TMax = 100.0;
	ray.Origin = pos;

	float2 xi = getSampleParam(DTid, gridSize, g_sampleIndex, SAMPLE_COUNT);
	//const uint sampleCount = xi.x < 0.75 ? SAMPLE_COUNT : 1;
	const uint sampleCount = SAMPLE_COUNT;
	float closestSD = sampleCount > 1 ? 10000.0 : g_rwSDF[DTid];
	uint id = 0;
	float2 baryc = 0.0;
	needUpdate = false;

	for (uint i = 0; i < sampleCount; ++i)
	{
		xi = getSampleParam(DTid, gridSize, g_sampleIndex * sampleCount + i, sampleCount);
		ray.Direction = computeDirectionUS(xi);

		q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0, ray);

		// Execute inline ray tracing (ray query)
		q.Proceed();

		if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			const float dist = q.CommittedRayT();
			const float signedDist = q.CommittedTriangleFrontFace() ? dist : -dist;

			if (dist < abs(closestSD))
			{
				needUpdate = true;
				closestSD = signedDist;
				id = ((q.CommittedInstanceIndex() << PRIMITIVE_BITS) | q.CommittedPrimitiveIndex()) + 1;
				baryc = q.CommittedTriangleBarycentrics();
			}
		}
	}

	if (needUpdate)
	{
		g_rwSDF[DTid] = closestSD;

		const float voxel = 2.0 * length(g_world[1]) / gridSize.y;
		if (abs(closestSD) < voxel * 0.5 * sqrt(2.0))
		{
			g_rwIds[DTid] = id;
			g_rwBaryc[DTid] = baryc;
		}
	}
}
