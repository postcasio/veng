// Skybox: fills the cleared-depth background (where no geometry was drawn) with the
// environment radiance cube along each pixel's view ray, compositing over the lit scene
// color. A separate fullscreen pass after deferred lighting so it shades only background
// pixels (foreground is discarded) and so a richer sky model can replace it in place.
//
// The depth target is sampled through set-0 bindless; the radiance cube + linear sampler
// ride the renderer's IBL set (set 1 here), the same set the lighting pass binds at set 2.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

[[vk::binding(0, 1)]] TextureCube g_Radiance;
[[vk::binding(4, 1)]] SamplerState g_IblSampler;

struct PushConstants
{
    uint DepthTexture;        // g-buffer depth bindless index
    uint Sampler;             // shared sampler bindless index
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint Enabled;             // 1 = an environment is bound this frame; 0 = discard all
    float EnvIntensity;       // scales the skybox radiance
    uint Pad0;
    uint Pad1;
    uint Pad2;
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
        discard;
    }

    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    // The depth target is rendered into the dynamic-resolution sub-rect; remap the logical UV
    // into the valid region before sampling (identity at scale 1.0). The NDC reconstruction
    // below keeps the raw logical UV — the view is logically full-screen.
    float2 sampleUV = ScaledSampleUV(input.v_UV, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;

    // Geometry already shaded this pixel; leave the lit color untouched.
    if (depth < 1.0)
    {
        discard;
    }

    // Unproject the far-plane point through the inverse view-projection and form the view ray.
    float2 ndcXY = input.v_UV * 2.0 - 1.0;
    float4 worldH = mul(vc.InvViewProj, float4(ndcXY, 1.0, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 dir = normalize(worldPos - vc.CameraPosition.xyz);

    float3 sky = g_Radiance.SampleLevel(g_IblSampler, dir, 0.0).rgb * g_PC.EnvIntensity;
    return float4(sky, 1.0);
}
