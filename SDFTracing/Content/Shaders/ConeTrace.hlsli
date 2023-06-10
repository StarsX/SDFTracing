//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "MonteCarlo.hlsli"

#define FLT_MAX 3.402823466e+38 // max value

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
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

float3 TraceCone(Texture3D<float> txSDF, RayDesc ray, float coneRadius)
{
	const float k = ray.TMax / coneRadius;
	float r = 0.0, pr = FLT_MAX / 2.0, s = 1.0 / k;
	for (float t = ray.TMin; t < ray.TMax * 0.8; t += r)
	{
		float3 pos = ray.Origin + t * ray.Direction;
		pos = mul(float4(pos, 1.0), g_volumeWorldI);

		if (any(abs(pos) > 1.0)) break;

		const float3 uvw = pos * 0.5 + 0.5;
		r = txSDF.SampleLevel(g_sampler, uvw, 0.0);

		if (r < 1e-4) return float3(t, r, 0.0);

		const float r_sq = r * r;
		const float y = r_sq / (2.0 * pr);
		const float d = sqrt(r_sq - y * y);
		s = min(d / max(t - y, 0.0), s);

		pr = r;
	}

	s *= k;

	return float3(t, r, s);
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
		r = txSDF.SampleLevel(g_sampler, uvw, 0.0);

		occ += (t - max(r, 0.0)) / lMax * falloff;
	}

	const float ao = saturate(1.0 - occ * nInv);

	return float3(t, r, ao);
}

min16float4 TraceIndirect(Texture3D<float> txSDF, Texture3D txIrradiance, RayDesc ray, float falloff)
{
	const uint n = 32;
	const float nInv = 1.0 / n;
	const float rad = 1.0 - nInv; // Hemispherical factor (self occlusion correction)
	const float lMax = ray.TMax - ray.TMin;

	float t = ray.TMin, r = 0.0;
	float occ = 0.0;
	min16float4 radiosity = 0.0;
	for (uint i = 0; i < n; ++i)
	{
		t = ray.TMin + hash(i) * lMax;
		const float2 xi = float2(hash(t + 1.0), hash(t + 2.0));
		const float3 dir = normalize(ray.Direction + computeDirectionHS(ray.Direction, xi) * rad); // mix direction with the normal

		float3 pos = ray.Origin + t * dir;
		pos = mul(float4(pos, 1.0), g_volumeWorldI);

		const float3 uvw = pos * 0.5 + 0.5;
		r = txSDF.SampleLevel(g_sampler, uvw, 0.0);

		const float oc = (t - max(r, 0.0)) / lMax * falloff;
		occ += oc;

		const float4 irradiance = txIrradiance.SampleLevel(g_sampler, uvw, 4.0);
		const min16float a = irradiance.w ? 1.0 : 0.0;
		radiosity += min16float4(irradiance.xyz / irradiance.w * oc, a);
	}

	return min16float4(radiosity.xyz / radiosity.w, t);
}
