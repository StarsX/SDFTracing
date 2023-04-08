//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "DecodeVisibility.hlsli"

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	uint g_meshId;
};

//--------------------------------------------------------------------------------------
// Base visiblity-buffer pass
//--------------------------------------------------------------------------------------
uint main(uint primitiveId : SV_PrimitiveID) : SV_TARGET
{
	return ((g_meshId << PRIMITIVE_BITS) | primitiveId) + 1;
}
