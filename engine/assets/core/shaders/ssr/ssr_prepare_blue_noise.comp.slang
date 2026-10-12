// Screen-space reflections: this frame's 128×128 tile of two-dimensional blue noise, which the
// intersection samples its reflection directions from. Ports ffx_sssr_prepare_blue_noise_texture.h
// and its pass entry point from the FidelityFX SDK v1.1.4: Heitz and Belcour's (2019) sampler reads
// the first sample of an Owen-scrambled Sobol sequence, scrambled per pixel by an optimised key,
// then offsets it by the golden ratio times the frame index so successive frames decorrelate.
//
// The Sobol and scrambling tables are the ones the reference's host uploads (the 1spp variant),
// read here as unsigned-integer textures.
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

#include "ssr_common.slang"

[[vk::binding(1, 3)]] Texture2D<uint> g_SobolBuffer;
[[vk::binding(2, 3)]] Texture2D<uint> g_ScramblingTileBuffer;
[[vk::binding(3, 3)]] [format("rg8")] RWTexture2D<float2> g_BlueNoiseTexture;

#define GOLDEN_RATIO 1.61803398875f

uint FFX_SSSR_GetSobolSample(uint3 coordinate)
{
    return g_SobolBuffer.Load(int3(coordinate));
}

uint FFX_SSSR_GetScramblingTile(uint3 coordinate)
{
    return g_ScramblingTileBuffer.Load(int3(coordinate));
}

void FFX_SSSR_StoreBlueNoiseSample(uint2 coordinate, float2 blue_noise_sample)
{
    g_BlueNoiseTexture[coordinate] = blue_noise_sample;
}

// Blue Noise Sampler by Eric Heitz. Returns a value in the range [0, 1].
float SampleRandomNumber(uint pixel_i, uint pixel_j, uint sample_index, uint sample_dimension)
{
    // Wrap arguments
    pixel_i = pixel_i & 127u;
    pixel_j = pixel_j & 127u;
    sample_index = sample_index & 255u;
    sample_dimension = sample_dimension & 255u;

    // xor index based on optimized ranking
    const uint ranked_sample_index = sample_index;

    // Fetch value in sequence
    uint value = FFX_SSSR_GetSobolSample(uint3(sample_dimension, ranked_sample_index * 256u, 0));

    // If the dimension is optimized, xor sequence value based on optimized scrambling
    uint originalIndex = (sample_dimension % 8u) + (pixel_i + pixel_j * 128u) * 8u;
    value = value ^ FFX_SSSR_GetScramblingTile(uint3(originalIndex % 512u, originalIndex / 512u, 0));

    // Convert to float and return
    return (value + 0.5f) / 256.0f;
}

float2 SampleRandomVector2D(uint2 pixel)
{
    float2 u = float2(
        fmod(SampleRandomNumber(pixel.x, pixel.y, 0, 0u) + (FrameIndex() & 0xFFu) * GOLDEN_RATIO, 1.0f),
        fmod(SampleRandomNumber(pixel.x, pixel.y, 0, 1u) + (FrameIndex() & 0xFFu) * GOLDEN_RATIO, 1.0f));
    return u;
}

void PrepareBlueNoiseTexture(uint2 dispatch_thread_id)
{
    FFX_SSSR_StoreBlueNoiseSample(dispatch_thread_id, SampleRandomVector2D(dispatch_thread_id));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    PrepareBlueNoiseTexture(DispatchThreadId.xy);
}
