// GUI text fragment path: reconstructs a crisp glyph edge from a signed-distance-field
// atlas page. A glyph carries its field type per quad in params.y — an Msdf (0)
// rendition reconstructs coverage from the median of its three channels, an Sdf (1)
// rendition from its single red channel — and both share one distance-range convention,
// so the remap to a screen-pixel range is identical whichever field a glyph carries.
// The distance range (in atlas texels) rides params.x, scaled by the
// atlas-texels-per-screen-pixel rate from the UV derivatives; the atlas page texture is
// the bindless slot in params.z and the sampler in params.w. Output is premultiplied
// linear RGBA so the pass blends the text "over" the scene in linear space, tinted by
// the run's vertex color.

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

float Median(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float distanceRange = input.v_Params.x; // px range baked into the SDF, in atlas texels
    int fieldType = int(input.v_Params.y);  // 0 = Msdf (median of rgb), 1 = Sdf (r)
    int textureIndex = int(input.v_Params.z);
    int samplerIndex = int(input.v_Params.w);

    Texture2D atlas = g_Textures[NonUniformResourceIndex(textureIndex)];
    SamplerState atlasSampler = g_Samplers[NonUniformResourceIndex(samplerIndex)];

    // An Msdf glyph reconstructs the signed distance from the median of its three channels; an Sdf
    // glyph carries a single-channel field in red. Both encode the same distance range, so the
    // coverage math below divides by one range constant regardless of which branch produced sd.
    float4 sample = atlas.Sample(atlasSampler, input.v_Uv);
    float sd = fieldType == 1 ? sample.r : Median(sample.r, sample.g, sample.b);

    // Convert the [0,1] distance sample to a signed screen-pixel distance: the atlas
    // stores 0.5 on the edge, distanceRange atlas texels across the transition. The
    // per-pixel atlas-texel rate comes from the UV derivatives times the atlas size.
    uint atlasWidth;
    uint atlasHeight;
    atlas.GetDimensions(atlasWidth, atlasHeight);
    float2 texelsPerPixel = fwidth(input.v_Uv) * float2(atlasWidth, atlasHeight);
    // Clamped to one screen pixel: under minification (a glyph drawn below its baked atlas
    // scale) the projected range shrinks under a pixel, and unclamped the coverage would
    // never reach zero across the quad — every glyph ghosts inside a faint box. One pixel
    // is the narrowest transition a fragment grid can resolve anyway.
    float screenPxRange = max(distanceRange / max(length(texelsPerPixel), 1e-4), 1.0);

    float screenPxDistance = screenPxRange * (sd - 0.5);
    float coverage = clamp(screenPxDistance + 0.5, 0.0, 1.0);

    float alpha = input.v_Color.a * coverage;
    return float4(input.v_Color.rgb * alpha, alpha);
}
