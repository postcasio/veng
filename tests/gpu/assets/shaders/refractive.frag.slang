#include "Veng/translucent.slang"

// Refractive translucent fragment stage (gpu fixture): returns the pre-translucent scene
// color behind the fragment — offset by the authored UVOffset — at full coverage, proving
// the Refraction grab pass and the view block's SceneColor handles reach a translucent
// material. With the renderer's Refraction setting off, SampleSceneColor returns black.
struct MaterialParams
{
    float4 UVOffset; // xy logical-UV offset applied to the scene sample; zw unused
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
    return float4(SampleSceneColor(vc, uv), 1.0);
}
