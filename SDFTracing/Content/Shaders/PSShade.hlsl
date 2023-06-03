//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "DecodeVisibility.hlsli"
#include "ConeTrace.hlsli"

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
Texture3D<float3> g_txIrradiance : register (t4, space0);

min16float4 main(PSIn input) : SV_TARGET
{
	const Attrib attrib = GetPixelAttrib(input.Pos.xy, input.UV, g_viewProj);
	if (attrib.MeshId == 0xffffffff) discard;

	float3 gridSize;
	g_txSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
	const float voxel = 2.0 * g_volumeWorld[1].y / gridSize.y;

	const PerObject matrices = g_matrices[attrib.MeshId];

	RayDesc ray;
	ray.Origin = mul(float4(attrib.Pos, 1.0), matrices.World);
	ray.TMin = voxel;
	const float3 N = normalize(mul(attrib.Nrm, (float3x3)matrices.WorldIT));

	min16float3 radiance = 0.0;

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
			radiance += NoL * lightColor * min16float(tr.z);
		}
	}

	// AO
	ray.Direction = N;
	ray.TMin = 0.0;
	ray.TMax = g_volumeWorld[1].y * 0.4;

	const float3 tr = TraceAO(g_txSDF, ray, 2.25);

	const min16float3 albedo = attrib.Emissive > 0.0 ? 0.0 : attrib.Color;
	const min16float3 emissive = attrib.Emissive > 0.0 ? attrib.Color * min16float(attrib.Emissive) : 0.0;

	const min16float3 ambient = PI;
	radiance += ambient * min16float(tr.z);
	//radiance += TraceIndirect(g_txSDF, g_txIrradiance, ray, 2.25).xyz;
	const min16float3 result = albedo / PI * radiance + emissive;

	return min16float4(result / (result + 0.5), 1.0);
	//return min16float4(input.Albedo + input.Emissive, 1.0);
}
