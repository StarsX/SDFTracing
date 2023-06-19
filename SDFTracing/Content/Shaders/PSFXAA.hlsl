//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FXAA.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PS_Input
{
	float4 Pos : SV_POSITION;
	float2 UV : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture2D g_txImage;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Pixel shader for FXAA
//--------------------------------------------------------------------------------------
min16float4 main(const PS_Input input) : SV_TARGET
{
	float2 texSize;
	g_txImage.GetDimensions(texSize.x, texSize.y);

	const FxaaTex fxaaTex = { g_sampler, g_txImage };

	return min16float4(FxaaPixelShader(input.UV, fxaaTex, 1.0 / texSize), 1.0);
}