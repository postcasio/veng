// A GuiFill fixture fragment: a procedural, animated two-tint sweep across the shape's own box
// coordinate. It reads what the domain gives a UI fill and nothing else — the normalized box
// position, the pass clock, and its own parameter block — and returns through GuiFillResolve, so
// the engine's rounded-rect coverage, border ring, clip, and rotation compose with it untouched.
// The vertex color's alpha is the element's composited opacity, folded into the fill's own alpha.
#include "Veng/guifill.slang"

struct MaterialParams
{
    float4 TintA;
    float4 TintB;
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
    float t = saturate(0.5 + 0.5 * sin((box.x + box.y) * 3.0 + g_PC.Time * 2.0));
    float4 fill = lerp(p.TintA, p.TintB, t);
    fill.a *= input.v_Color.a;
    return GuiFillResolve(input, fill);
}
