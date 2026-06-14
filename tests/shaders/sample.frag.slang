// Samples a combined image sampler at the interpolated UV. Set 0 is reserved
// for the bindless registry, so the sampler binds on set 1, matching the test's
// hand-built CombinedImageSampler layout.
[[vk::binding(0, 1)]] Sampler2D u_Source;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return u_Source.Sample(input.v_UV);
}
