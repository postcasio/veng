// Ribbon tube fragment stage. A tube is drawn on both faces, so it reads as a glowing volume rather
// than a shell: each face's coverage is how squarely it faces the eye, |N.V| raised to the tube's
// Softness, so the core where the view crosses the most of the tube is brightest and the silhouette
// fades to nothing. fsMain returns the straight colour and that coverage times the opacity, as the
// band stage does, so the alpha and additive pipelines serve it unchanged; fsMainMasked also returns
// the bloom-mask amplitude.
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

[shader("fragment")]
MaskedOutput fsMainMasked(TubeVSOutput input)
{
    MaskedOutput output;
    output.Color = ShadeTube(input);
    output.Mask = saturate(dot(output.Color.rgb * output.Color.a, LumaWeights));
    return output;
}
