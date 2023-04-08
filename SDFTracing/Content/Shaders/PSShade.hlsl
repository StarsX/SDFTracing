//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "DecodeVisibility.hlsli"
#include "MonteCarlo.hlsli"

#define FLT_MAX 3.402823466e+38 // max value

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos	: SV_POSITION;
	float2	UV : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	matrix g_viewProj;
	float4x3 g_volumeWorld;
	float4x3 g_volumeWorldI;
};

//--------------------------------------------------------------------------------------
// Texture and sampler
//--------------------------------------------------------------------------------------
Texture3D<float> g_txSDF : register (t2, space0);

SamplerState g_sampler;

float3 TraceAO(Texture3D<float> txSDF, RayDesc ray)
{
	float t = ray.TMin, r = 0.0;
	float occ = 0.0, sca = 1.0;
	for (uint i = 0; i < 5 && t < ray.TMax; ++i)
	{
		float3 pos = ray.Origin + t * ray.Direction;
		pos = mul(float4(pos, 1.0), g_volumeWorldI);

		if (any(abs(pos) > 1.0)) break;

		const float3 uvw = pos * 0.5 + 0.5;
		const float r = txSDF.SampleLevel(g_sampler, uvw, 0.0);

		occ += (t - max(r, 0.0)) * sca;
		sca *= 0.97;
		if (occ >= 1.0 / 1.5) break;

		t += max(r * 0.5, 0.0625);
	}

	const float ao = saturate(1.0 - 1.5 * occ) * (0.5 * ray.Direction.y + 0.5);

	return float3(t, r, ao);
}

//--------------------------------------------------------------------------------------
// https://www.shadertoy.com/view/4sdGWN
//--------------------------------------------------------------------------------------
float3 TraceAO(Texture3D<float> txSDF, RayDesc ray, float falloff)
{
	const uint n = 32;
	const float nInv = 1.0 / n;
	const float rad = 1.0 - nInv; // Hemispherical factor (self occlusion correction)
	const float lMax = ray.TMax - ray.TMin;

	float t = ray.TMin, r = 0.0;
	float occ = 0.0;
	for (uint i = 0; i < n; ++i)
	{
		t = ray.TMin + hash(i) * lMax;
		const float2 xi = float2(hash(t + 1.0), hash(t + 2.0));
		const float3 dir = normalize(ray.Direction + computeDirectionHS(ray.Direction, xi) * rad); // mix direction with the normal

		float3 pos = ray.Origin + t * dir;
		pos = mul(float4(pos, 1.0), g_volumeWorldI);

		const float3 uvw = pos * 0.5 + 0.5;
		const float r = txSDF.SampleLevel(g_sampler, uvw, 0.0);

		occ += (t - max(r, 0.0)) / lMax * falloff;
	}

	const float ao = saturate(1.0 - occ * nInv);

	return float3(t, r, ao);
}

min16float4 main(PSIn input) : SV_TARGET
{
	const Attrib attrib = GetPixelAttrib(input.Pos.xy, input.UV, g_viewProj);
	if (attrib.MeshId == 0xffffffff) discard;

	//float3 gridSize;
	//g_txSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	//float3 voxelVec = 2.0 / gridSize;
	//voxelVec = abs(mul(voxelVec, (float3x3)g_volumeWorld));
	//const float voxel = max(max(voxelVec.x, voxelVec.y), voxelVec.z);

	const PerObject matrices = g_matrices[attrib.MeshId];

	RayDesc ray;
	ray.Origin = mul(float4(attrib.Pos, 1.0), matrices.World);
	ray.Direction = normalize(mul(attrib.Nrm, (float3x3)matrices.WorldIT));
#if 0
	ray.TMin = 1e-3;
	ray.TMax = 100.0;

	const float3 tr = TraceAO(g_txSDF, ray);
#else
	ray.TMin = 0.0;
	ray.TMax = g_volumeWorld[1].y * 0.4;

	const float3 tr = TraceAO(g_txSDF, ray, 2.2);
#endif

	const min16float ambient = PI * 2.0;
	const min16float3 albedo = attrib.Emissive > 0.0 ? 0.0 : attrib.Color;
	const min16float3 emissive = attrib.Emissive > 0.0 ? attrib.Color * min16float(attrib.Emissive) : 0.0;
	const min16float ao = min16float(tr.z);
	const min16float3 result = albedo / PI * ambient * ao + emissive;

	return min16float4(result / (result + 0.5), 1.0);
	//return min16float4(input.Albedo + input.Emissive, 1.0);
}