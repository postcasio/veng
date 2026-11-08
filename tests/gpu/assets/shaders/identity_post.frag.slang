// Identity PostProcess fragment fixture: samples its one runtime-bound input
// texture/sampler through the bindless set-0 array and writes it unchanged to the
// single color target (the PostProcess domain's output contract — one float4
// SV_Target0). It proves the PostProcess fullscreen-material path end to end:
// build-from-material, set-0 bind, upstream sample, selector push at offset 0,
// fullscreen draw, output written.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    uint Source;        // runtime-bound sampled-image handle
    uint SourceSampler; // runtime-bound sampler handle
};

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialOffset);
    return g_Textures[NonUniformIndex(p.Source)].Sample(
        g_Samplers[NonUniformIndex(p.SourceSampler)], input.v_UV);
}
