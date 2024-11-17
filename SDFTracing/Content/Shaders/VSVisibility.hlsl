//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "VertexLayout.hlsli"

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
struct VSOut
{
	float4 Pos	: SV_POSITION;
	float2 UV	: TEXCOORD;
};

struct PerObject
{
	float4x3 World;
	float4x3 WorldIT;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	uint g_meshId;
};

cbuffer cbPerFrame
{
	matrix g_viewProj;
};

//--------------------------------------------------------------------------------------
// Buffers
//--------------------------------------------------------------------------------------
StructuredBuffer<PerObject> g_matrices		: register (t0, space0);
StructuredBuffer<Vertex> g_vertexBuffers[]	: register (t0, space1);

VSOut main(uint vid : SV_VERTEXID)
{
	VSOut output;

	const Vertex vertex = g_vertexBuffers[g_meshId][vid];
	float4 pos = float4(vertex.Pos, 1.0);
	pos.xyz = mul(pos, g_matrices[g_meshId].World);

	output.Pos = mul(pos, g_viewProj);
	output.UV = vertex.UV0;

	return output;
}
