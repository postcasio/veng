// Composite fragment stage: blends the ImGui overlay over the rendered scene and
// applies the final display-transfer encoding for the swapchain's color space.
// The scene and overlay images, plus the shared sampler, are addressed by bindless
// handles pushed in the constant block; set 0 is the bindless registry.
//
// EncodeMode selects the transfer applied to the linear blended color:
//   0 — passthrough. Correct for an sRGB-nonlinear swapchain (the _SRGB format
//       applies the sRGB transfer on store) and for scRGB / extended-linear (the
//       display interprets linear values directly).
//   1 — HDR10: convert Rec.709 primaries to Rec.2020, scale linear-1.0 to paper
//       white in nits, clamp to peak, and PQ (ST2084) encode.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct PushConstants
{
    uint SceneTexture;
    uint ImGuiTexture;
    uint Sampler;
    uint EncodeMode;
    float PaperWhiteNits;
    float PeakNits;
};

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// Rec.709 (sRGB) primaries to Rec.2020 primaries, each row summing to ~1.
static const float3x3 Rec709ToRec2020 = float3x3(0.627402, 0.329292, 0.043306, 0.069095, 0.919544,
                                                 0.011360, 0.016394, 0.088028, 0.895578);

// SMPTE ST 2084 (PQ) inverse-EOTF over an input normalized to [0,1] across 10000 nits.
float3 PqEncode(float3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 y = clamp(nits / 10000.0, 0.0, 1.0);
    float3 ym1 = pow(y, m1);
    return pow((c1 + c2 * ym1) / (1.0 + c3 * ym1), m2);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float4 scene = g_Textures[NonUniformResourceIndex(g_PC.SceneTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV);
    float4 ui = g_Textures[NonUniformResourceIndex(g_PC.ImGuiTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV);

    float3 color = lerp(scene.rgb, ui.rgb, ui.a);

    if (g_PC.EncodeMode == 1)
    {
        float3 rec2020 = mul(Rec709ToRec2020, color);
        float3 nits = min(rec2020 * g_PC.PaperWhiteNits, g_PC.PeakNits.xxx);
        color = PqEncode(nits);
    }

    return float4(color, 1.0);
}
