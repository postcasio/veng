// Reflection-coverage debug blit: colours each pixel by the code the reflection classifier's
// coverage entry point wrote for it (ssr_coverage.slang). The codes are loaded at the pixel, since
// filtering between two codes would name a third.
#include "ssr_coverage.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];

struct PushConstants
{
    uint CoverageTexture;
    uint Sampler;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

float3 CoverageColour(uint code)
{
    switch (code)
    {
    case SsrCoverageEnvironment:
        return float3(0.1, 0.25, 1.0);
    case SsrCoverageRay:
        return float3(0.1, 0.9, 0.1);
    case SsrCoverageVarianceRay:
        return float3(1.0, 0.9, 0.1);
    case SsrCoverageCopied:
        return float3(0.1, 0.9, 0.9);
    case SsrCoverageMirror:
        return float3(1.0, 1.0, 1.0);
    case SsrCoverageUnreached:
        return float3(1.0, 0.1, 0.1);
    default:
        return float3(0.05, 0.05, 0.05);
    }
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float stored = g_Textures[NonUniformResourceIndex(g_PC.CoverageTexture)].Load(int3(int2(input.sv_position.xy), 0)).r;
    return float4(CoverageColour(uint(round(stored * 255.0))), 1.0);
}
