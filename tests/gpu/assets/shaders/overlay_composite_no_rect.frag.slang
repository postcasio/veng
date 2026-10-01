// A composite-material fixture that predates the document-rect contract: it declares the Document
// handle but no DocumentRect, and reads the document by scene pixel. The overlay pass reports such a
// material by name and does not composite it, rather than drawing it at the wrong offset.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    uint Document; // runtime-bound intermediate document handle
};

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialOffset);
    return g_Textures[NonUniformResourceIndex(p.Document)].Load(int3(int2(input.sv_position.xy), 0));
}
