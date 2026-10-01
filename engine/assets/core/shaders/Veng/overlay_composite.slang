// The read side of the pre-bloom GUI overlay composite contract. A PostProcess material compositing
// a SceneHdrPreBloom overlay's document runs over the scene colour, but the document it reads is not
// scene-sized: the engine renders it into an intermediate holding only the document's rect — the
// granule-rounded screen bounds of its projection — with the rect's origin at the intermediate's
// origin. So the material declares, beside its own parameters,
//
//   uint   Document;     // bindless index of the rendered document (premultiplied HDR)
//   float4 DocumentRect; // xy the rect's origin in scene pixels, zw its size
//
// both written by the engine each frame, and reads the document through LoadOverlayDocument below
// rather than by scene pixel. The engine scissors the composite to the rect; the helper's
// transparent outside it keeps a read past the rect inert all the same.
//
// It declares no binding of its own: it reads g_Textures from the PostProcess contract, so a
// material includes it after Veng/postprocess.slang.

// Loads the document texel under a scene pixel (sv_position.xy), or transparent (premultiplied
// zero) where the pixel lies outside the document's rect.
float4 LoadOverlayDocument(uint documentHandle, float4 documentRect, float2 pixel)
{
    const float2 local = floor(pixel) - documentRect.xy;
    if (any(local < 0.0) || any(local >= documentRect.zw))
    {
        return float4(0.0);
    }
    return g_Textures[NonUniformResourceIndex(documentHandle)].Load(int3(int2(local), 0));
}
