// Screen-space reflections: the constants block every pass of the chain reads, and the helpers the
// passes share. Ports ffx_sssr_common.h and the constant accessors and resource helpers of
// ffx_sssr_callbacks_hlsl.h from the FidelityFX SDK v1.1.4.
//
// The reference's coordinate helpers flip y between UV and NDC (Direct3D's NDC is y-up). The
// engine's projection leaves NDC y-down, the same way round as UV, so the flip is omitted: the
// mapping is the same one the deferred lighting reconstructs through.
//
// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

// The reference's cbSSSR, less the g-buffer decode parameters (the normal unpack pair, the
// roughness channel and its perceptual flag), which the engine's fixed g-buffer layout settles at
// the load, followed by the one field of the denoiser's cbDenoiserReflections it does not share:
// the denoiser's render size. Mirrors the renderer's SsrConstants (SsrChain.h) field for field;
// std140.
struct SsrConstants
{
    float4x4 InvViewProjection;
    float4x4 Projection;
    float4x4 InvProjection;
    float4x4 ViewMatrix;
    float4x4 InvView;
    float4x4 PrevViewProjection;
    uint2 RenderSize;
    float2 InverseRenderSize;
    float IblFactor;
    float TemporalStabilityFactor;
    float DepthBufferThickness;
    float RoughnessThreshold;
    float VarianceThreshold;
    uint FrameIndex;
    uint MaxTraversalIntersections;
    uint MinTraversalOccupancy;
    uint MostDetailedMip;
    uint SamplesPerQuad;
    uint TemporalVarianceGuidedTracingEnabled;
    uint Padding;
    uint2 DenoiserRenderSize;
    float2 DenoiserInverseRenderSize;
    float4 IblTint;
};

// Every pass that reads the constants binds them at binding 0 of its own set 3, at this frame's
// region of the chain's ring (a dynamic offset).
[[vk::binding(0, 3)]] ConstantBuffer<SsrConstants> g_Ssr;

float4x4 InvViewProjection() { return g_Ssr.InvViewProjection; }
float4x4 Projection() { return g_Ssr.Projection; }
float4x4 InvProjection() { return g_Ssr.InvProjection; }
float4x4 ViewMatrix() { return g_Ssr.ViewMatrix; }
float4x4 InvView() { return g_Ssr.InvView; }
float4x4 PrevViewProjection() { return g_Ssr.PrevViewProjection; }
uint2 RenderSize() { return g_Ssr.RenderSize; }
float2 InverseRenderSize() { return g_Ssr.InverseRenderSize; }
float IBLFactor() { return g_Ssr.IblFactor; }
float3 IBLTint() { return g_Ssr.IblTint.rgb; }
float TemporalStabilityFactor() { return g_Ssr.TemporalStabilityFactor; }
float DepthBufferThickness() { return g_Ssr.DepthBufferThickness; }
float RoughnessThreshold() { return g_Ssr.RoughnessThreshold; }
float VarianceThreshold() { return g_Ssr.VarianceThreshold; }
uint FrameIndex() { return g_Ssr.FrameIndex; }
uint MaxTraversalIntersections() { return g_Ssr.MaxTraversalIntersections; }
uint MinTraversalOccupancy() { return g_Ssr.MinTraversalOccupancy; }
uint MostDetailedMip() { return g_Ssr.MostDetailedMip; }
uint SamplesPerQuad() { return g_Ssr.SamplesPerQuad; }
bool TemporalVarianceGuidedTracingEnabled() { return g_Ssr.TemporalVarianceGuidedTracingEnabled != 0u; }

//=== Common functions of the SssrSample ===

void UnpackRayCoords(uint packed, out uint2 ray_coord, out bool copy_horizontal, out bool copy_vertical, out bool copy_diagonal)
{
    ray_coord.x = (packed >> 0) & 32767;  // 0b111111111111111;
    ray_coord.y = (packed >> 15) & 16383; // 0b11111111111111;
    copy_horizontal = bool((packed >> 29) & 1u);
    copy_vertical = bool((packed >> 30) & 1u);
    copy_diagonal = bool((packed >> 31) & 1u);
}

uint PackRayCoords(uint2 ray_coord, bool copy_horizontal, bool copy_vertical, bool copy_diagonal)
{
    uint ray_x_15bit = ray_coord.x & 0x7FFFu;
    uint ray_y_14bit = ray_coord.y & 0x3FFFu;
    uint copy_horizontal_1bit = copy_horizontal ? 1 : 0;
    uint copy_vertical_1bit = copy_vertical ? 1 : 0;
    uint copy_diagonal_1bit = copy_diagonal ? 1 : 0;

    uint packed = (copy_diagonal_1bit << 31) | (copy_vertical_1bit << 30) | (copy_horizontal_1bit << 29) | (ray_y_14bit << 15) | (ray_x_15bit << 0);
    return packed;
}

// Transforms origin to uv space
// Mat must be able to transform origin from its current space into clip space.
float3 ProjectPosition(float3 origin, float4x4 mat)
{
    float4 projected = mul(mat, float4(origin, 1));
    projected.xyz /= projected.w;
    projected.xy = 0.5 * projected.xy + 0.5;
    return projected.xyz;
}

// Mat must be able to transform origin from texture space to a linear space.
float3 InvProjectPosition(float3 coord, float4x4 mat)
{
    coord.xy = 2 * coord.xy - 1;
    float4 projected = mul(mat, float4(coord, 1));
    projected.xyz /= projected.w;
    return projected.xyz;
}

// Origin and direction must be in the same space and mat must be able to transform from that space into clip space.
float3 ProjectDirection(float3 origin, float3 direction, float3 screen_space_origin, float4x4 mat)
{
    float3 offsetted = ProjectPosition(origin + direction, mat);
    return offsetted - screen_space_origin;
}

bool IsGlossyReflection(float roughness)
{
    return roughness < RoughnessThreshold();
}

bool IsMirrorReflection(float roughness)
{
    return roughness < 0.0001;
}

float3 ScreenSpaceToViewSpace(float3 screen_uv_coord)
{
    return InvProjectPosition(screen_uv_coord, InvProjection());
}
