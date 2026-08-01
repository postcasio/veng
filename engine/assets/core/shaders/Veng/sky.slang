// The engine's Sky material contract: the set-0 bindless declarations a sky material reads,
// the per-frame view block, the sky push block, the fragment-input struct, and the two
// helpers a sky fragment builds on — the g-buffer depth-mask discard and the per-pixel
// view-ray reconstruction. A Sky material `#include`s this (not Veng/surface.slang, whose
// g_PC surface push would collide with the sky push) and writes only its radiance function
// of a world-space view direction. The Surface and PostProcess domains have their own
// contracts in Veng/surface.slang and Veng/postprocess.slang.
//
// A Sky material is a fullscreen shader run in the SkyScenePass slot: it fills the
// background (the far-plane, un-shaded pixels) with sky radiance and composites over the
// lit scene color with LoadOp::Load, exactly where the procedural atmosphere and the cubemap
// skybox composite. It reuses the fullscreen vertex stage (no per-vertex sky state), like a
// PostProcess material, so only its fragment declares a push block.

// The per-material block byte stride. Mirrors BindlessRegistry::MaterialParamStride, and moves only with it.
static const uint MaterialParamStride = 512;

// The canonical per-frame view-constants block (set 0, binding 5): the sky reconstructs
// the view ray from InvViewProj + CameraPosition and maps the sub-rect depth UV through it.
#include "../view_constants.slang"

// The set-0 bindless arrays a sky reads: the g-buffer depth (a sampled image + a sampler),
// the byte-address storage-buffer array (a sky reads a game-supplied data buffer by handle),
// and the per-material param block. Mirrors the BindlessRegistry layout.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

// The Sky fragment's per-draw push block: the material selector at offset 0 (the
// frame-folded param-block index), plus the g-buffer depth handle, its sampler, and the
// current view-constants region. The engine's SkyMaterialScenePass fills these each frame;
// a sky fragment reads them through the helpers below, not directly. Exactly one
// push_constant block, so the pipeline layout carries a single 16-byte range at offset 0.
struct SkyPushConstants
{
    uint MaterialIndex;      // per-material param-block selector (frame-folded)
    uint DepthTexture;       // g-buffer depth bindless sampled-image index
    uint DepthSampler;       // shared sampler bindless index
    uint ViewConstantsIndex; // current frame's view-constants region
};

// The screen-UV interpolant a Sky fragment reads (the fullscreen vertex stage writes it).
// A sky fragment reconstructs its per-pixel view ray from this UV and the view constants.
struct SkyFragmentInput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// True when the pixel is background (nothing was drawn there): the g-buffer depth is at the
// far plane. A sky fragment discards a foreground pixel so it never overwrites lit geometry
// — the sky is a background composite. Reads the depth through set-0 bindless at the
// dynamic-resolution-mapped UV, matching how the lit target was written.
bool SkyIsBackground(SkyPushConstants pc, float2 uv)
{
    ViewConstants vc = LoadViewConstants(pc.ViewConstantsIndex);
    float2 sampleUV = ScaledSampleUV(uv, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    float depth = g_Textures[NonUniformResourceIndex(pc.DepthTexture)]
                      .Sample(g_Samplers[NonUniformResourceIndex(pc.DepthSampler)], sampleUV)
                      .r;
    return depth >= 1.0;
}

// The world-space view direction through a background pixel. A sky shader evaluates its radiance
// along this direction.
//
// Reconstructed through the translation-free InvViewRotProj rather than InvViewProj. The direction
// is a property of where the camera *looks*, not where it *is*, and taking it from InvViewProj means
// forming a far-plane world point and subtracting the camera off it — two large, nearly equal
// numbers whose small difference is the answer. That cancellation costs f32 precision in proportion
// to the camera's distance from the world origin, and unevenly across the frame (worst where the far
// point is furthest out), so a star field shimmers and does it worse looking away from the origin.
// A rotation-only inverse has no translation to cancel and is exact wherever the camera is.
float3 SkyViewDirection(SkyPushConstants pc, float2 uv)
{
    ViewConstants vc = LoadViewConstants(pc.ViewConstantsIndex);
    float2 ndcXY = uv * 2.0 - 1.0;
    // normalize removes the 1/w, so w never divides and a far-plane w of zero is harmless.
    return normalize(mul(vc.InvViewRotProj, float4(ndcXY, 1.0, 1.0)).xyz);
}
