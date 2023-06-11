//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "DecodeVisibility.hlsli"
#include "ConeTrace.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct LightSource
{
	float4 Min;
	float4 Max;
	float4 Emissive;
	float4x3 World;
};

//--------------------------------------------------------------------------------------
// Texture and buffer
//--------------------------------------------------------------------------------------
RWTexture3D<float4> g_rwIrradiance;

Texture3D<uint> g_txIds		: register (t0, space0);
Texture3D<float> g_txSDF	: register (t2, space0);
StructuredBuffer<LightSource> g_lightSources : register (t3, space0);
Texture3D<float2> g_txBaryc	: register (t4, space0);

[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint encodedId = g_txIds[DTid];
	if (encodedId <= 0)
	{
		uint3 gridSize;
		g_txSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
		if (any(DTid == 0) || any(DTid + 1 >= gridSize))
			g_rwIrradiance[DTid] = float4(0.0.xxx, 1.0);

		return;
	}

	const float2 barycentrics = g_txBaryc[DTid];

	// Decode visibility
	const Visibility vis = DecodeVisibility(encodedId);

	// Fetch vertices
	Vertex vertices[3];
	getVertices(vertices, vis.MeshId, vis.PrimId);

	// Interpolate triangle sample attributes
	const Attrib attrib = interpAttrib(vis.MeshId, vertices, barycentrics);

	float3 gridSize;
	g_txSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
	const float voxel = 2.0 * g_volumeWorld[1].y / gridSize.y;

	const PerObject matrices = g_matrices[attrib.MeshId];

	RayDesc ray;
	ray.Origin = mul(float4(attrib.Pos, 1.0), matrices.World);
	ray.TMin = voxel;
	const float3 N = normalize(mul(attrib.Nrm, (float3x3)matrices.WorldIT));

	min16float3 irradiance = 0.0;

	// Shadow
	uint lightSourceCount, lightSourceStride;
	g_lightSources.GetDimensions(lightSourceCount, lightSourceStride);
	for (uint i = 0; i < lightSourceCount; ++i)
	{
		const LightSource lightSource = g_lightSources[i];
		float4 lightPos = (lightSource.Max + lightSource.Min) * 0.5;
		lightPos.xyz = mul(lightPos, lightSource.World);

		const float3 disp = lightPos.xyz - ray.Origin;
		const float3 L = normalize(disp);

		const min16float NoL = min16float(dot(N, L));

		if (NoL > 0.0)
		{
			const float3 lMin = mul(lightSource.Min, lightSource.World);
			const float3 lMax = mul(lightSource.Max, lightSource.World);
			const float3 lightExt = (lMax.xyz - lMin.xyz) * 0.5;
			const float lMinDim = min(lightExt.x, min(lightExt.y, lightExt.z));
			const float lMaxDim = max(lightExt.x, max(lightExt.y, lightExt.z));
			const float3 lOrient = float3(lightExt <= lMinDim);
			const float coneRadius = abs(dot(lOrient, L)) * lMaxDim;

			ray.Direction = L;
			ray.TMax = length(disp);

			const float3 tr = TraceCone(g_txSDF, ray, coneRadius);
			const min16float3 lightColor = min16float3(lightSource.Emissive.xyz * lightSource.Emissive.w);
			irradiance += NoL * lightColor * min16float(tr.z);
		}
	}

	const min16float3 albedo = attrib.Emissive > 0.0 ? 0.0 : attrib.Color;
	const min16float3 emissive = attrib.Emissive > 0.0 ? attrib.Color * min16float(attrib.Emissive) : 0.0;

	g_rwIrradiance[DTid] = float4(albedo * irradiance + emissive, 1.0);
	//g_rwIrradiance[DTid] = float4(albedo + emissive, 1.0);
}
