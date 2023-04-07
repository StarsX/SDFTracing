//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConst.h"
#include "VertexLayout.hlsli"

//--------------------------------------------------------------------------------------
// Struct
//--------------------------------------------------------------------------------------
struct VSOut
{
	float4	Pos		: SV_POSITION;
	float3	Nrm		: NORMAL;
	float3	PosW	: POSWORLD;
	float2	UV		: TEXCOORD;
	min16float3	Albedo : COLOR;
	min16float3	Emissive : EMISSIVE;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	float4x4 g_viewProj;
};

cbuffer cbPerObject
{
	float4x3	g_world;
	float3x3	g_worldIT;
	uint		g_meshId;
};

//--------------------------------------------------------------------------------------
// IA buffers
//--------------------------------------------------------------------------------------
StructuredBuffer<Vertex> g_vertexBuffers[];

VSOut main(uint vid : SV_VERTEXID)
{
	const Vertex vertex = g_vertexBuffers[g_meshId][vid];

	VSOut output;
	output.PosW = mul(float4(vertex.Pos, 1.0), g_world);
	output.Pos = mul(float4(output.PosW, 1.0), g_viewProj);
	output.Nrm = mul(vertex.Nrm, g_worldIT);
	output.UV = vertex.UV;

	const min16float3 color = min16float3(D3DX_R8G8B8A8_UNORM_to_FLOAT4(vertex.Color).xyz);
	output.Albedo = vertex.Emissive > 0.0 ? 0.0 : color;
	output.Emissive = vertex.Emissive > 0.0 ? color * min16float(vertex.Emissive) : 0.0;

	return output;
}
