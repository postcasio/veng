// A GuiOverlay composite-material fixture for the backdrop contract: it puts the overlay's document
// over the backdrop sampled at its pixel, blurred by Blur, and emits at full coverage — so wherever
// the document's rect reaches, the scene beneath is replaced by its own copy, sharp at Blur 0 and
// softened above it.

#include "Veng/postprocess.slang"
#include "Veng/overlay_composite.slang"

struct MaterialParams
{
    float  Blur;            // how far down the backdrop chain to read, in [0, 1]
    uint   Document;        // runtime-bound intermediate document handle
    float4 DocumentRect;    // the document's rect in scene pixels: origin, size
    uint   Backdrop;        // runtime-bound backdrop chain handle
    uint   BackdropSampler; // the chain's sampler
    float4 BackdropParams;  // levels, inverse colour weight, inverse extent
};

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

[shader("fragment")]
float4 fsMain(PostProcessFragmentInput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialOffset);
    const float4 texel =
        LoadOverlayDocument(p.Document, p.DocumentRect, input.sv_position.xy); // premultiplied
    const float3 backdrop = SampleOverlayBackdrop(p.Backdrop, p.BackdropSampler, p.BackdropParams,
                                                  input.sv_position.xy, p.Blur);
    return float4(texel.rgb + backdrop * (1.0 - texel.a), 1.0);
}
