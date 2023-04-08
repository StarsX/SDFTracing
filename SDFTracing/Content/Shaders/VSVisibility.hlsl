//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "VertexLayout.hlsli"

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
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
	float4x4 g_viewProj;
};

//--------------------------------------------------------------------------------------
// Buffers
//--------------------------------------------------------------------------------------
StructuredBuffer<PerObject> g_matrices;
StructuredBuffer<Vertex> g_vertexBuffers[];

float4 main(uint vid : SV_VERTEXID) : SV_POSITION
{
	const Vertex vertex = g_vertexBuffers[g_meshId][vid];
	float4 pos = float4(vertex.Pos, 1.0);
	pos.xyz = mul(pos, g_matrices[g_meshId].World);

	return mul(pos, g_viewProj);
}
