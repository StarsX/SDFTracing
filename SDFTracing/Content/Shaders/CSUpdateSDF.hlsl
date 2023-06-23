//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "DecodeVisibility.hlsli"
#include "MonteCarlo.hlsli"
#include "ImpactRange.hlsli"

#define TEMPORAL_FRAME_COUNT 32
#define PERFRAME_SAMPLE_COUNT 32
#define FLT_MAX 3.402823466e+38 // max value

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
	uint lastHitMesh = g_rwIds[DTid];
	bool needUpdate = lastHitMesh;
	bool isLastHitStatic = false;

	if (needUpdate)
	{
		lastHitMesh = DecodeVisibility(lastHitMesh).MeshId;
		needUpdate = g_dynamicMeshIds[lastHitMesh] != 0xffffffff;
		isLastHitStatic = !needUpdate;
	}

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
			bound.Radius += getImpactDistance() * 0.5;

			if (distance(bound.Pos, pos) < bound.Radius)
			{
				needUpdate = true;
				break;
			}
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

	float2 xi;
	//xi = getSampleParam(DTid, gridSize, g_sampleIndex, TEMPORAL_FRAME_COUNT);
	//const uint n = xi.x < 0.75 ? PERFRAME_SAMPLE_COUNT : 1;
	const uint n = PERFRAME_SAMPLE_COUNT;
	float closestSD = (g_sampleIndex % TEMPORAL_FRAME_COUNT) || isLastHitStatic ? g_rwSDF[DTid] : FLT_MAX;
	uint id = 0;
	float2 baryc = 0.0;
	needUpdate = false;

	for (uint i = 0; i < n; ++i)
	{
		xi = getSampleParam(DTid, gridSize, n * g_sampleIndex + i, n * TEMPORAL_FRAME_COUNT);
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
