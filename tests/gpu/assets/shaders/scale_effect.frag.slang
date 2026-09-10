// A PostProcessEffect fixture that samples the finished scene color the engine binds and writes it
// scaled by half. It proves, end to end through the SceneRenderer effect chain: the scene-color
// input reaches the material, the fullscreen mapping is correct, the sub-rect scale map is applied,
// and — because the output is visibly darker than the un-effected frame — that every downstream
// pre-bloom consumer reads the effect's output rather than the pre-effect scene color. Chained twice
// it darkens twice, proving Order and the ping-pong.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    float4 SceneScaleUV; // xy scene-color valid-region scale, zw its half-texel clamp
    uint Scene;          // runtime-bound scene-color source handle
    uint SceneSampler;   // runtime-bound scene-color sampler handle
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float3 scene = SamplePostProcessScene(p.Scene, p.SceneSampler, p.SceneScaleUV, input.v_UV);
    return float4(scene * 0.5, 1.0);
}
