// The HUD header's fill: a GuiFill material, the fill source a shader authors rather than the
// engine. It reads what the domain gives a UI fill — the fragment's normalized position inside the
// shape, the pass clock, and its own parameter block — and returns through GuiFillResolve, so the
// engine's rounded-rect coverage, border ring, clip, and rotation compose with it untouched.
//
// A diagonal band sweeps across the box on the clock, tinting between two authored colors: an
// animated appearance with no per-frame game code and no per-frame parameter write.
#include "Veng/guifill.slang"

struct MaterialParams
{
    float4 TintA; // the band's trough color
    float4 TintB; // the band's crest color
    float4 Motion; // x: spatial frequency, y: sweep speed in turns/second, zw unused
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(GuiFillInputs input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float2 box = GuiFillBoxCoord(input);
    float phase = (box.x + box.y) * p.Motion.x + g_PC.Time * p.Motion.y;
    float t = saturate(0.5 + 0.5 * sin(phase));
    float4 fill = lerp(p.TintA, p.TintB, t);
    // The vertex color's alpha carries the element's composited opacity.
    fill.a *= input.v_Color.a;
    return GuiFillResolve(input, fill);
}
