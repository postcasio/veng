#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    /// @brief The engine core pack's fullscreen vertex shader, shared by every fullscreen pipeline.
    ///
    /// The AssetManager auto-mounts the core pack, so this id resolves without a consumer mount.
    inline constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};

    /// @brief Linear float HDR format for the lighting target and the tail's scene-color
    /// intermediates.
    ///
    /// G1 uses the same format as a sampled color target, establishing RGBA16F
    /// color-attachment + sampled support on the platform.
    inline constexpr Format HdrFormat = Format::RGBA16Sfloat;

    /// @brief Usage every HDR scene-color intermediate is allocated with.
    inline constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

    /// @brief Single-channel unorm format for the SSAO target.
    ///
    /// The renderer builds the SSAO pipeline against this format, and SsaoScenePass owns the image.
    inline constexpr Format SsaoFormat = Format::R8Unorm;

    /// @brief Single-channel half-float format for the bloom-mask target.
    ///
    /// The mask is an **amplitude**, not a coverage fraction: it multiplies the filtered scene
    /// color into level 0 of the bloom pyramid, so a value above 1 seeds a halo brighter than the
    /// surface's own radiance. That is the whole point of naming a glow apart from a radiance — a
    /// surface held inside the range the tone curve renders in full saturation still has to reach
    /// the pyramid at any strength — and a normalized format would cap it at exactly the radiance
    /// the surface was pulled down to. The additive blend the translucent pass writes it under is
    /// unbounded here for the same reason.
    inline constexpr Format BloomMaskFormat = Format::R16Sfloat;

    /// @brief Usage the bloom-mask target is allocated with: written as a color attachment, sampled by the bloom down-sweep.
    inline constexpr ImageUsage BloomMaskUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

    /// @brief Maximum per-submesh candidates a frame's per-draw / cull buffers hold.
    ///
    /// The fixed candidate maximum: the indirect buffer covers this many slots, culled ones
    /// no-op. A frame exceeding it is clamped — the overflow submeshes are not drawn, they are
    /// counted per gather phase in SceneRenderer::GetDrawBudgetStats(), and the renderer logs a
    /// warning once for its whole lifetime. The renderer's per-draw buffers and the GPU cull's
    /// candidate/indirect buffers are both sized from it, so it has one owner rather than a copy
    /// per side.
    inline constexpr u32 MaxCullCandidates = 4096;
}
