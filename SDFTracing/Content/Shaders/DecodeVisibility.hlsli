//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "VertexLayout.hlsli"

#define PRIMITIVE_BITS 20

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------
struct Visibility
{
	uint MeshId;
	uint PrimId;
};

struct Attrib
{
	uint MeshId;
	float3	Pos;
	float3	Nrm;
	float2	UV0;
	float2	UV1;
	float4	Tan;
	min16float3 Color;
	min16float Emissive;
};

struct PerObject
{
	float4x3 World;
	float4x3 WorldIT;
};

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
Texture2D<uint> g_txVisibility : register (t0, space0);
StructuredBuffer<PerObject> g_matrices : register (t1, space0);
Buffer<uint> g_indexBuffers[] : register (t0, space1);
StructuredBuffer<Vertex> g_vertexBuffers[] : register (t0, space2);

//--------------------------------------------------------------------------------------
// Decode visibility-buffer values
//--------------------------------------------------------------------------------------
Visibility decodeVisibility(uint v)
{
	Visibility vis;

	--v;
	vis.MeshId = v >> PRIMITIVE_BITS;
	vis.PrimId = v & ((1u << PRIMITIVE_BITS) - 1);

	return vis;
}

//--------------------------------------------------------------------------------------
// Calculate barycentrics
// http://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/
//--------------------------------------------------------------------------------------
float2 calcBarycentrics(float4 p[3], float2 ndc)
{
	const float3 invW = 1.0 / float3(p[0].w, p[1].w, p[2].w);

	const float2 ndc0 = p[0].xy * invW.x;
	const float2 ndc1 = p[1].xy * invW.y;
	const float2 ndc2 = p[2].xy * invW.z;

	const float invDet = 1.0 / determinant(float2x2(ndc2 - ndc1, ndc0 - ndc1));
	const float3 dPdx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * invDet;
	const float3 dPdy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * invDet;

	const float2 deltaVec = ndc - ndc0;
	const float interpInvW = (invW.x + deltaVec.x * dot(invW, dPdx) + deltaVec.y * dot(invW, dPdy));
	const float interpW = 1.0 / interpInvW;

	float2 barycentrics;
	barycentrics.x = interpW * (deltaVec.x * dPdx.y * invW.y + deltaVec.y * dPdy.y * invW.y);
	barycentrics.y = interpW * (deltaVec.x * dPdx.z * invW.z + deltaVec.y * dPdy.z * invW.z);

	return barycentrics;
}

//--------------------------------------------------------------------------------------
// Get triangle vertices
//--------------------------------------------------------------------------------------
void getVertices(out Vertex vertices[3], uint meshIdx, uint primIdx)
{
	const uint baseIdx = primIdx * 3;
	const uint3 indices =
	{
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 1],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 2]
	};

	// Retrieve corresponding vertex normals for the triangle vertices.
	[unroll]
	for (uint i = 0; i < 3; ++i)
		vertices[i] = g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[i]];
}

//--------------------------------------------------------------------------------------
// Interpolate attributes
//--------------------------------------------------------------------------------------
Attrib interpAttrib(uint meshIdx, Vertex vertices[3], float2 barycentrics)
{
	const float3 baryWeights = float3(1.0 - (barycentrics.x + barycentrics.y), barycentrics.xy);

	Attrib attrib;
	attrib.MeshId = meshIdx;

	attrib.Pos =
		baryWeights[0] * vertices[0].Pos +
		baryWeights[1] * vertices[1].Pos +
		baryWeights[2] * vertices[2].Pos;

	attrib.Nrm =
		baryWeights[0] * vertices[0].Nrm +
		baryWeights[1] * vertices[1].Nrm +
		baryWeights[2] * vertices[2].Nrm;

	attrib.UV0 =
		baryWeights[0] * vertices[0].UV0 +
		baryWeights[1] * vertices[1].UV0 +
		baryWeights[2] * vertices[2].UV0;

	attrib.UV1 =
		baryWeights[0] * vertices[0].UV1 +
		baryWeights[1] * vertices[1].UV1 +
		baryWeights[2] * vertices[2].UV1;

	attrib.Tan =
		baryWeights[0] * vertices[0].Tan +
		baryWeights[1] * vertices[1].Tan +
		baryWeights[2] * vertices[2].Tan;

	attrib.Color = min16float3(
		baryWeights[0] * D3DX_R8G8B8A8_UNORM_to_FLOAT4(vertices[0].Color).xyz +
		baryWeights[1] * D3DX_R8G8B8A8_UNORM_to_FLOAT4(vertices[1].Color).xyz +
		baryWeights[2] * D3DX_R8G8B8A8_UNORM_to_FLOAT4(vertices[2].Color).xyz);

	attrib.Emissive = min16float(
		baryWeights[0] * vertices[0].Emissive +
		baryWeights[1] * vertices[1].Emissive +
		baryWeights[2] * vertices[2].Emissive);

	return attrib;
}

//--------------------------------------------------------------------------------------
// Generate a ray in world space for a primary-surface pixel corresponding to an index
// from the dispatched 2D grid.
//--------------------------------------------------------------------------------------
Attrib GetPixelAttrib(uint2 index, float2 uv, matrix viewProj)
{
	const uint visibility = g_txVisibility[index];

	Attrib attrib;
	if (visibility > 0)
	{
		// Decode visibility
		const Visibility vis = decodeVisibility(visibility);

		// Fetch vertices
		Vertex vertices[3];
		getVertices(vertices, vis.MeshId, vis.PrimId);

		// Calculate barycentrics
		float2 screenPos = uv * 2.0 - 1.0;
		screenPos.y = -screenPos.y; // Invert Y for Y-up-style NDC.

		const float4x3 world = g_matrices[vis.MeshId].World;

		float4 p[3];
		[unroll]
		for (uint i = 0; i < 3; ++i)
		{
			p[i].xyz = mul(float4(vertices[i].Pos, 1.0), world);
			p[i].w = 1.0;
			p[i] = mul(p[i], viewProj);
		}
		const float2 barycentrics = calcBarycentrics(p, screenPos);

		// Interpolate triangle sample attributes
		attrib = interpAttrib(vis.MeshId, vertices, barycentrics);
	}
	else
	{
		attrib = (Attrib)0;
		attrib.MeshId = 0xffffffff;
	}

	return attrib;
}

Attrib GetTexelAttrib(float2 uv, uint meshId, uint primId)
{
	// Fetch vertices
	Vertex vertices[3];
	getVertices(vertices, meshId, primId);

	// Calculate barycentrics
	float2 screenPos = uv * 2.0 - 1.0;
	screenPos.y = -screenPos.y; // Invert Y for Y-up-style NDC.

	float4 p[3];
	[unroll]
	for (uint i = 0; i < 3; ++i)
	{
		p[i].xy = vertices[i].UV1 * 2.0 - 1.0;
		p[i].y = -p[i].y; // Invert Y for Y-up-style NDC.
		p[i].zw = float2(0.0, 1.0);
	}
	const float2 barycentrics = calcBarycentrics(p, screenPos);

	Attrib attrib;
	if (any(barycentrics < 0.0) || any(barycentrics > 1.0))
	{
		attrib = (Attrib)0;
		attrib.MeshId = 0xffffffff;
	}
	else
		// Interpolate triangle sample attributes
		attrib = interpAttrib(meshId, vertices, barycentrics);

	return attrib;
}
