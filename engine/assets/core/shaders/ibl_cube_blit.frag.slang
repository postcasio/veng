// IBL cube debug blit: fills the whole output with a radiance cube sampled along each pixel's
// view ray — the DebugView::EnvironmentIbl arm (the prefiltered specular cube the IBL lighting
// reads) and the DebugView::EnvironmentSource arm (the raw cube the IBL convolved from). Unlike
// the skybox it draws every pixel, foreground included, so the environment lighting the scene is
// visible whatever geometry is in front of it.
//
// The cube + linear sampler ride a dedicated set (set 3, the first author set after the reserved
// bindless registries); the view ray is reconstructed through
// the translation-free InvViewRotProj (see Veng/sky.slang SkyViewDirection), exact wherever the
// camera stands. Lod selects the prefilter roughness mip; Intensity is the frame exposure so an
// HDR cube reads at the scene's own brightness. Enabled 0 (no cube backs the source) shows black.
#include "view_constants.slang"

[[vk::binding(0, 3)]] TextureCube g_Cube;
[[vk::binding(1, 3)]] SamplerState g_CubeSampler;

struct PushConstants
{
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
    float Lod;               // prefilter mip (roughness -> LOD); 0 for a single-mip source cube
    float Intensity;         // scales the radiance (the frame exposure)
    uint Enabled;            // 1 = a cube backs this arm; 0 = show black
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    if (g_PC.Enabled == 0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    float2 ndcXY = input.v_UV * 2.0 - 1.0;
    float3 dir = normalize(mul(vc.InvViewRotProj, float4(ndcXY, 1.0, 1.0)).xyz);
    float3 radiance = g_Cube.SampleLevel(g_CubeSampler, dir, g_PC.Lod).rgb * g_PC.Intensity;
    return float4(radiance, 1.0);
}
