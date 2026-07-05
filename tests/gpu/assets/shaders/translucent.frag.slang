#include "Veng/translucent.slang"

// Forward translucent material fragment stage (gpu fixture). Reads its per-draw MaterialParams
// block through the engine's set-0 g_MaterialParams by the v_MaterialIndex interpolant (the surface
// vertex path Veng/translucent.slang re-exports), and returns final HDR color + coverage as the
// single float4 SV_Target0 the Translucent domain contract requires. Unlit — it returns its
// authored color directly, alpha-blended into the scene by the forward translucent pass.
struct MaterialParams
{
    float4 Color; // HDR rgb + coverage a
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);
    return p.Color;
}
