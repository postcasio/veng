// GUI shape fragment path: a rounded-rectangle signed-distance field with
// screen-derivative anti-aliased edges and an optional inner border, optionally
// modulated by a bindless texture (a plain textured quad or one 9-slice tile is a
// zero-radius rect with a bound texture). The interpolated per-fragment local
// coordinate and the rect half-extent reconstruct the SDF: negative inside, zero
// on the edge. Corner radius and border width ride the params; the texture and
// sampler indices are bindless set-0 slots, with a negative texture index meaning
// untextured. Output is premultiplied linear RGBA so the pass blends it "over" the
// scene in linear space.

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_Uv : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_RectHalf : TEXCOORD1;
    float2 v_RectCoord : TEXCOORD2;
    nointerpolation float4 v_Params : TEXCOORD3;
};

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

    float4 color = input.v_Color;
    if (textureIndex >= 0)
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
