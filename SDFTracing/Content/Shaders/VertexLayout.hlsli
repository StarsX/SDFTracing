//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define HLSL_VERSION 2018
typedef float FLOAT;
#include "D3DX_DXGIFormatConvert.inl"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct Vertex
{
	float3	Pos;
	float3	Nrm;
	float2	UV0;
	float2	UV1;
	float4	Tan;
	uint	Color;
	float	Emissive;
};
