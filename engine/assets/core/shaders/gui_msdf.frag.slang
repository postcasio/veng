// GUI MSDF text fragment path: reconstructs a crisp glyph edge from a multi-channel
// signed-distance-field atlas. The coverage is the median of the sampled RGB
// distances, remapped to a screen-pixel range so the anti-aliasing holds at any
// draw size. The distance range baked into the atlas (in atlas texels) rides the
// params, scaled by the atlas-texels-per-screen-pixel rate from the UV derivatives;
// the atlas texture and sampler are bindless set-0 slots in the params. Output is
// premultiplied linear RGBA so the pass blends the text "over" the scene in linear
// space, tinted by the run's vertex color.

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
    int textureIndex = int(input.v_Params.z);
    int samplerIndex = int(input.v_Params.w);

    Texture2D atlas = g_Textures[NonUniformResourceIndex(textureIndex)];
    SamplerState atlasSampler = g_Samplers[NonUniformResourceIndex(samplerIndex)];

    float3 msd = atlas.Sample(atlasSampler, input.v_Uv).rgb;
    float sd = Median(msd.r, msd.g, msd.b);

    // Convert the [0,1] distance sample to a signed screen-pixel distance: the atlas
    // stores 0.5 on the edge, distanceRange atlas texels across the transition. The
    // per-pixel atlas-texel rate comes from the UV derivatives times the atlas size.
    uint atlasWidth;
    uint atlasHeight;
    atlas.GetDimensions(atlasWidth, atlasHeight);
    float2 texelsPerPixel = fwidth(input.v_Uv) * float2(atlasWidth, atlasHeight);
    float screenPxRange = distanceRange / max(length(texelsPerPixel), 1e-4);

    float screenPxDistance = screenPxRange * (sd - 0.5);
    float coverage = clamp(screenPxDistance + 0.5, 0.0, 1.0);

    float alpha = input.v_Color.a * coverage;
    return float4(input.v_Color.rgb * alpha, alpha);
}
