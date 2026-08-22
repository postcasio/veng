#include "Veng/translucent.slang"

// Refractive translucent fragment stage (gpu fixture): returns the pre-translucent scene
// color behind the fragment — offset by the authored UVOffset, and blurred by the amount beside it
// — at full coverage, proving the Refraction grab pass, its mip chain, and the view block's
// SceneColor handles all reach a translucent material. With the renderer's Refraction setting off,
// the sample returns black; with RefractionBlur off, any blur returns the sharp copy.
struct MaterialParams
{
    float4 UVOffset; // xy logical-UV offset applied to the scene sample; z blur in [0,1]; w unused
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float2 uv = SceneScreenUV(vc, input.sv_position) + p.UVOffset.xy;
    // Blur zero routes through the blurred helper too, so the case that asserts the two agree is
    // asserting the helper's own level-0 path rather than a branch that skipped it.
    return float4(SampleSceneColorBlurred(vc, uv, p.UVOffset.z), 1.0);
}
