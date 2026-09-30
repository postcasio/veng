// Flipbook sprite fragment stage. Samples the atlas frame, resolves the atlas's alpha convention
// into premultiplied colour and coverage, and applies the tint and opacity. fsMain returns that
// colour alone; fsMainMasked also returns the bloom-mask amplitude, the colour's luminance, so a
// bright sprite glows regardless of the bloom threshold. The alpha pipeline composites the result
// premultiplied-over and the additive pipeline adds it, so one stage serves both.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    nointerpolation uint v_Texture : TEXCOORD1;
    nointerpolation uint v_Sampler : TEXCOORD2;
    nointerpolation uint v_AlphaMode : TEXCOORD3;
};

struct MaskedOutput
{
    float4 Color : SV_Target0;
    float Mask : SV_Target1;
};

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

float4 ShadeSprite(VSOutput input)
{
    float4 texel = g_Textures[NonUniformResourceIndex(input.v_Texture)].Sample(
        g_Samplers[NonUniformResourceIndex(input.v_Sampler)], input.v_UV);

    // Every convention is brought to premultiplied (colour already weighted by coverage).
    float3 color = texel.rgb;
    float coverage = texel.a;
    if (input.v_AlphaMode == 1)
    {
        color *= coverage;
    }
    else if (input.v_AlphaMode == 2)
    {
        coverage = saturate(dot(color, LumaWeights));
    }
    else if (input.v_AlphaMode == 3)
    {
        coverage = 1.0;
    }

    float opacity = input.v_Color.a;
    return float4(color * input.v_Color.rgb * opacity, coverage * opacity);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return ShadeSprite(input);
}

[shader("fragment")]
MaskedOutput fsMainMasked(VSOutput input)
{
    MaskedOutput output;
    output.Color = ShadeSprite(input);
    output.Mask = saturate(dot(output.Color.rgb, LumaWeights));
    return output;
}
