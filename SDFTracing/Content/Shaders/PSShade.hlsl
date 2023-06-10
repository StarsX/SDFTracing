//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "DecodeVisibility.hlsli"
#include "ConeTrace.hlsli"

#ifndef _LIT_INDIRECT_
#define _LIT_INDIRECT_ 1
#endif

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos	: SV_POSITION;
	float2	UV : TEXCOORD;
};

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
Texture3D<float> g_txSDF : register (t2, space0);
StructuredBuffer<LightSource> g_lightSources : register (t3, space0);
Texture3D g_txIrradiance : register (t4, space0);

min16float4 main(PSIn input) : SV_TARGET
{
	const Attrib attrib = GetPixelAttrib(input.Pos.xy, input.UV, g_viewProj);
	if (attrib.MeshId == 0xffffffff) discard;

	float3 gridSize;
	g_txSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
	const float voxel = 2.0 * length(g_volumeWorld[1]) / gridSize.y;

	const PerObject matrices = g_matrices[attrib.MeshId];

	RayDesc ray;
	ray.Origin = mul(float4(attrib.Pos, 1.0), matrices.World);
	ray.TMin = voxel;
	const float3 N = normalize(mul(attrib.Nrm, (float3x3)matrices.WorldIT));

#if 0
	// Test irradiance map
	float3 pos = mul(float4(ray.Origin, 1.0), g_volumeWorldI);
	const float3 uvw = pos * 0.5 + 0.5;
	float3 irr = g_txIrradiance.SampleLevel(g_sampler, uvw, 0.0).xyz;
	return min16float4(irr / (irr + 0.5), 1.0);
#endif

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

	// AO
	ray.Direction = N;
	ray.TMin = voxel;
	ray.TMax = length(g_volumeWorld[1]) * 0.4;

#if _LIT_INDIRECT_
	irradiance += TraceIndirect(g_txSDF, g_txIrradiance, ray, 2.25).xyz;
#else
	const float3 tr = TraceAO(g_txSDF, ray, 2.25);
	const float4 ambient = g_txIrradiance.SampleLevel(g_sampler, 0.5, 16.0);
	irradiance += min16float3(ambient.xyz / ambient.w) * min16float(tr.z);
#endif

	const min16float3 albedo = attrib.Emissive > 0.0 ? 0.0 : attrib.Color;
	const min16float3 emissive = attrib.Emissive > 0.0 ? attrib.Color * min16float(attrib.Emissive) : 0.0;
	const min16float3 radiance = albedo / PI * irradiance + emissive;

	return min16float4(radiance / (radiance + 0.5), 1.0);
	//return min16float4(albedo + emissive, 1.0);
}
