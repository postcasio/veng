#include "Veng/translucent.slang"

// Scene-depth reader translucent fragment stage (gpu fixture), for a material declaring only
// "readsSceneDepth". Select.x picks what it returns, at full coverage:
//   0 — white where the opaque-depth copy holds geometry behind this fragment, black where it reads
//       the far plane: the depth reaches a material with no colour grab at all;
//   1 — the view's frame index in red (a quarter per frame) and its temporal-resolve flag in green.
struct MaterialParams
{
    float4 Select; // x the mode above; yzw unused
};

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialOffset);
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    if (p.Select.x < 0.5)
    {
        // Reverse-Z: geometry behind the fragment is nearer the far plane's 0 than it is.
        const float depth = SampleSceneDepth(vc, SceneScreenUV(vc, input.sv_position));
        const float behind = depth > 0.0 && depth < input.sv_position.z ? 1.0 : 0.0;
        return float4(behind, behind, behind, 1.0);
    }
    return float4(0.25 * float(vc.FrameIndex), float(vc.TemporalResolveActive), 0.0, 1.0);
}
