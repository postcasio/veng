#include "Veng/translucent.slang"
#include "Veng/octahedral.slang"

// A capture-consuming material that reads the probe centre the component publishes (gpu fixture).
// It stands in for a parallax-correcting fragment without doing the correction's math: what it
// needs from the engine is the world position the map was rendered from and a flag saying whether a
// capture exists at all, and this material makes both readable off a rendered frame.
//
// Three branches, in the order a real consumer takes them: an unbound centre (w == 0) returns a
// bright magenta fallback — the only branch that may run while the handle slots hold an unpopulated
// sentinel, which is the whole reason the flag exists; Display.x set returns the published centre as
// radiance, so a frame reads back the position the drive said the probe sat at; otherwise it samples
// the capture in a fixed direction, the ordinary consuming path.
struct MaterialParams
{
    uint  Texture;
    uint  Sampler;
    uint  Pad0;
    uint  Pad1;
    float4 Center;  // engine-published: xyz the probe's world position, w 1 while a capture is bound
    float4 Display; // x selects what the fragment shows: 0 the captured colour, 1 the centre itself
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    // Nothing bound: the texture slot carries the unbound sentinel, so this is the only branch that
    // may sample nothing. Bright, so the frame reads it whatever the exposure.
    if (p.Center.w <= 0.5)
    {
        return float4(2.0, 0.0, 2.0, 1.0);
    }

    if (p.Display.x > 0.5)
    {
        return float4(p.Center.xyz, 1.0);
    }

    const float2 uv = OctahedralUV(float3(0.0, -1.0, 0.0));
    const float3 color = g_Textures[NonUniformResourceIndex(p.Texture)]
                             .Sample(g_Samplers[NonUniformResourceIndex(p.Sampler)], uv)
                             .rgb;
    return float4(color, 1.0);
}
