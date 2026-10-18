// Ribbon tube fragment stage. A tube is drawn on both faces, so it reads as a glowing volume rather
// than a shell: each face's coverage is how squarely it faces the eye, |N.V| raised to the tube's
// Softness, so the core where the view crosses the most of the tube is brightest and the silhouette
// fades to nothing. fsMain returns the straight colour and that coverage times the opacity, as the
// band stage does, so the alpha and additive pipelines serve it unchanged; fsMainMasked also returns
// the bloom-mask amplitude, and the reactive entries the temporal resolve's reactive mask after it.
#include "view_constants.slang"

struct TubeVSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    float3 v_Normal : TEXCOORD0;
    float3 v_ViewPosition : TEXCOORD1;
    nointerpolation float v_Softness : TEXCOORD2;
};

struct MaskedOutput
{
    float4 Color : SV_Target0;
    float Mask : SV_Target1;
};

struct ReactiveOutput
{
    float4 Color : SV_Target0;
    float Reactive : SV_Target1;
};

struct MaskedReactiveOutput
{
    float4 Color : SV_Target0;
    float Mask : SV_Target1;
    float Reactive : SV_Target2;
};

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

float4 ShadeTube(TubeVSOutput input)
{
    float3 normal = normalize(input.v_Normal);
    float3 toEye = normalize(-input.v_ViewPosition);
    float facing = saturate(abs(dot(normal, toEye)));
    float coverage = input.v_Softness > 0.0 ? pow(facing, input.v_Softness) : 1.0;
    return float4(input.v_Color.rgb, input.v_Color.a * coverage);
}

[shader("fragment")]
float4 fsMain(TubeVSOutput input) : SV_Target
{
    return ShadeTube(input);
}

// The reactive amount of a straight colour: its coverage, or the luminance of the light it adds.
float ReactiveOf(float4 color)
{
    return saturate(max(color.a, dot(color.rgb * color.a, LumaWeights)));
}

[shader("fragment")]
MaskedOutput fsMainMasked(TubeVSOutput input)
{
    MaskedOutput output;
    output.Color = ShadeTube(input);
    output.Mask = saturate(dot(output.Color.rgb * output.Color.a, LumaWeights));
    return output;
}

[shader("fragment")]
ReactiveOutput fsMainReactive(TubeVSOutput input)
{
    ReactiveOutput output;
    output.Color = ShadeTube(input);
    output.Reactive = ReactiveOf(output.Color);
    return output;
}

[shader("fragment")]
MaskedReactiveOutput fsMainMaskedReactive(TubeVSOutput input)
{
    MaskedReactiveOutput output;
    output.Color = ShadeTube(input);
    output.Mask = saturate(dot(output.Color.rgb * output.Color.a, LumaWeights));
    output.Reactive = ReactiveOf(output.Color);
    return output;
}
