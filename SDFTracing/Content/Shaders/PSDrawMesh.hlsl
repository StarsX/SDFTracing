//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"

#define FLT_MAX 3.402823466e+38 // max value

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos		: SV_POSITION;
	float3	Nrm		: NORMAL;
	float3	PosW	: POSWORLD;
	float2	UV		: TEXCOORD;
	min16float3	Albedo : COLOR;
	min16float3	Emissive : EMISSIVE;
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

//cbuffer cbPerObject
//{
//	float4x3	g_world;
//	float3x3	g_worldIT;
//	uint		g_meshId;
//};

//--------------------------------------------------------------------------------------
// Textures and sampler
//--------------------------------------------------------------------------------------
Texture3D<float> g_txSDF;

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

min16float4 main(PSIn input) : SV_TARGET
{
	//float3 gridSize;
	//g_txSDF.GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	//float3 voxelVec = 2.0 / gridSize;
	//voxelVec = abs(mul(voxelVec, (float3x3)g_volumeWorld));
	//const float voxel = max(max(voxelVec.x, voxelVec.y), voxelVec.z);

	RayDesc ray;
	ray.Origin = input.PosW;
	ray.Direction = normalize(input.Nrm);
	ray.TMin = 1e-3;
	ray.TMax = 100.0;

	const float3 tr = TraceAO(g_txSDF, ray);
	const min16float ambient = PI;
	const min16float ao = min16float(tr.z);
	const min16float3 result = input.Albedo / PI * ambient * ao + input.Emissive;

	return min16float4(result / (result + 0.5), 1.0);
	//return min16float4(input.Albedo + input.Emissive, 1.0);
}
