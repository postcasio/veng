// A GuiOverlay composite-material fixture that RESAMPLES the overlay document rather than reading it
// 1:1. It declares a `DocumentSampler` field beside the runtime-bound `Document` handle, which the
// GuiHdrOverlayScenePass fills with the shared linear-clamp fullscreen sampler each frame; with that
// in hand the fragment reads the document at a scaled coordinate, drawing it at `Scale` of its
// rasterized size about the composite origin. A composite that reads 1:1 needs no sampler (a
// `.Load()` by pixel coordinate), so this fixture is the opt-in half of that contract.
//
// Scale < 1 drives the read past the document's right and bottom edges, where the bound sampler's
// address mode is observable: the shared sampler clamps to the (empty) edge, so the region beyond
// the shrunken document stays clear.

#include "Veng/postprocess.slang"

struct MaterialParams
{
    float Scale;          // fraction of its rasterized size the document is drawn at
    uint Document;        // runtime-bound intermediate document handle
    uint DocumentSampler; // runtime-bound sampler for it (the engine writes it where declared)
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    Texture2D document = g_Textures[NonUniformResourceIndex(p.Document)];
    SamplerState documentSampler = g_Samplers[NonUniformResourceIndex(p.DocumentSampler)];

    // The composite draws the post-resolve region, which sv_position addresses in pixels, while the
    // intermediate is the whole render allocation — so a resampling read converts the pixel it wants
    // into a UV through the texture's own dimensions rather than through the interpolated v_UV.
    uint2 dims;
    document.GetDimensions(dims.x, dims.y);
    const float2 source = input.sv_position.xy / max(p.Scale, 1e-4);

    // Already premultiplied; blended premultiplied-over into the scene color.
    return document.SampleLevel(documentSampler, source / float2(dims), 0.0);
}
