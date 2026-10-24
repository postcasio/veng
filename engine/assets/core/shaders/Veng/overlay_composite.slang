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
// rather than by scene pixel.
//
// A material that works in the document's own frame — an effect that should stand still on a
// projected sheet, or stop at one of its elements' edges — also declares any of
//
//   float2 DocumentExtent;        // the document's logical extent, in its own points
//   float4 DocumentFromScene[3];  // scene pixel -> document point, a 3x3 as rows (w unused)
//   float4 SceneFromDocument[3];  // its inverse: document point -> scene pixel
//
// written alongside when declared. A world-anchored document lies on a flat plane seen through a
// pinhole, so the two are exact projective maps; a screen-space one's are a scale. Map a point
// through either with OverlayMapPoint, and read a document point with LoadOverlayDocumentPoint. The engine scissors the composite to the rect; the helper's
// transparent outside it keeps a read past the rect inert all the same.
//
// A material that replaces what lies under its document — frosted glass, a screen that stops being a
// window — also declares
//
//   uint   Backdrop;        // bindless index of the backdrop chain
//   uint   BackdropSampler; // bindless index of the chain's sampler
//   float4 BackdropParams;  // x the chain's level count, y the reciprocal of the composite's colour
//                           // weight, zw the reciprocal of the chain's extent
//
// and reads it through SampleOverlayBackdrop: the scene colour as it stood before any overlay
// composited this frame, at the post-resolve allocation, with a halving blur chain. The blend can only
// reach the sharp scene already in the target, so a material wanting a different backdrop composites
// its document over the sample and emits at full coverage; the sample comes back divided by the colour
// weight the composite is blended at, so at blur 0 that restores the scene exactly whether or not the
// overlay compensates the exposure.
//
// It declares no binding of its own: it reads g_Textures and g_Samplers from the PostProcess contract,
// so a material includes it after Veng/postprocess.slang.

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

// Maps a point through a 3x3 homography given as rows, dividing through.
float2 OverlayMapPoint(float4 rows[3], float2 point)
{
    const float3 p = float3(point, 1.0);
    const float3 mapped = float3(dot(rows[0].xyz, p), dot(rows[1].xyz, p), dot(rows[2].xyz, p));
    return mapped.xy / mapped.z;
}

// Loads the document texel at a document point (in its logical points) through SceneFromDocument,
// or transparent where it projects outside the rect. Nearest: the document is read 1:1, unsampled.
float4 LoadOverlayDocumentPoint(uint documentHandle, float4 documentRect,
                                float4 sceneFromDocument[3], float2 point)
{
    return LoadOverlayDocument(documentHandle, documentRect,
                               OverlayMapPoint(sceneFromDocument, point));
}

// Samples the backdrop under a scene pixel, blurred by `blur` in [0, 1] across whatever chain the
// frame has: 0 is the sharp copy, 1 the coarsest level (roughly an 8-pixel edge across the long axis,
// a wash rather than a blur). Returns black where no backdrop is wired (`BackdropParams.x` zero).
float3 SampleOverlayBackdrop(uint backdrop, uint backdropSampler, float4 backdropParams, float2 pixel,
                             float blur)
{
    if (backdropParams.x < 1.0)
    {
        return float3(0.0);
    }
    const float lod = saturate(blur) * (backdropParams.x - 1.0);
    const float2 uv = saturate(pixel * backdropParams.zw);
    return g_Textures[NonUniformResourceIndex(backdrop)]
               .SampleLevel(g_Samplers[NonUniformResourceIndex(backdropSampler)], uv, lod)
               .rgb *
           backdropParams.y;
}
