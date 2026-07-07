// GUI shape fragment path: a rounded-rectangle signed-distance field with
// screen-derivative anti-aliased edges and an optional inner border, its fill one of
// three sources — a flat vertex color, a bindless texture modulating the color (a
// plain textured quad or one 9-slice tile is a zero-radius rect with a bound texture),
// or a gradient that reduces the fragment's normalized box position to an offset into a
// 1D ramp LUT (multi-stop color baked into the ramp). The interpolated per-fragment
// local coordinate and the rect half-extent reconstruct the SDF: negative inside, zero
// on the edge. Corner radius and border width ride the params; the texture and sampler
// indices are bindless set-0 slots, with a negative texture index meaning untextured.
// Output is premultiplied linear RGBA so the pass blends it "over" the scene in linear
// space.

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_Uv : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_RectHalf : TEXCOORD1;
    float2 v_RectCoord : TEXCOORD2;
    nointerpolation float4 v_Params : TEXCOORD3;
    nointerpolation float4 v_Grad : TEXCOORD4;
};

static const float InvTau = 0.15915494309189535; // 1 / (2*pi)

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

// Signed distance to a rounded rectangle centered at the origin, half-extent b,
// corner radius r (subtracted from the box so the corners round inward).
float RoundedBoxSdf(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float radius = input.v_Params.x;
    float border = input.v_Params.y;
    int textureIndex = int(input.v_Params.z);
    int samplerIndex = int(input.v_Params.w);

    // v_Grad.w is 0 for no gradient, else the GradientKind ordinal plus one (1 Linear, 2 Radial,
    // 3 Conic). A gradient reduces the fragment's normalized box position to a ramp offset t and
    // samples the ramp bound through the texture/sampler slots; a plain textured quad samples by UV.
    int gradKind = int(input.v_Grad.w);
    float4 color = input.v_Color;
    if (gradKind > 0)
    {
        float2 p = input.v_RectCoord / max(input.v_RectHalf, float2(1e-4, 1e-4));
        float t;
        if (gradKind == 1)
        {
            t = dot(p, input.v_Grad.xy) + 0.5;
        }
        else if (gradKind == 2)
        {
            t = length((p - input.v_Grad.xy) * input.v_Grad.z);
        }
        else
        {
            t = frac(atan2(p.y - input.v_Grad.y, p.x - input.v_Grad.x) * InvTau - input.v_Grad.z);
        }
        float4 ramp = g_Textures[NonUniformResourceIndex(textureIndex)].Sample(
            g_Samplers[NonUniformResourceIndex(samplerIndex)], float2(saturate(t), 0.5));
        color *= ramp;
    }
    else if (textureIndex >= 0)
    {
        float4 texel = g_Textures[NonUniformResourceIndex(textureIndex)].Sample(
            g_Samplers[NonUniformResourceIndex(samplerIndex)], input.v_Uv);
        color *= texel;
    }

    // Screen-space coverage from the SDF: a one-pixel-wide transition centered on
    // the edge, its width taken from the distance field's rate of change so the
    // anti-aliasing is resolution-independent.
    float dist = RoundedBoxSdf(input.v_RectCoord, input.v_RectHalf, radius);
    float aa = max(fwidth(dist), 1e-4);
    float coverage = 1.0 - smoothstep(-aa, aa, dist);

    // A positive border width fills only the ring within `border` pixels of the
    // edge, leaving the interior transparent; zero border fills the whole shape.
    if (border > 0.0)
    {
        float inner = RoundedBoxSdf(input.v_RectCoord, input.v_RectHalf, radius) + border;
        float innerCoverage = 1.0 - smoothstep(-aa, aa, inner);
        coverage *= 1.0 - innerCoverage;
    }

    float alpha = color.a * coverage;
    return float4(color.rgb * alpha, alpha);
}
