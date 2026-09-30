// Ribbon fragment stage. Shapes the ribbon's cross-section — full on its centre line, falling off
// as (1 - v^2)^2 to nothing at its edges — and returns the straight colour with that coverage
// times the opacity. fsMain returns the colour alone; fsMainMasked also returns the bloom-mask
// amplitude, the luminance of the light the fragment contributes, so an HDR ribbon glows
// regardless of the bloom threshold. The alpha pipeline composites it straight-alpha over and the
// additive pipeline adds it weighted by coverage, so one stage serves both.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    float v_Across : TEXCOORD0;
};

struct MaskedOutput
{
    float4 Color : SV_Target0;
    float Mask : SV_Target1;
};

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

float4 ShadeRibbon(VSOutput input)
{
    float edge = saturate(1.0 - input.v_Across * input.v_Across);
    return float4(input.v_Color.rgb, input.v_Color.a * edge * edge);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return ShadeRibbon(input);
}

[shader("fragment")]
MaskedOutput fsMainMasked(VSOutput input)
{
    MaskedOutput output;
    output.Color = ShadeRibbon(input);
    output.Mask = saturate(dot(output.Color.rgb * output.Color.a, LumaWeights));
    return output;
}
