// Screen-space reflections: the composite that adds the denoised reflection to the lit scene colour.
// Ports samples/sssr/shaders/sssr_apply_reflections.hlsl and sssr_apply_reflections_common.h from
// the FidelityFX SDK v1.1.4, the sample's apply pass, with the reflection it draws (its
// drawReflections branch at reflectionsIntensity 1).
//
// The sample samples every input with a linear sampler at the pixel's texture coordinate; at a
// pixel centre over targets of the frame's own resolution that is the texel, so the inputs are
// loaded at the fragment's pixel. The reflection is radiance A, which the sample's host copies to
// its output target first. The g-buffer stores perceptual roughness (the reference's
// sqrt(alphaRoughness)) and a signed world normal (its 2n - 1 decode), and the engine's split-sum
// LUT is indexed (N·V, perceptual roughness) where the sample's is indexed (N·V,
// 1 - perceptual roughness): the same table entry, read through each table's own axes.
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

[[vk::binding(0, 0)]] Texture2D g_Textures[];

[[vk::binding(0, 3)]] Texture2D<float4> reflectionTarget;
[[vk::binding(1, 3)]] Texture2D<float4> brdfTexture;
[[vk::binding(2, 3)]] SamplerState linearSampler;

// The sample's ApplyReflectionsConstants (its viewDirection: the camera's direction, w 0), followed
// by the g-buffer the engine reaches through bindless.
struct PushConstants
{
    float4 viewDirection;
    uint BaseColorTexture; // the g-buffer albedo's bindless index (G0)
    uint NormalTexture;    // the g-buffer world normal's bindless index (G1)
    uint MaterialTexture;  // the g-buffer occlusion/roughness/metallic's bindless index (G2)
    uint Padding;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// Important bits from the PBR shader
float3 getIBLContribution(float perceptualRoughness, float3 f0, float3 specularLight, float3 n, float3 v)
{
    float NdotV = clamp(dot(n, v), 0.0, 1.0);
    float2 brdfSamplePoint = saturate(float2(NdotV, perceptualRoughness));
    float2 brdf = brdfTexture.SampleLevel(linearSampler, brdfSamplePoint, 0.0).rg;

    return specularLight * (f0 * brdf.x + brdf.y);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    int3 texel = int3(int2(input.sv_position.xy), 0);
    float4 aoRoughnessMetallic = g_Textures[NonUniformResourceIndex(g_PC.MaterialTexture)].Load(texel);
    float3 radiance = reflectionTarget.Load(texel).xyz;
    float perceptualRoughness = aoRoughnessMetallic.g;
    float3 baseColor = g_Textures[NonUniformResourceIndex(g_PC.BaseColorTexture)].Load(texel).rgb;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, float3(aoRoughnessMetallic.b, aoRoughnessMetallic.b, aoRoughnessMetallic.b));
    float3 normal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Load(texel).xyz;
    float3 view = g_PC.viewDirection.xyz;

    radiance = getIBLContribution(perceptualRoughness, f0, radiance, normal, view);
    return float4(radiance, 1); // Show the reflections applied to the scene
}
