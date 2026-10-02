#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    // The scene-upscale push block, matching scene_upscale.frag: the source slot, the shared
    // sampler, and this frame's mapping of the destination UV into the source's valid region (one
    // mapping serves both the sample and the clamp) — and, for the paired entry, the bloom mask's
    // slot and its own mapping.
    struct SceneUpscalePush
    {
        u32 SourceTexture;
        u32 Sampler;
        u32 MaskTexture;
        u32 Pad0;
        vec2 ScaleUV;
        vec2 MaxUV;
        vec2 MaskScaleUV;
        vec2 MaskMaxUV;
    };

    /// @brief Which per-frame extent describes the rendered region of a promotion's source.
    enum class PromotionSource : u8
    {
        /// @brief The finished HDR scene colour; its valid region is SceneView::SceneColorExtent.
        SceneColor,
        /// @brief The bloom mask; its valid region is the rasterized SceneView::RenderExtent.
        BloomMask,
    };

    /// @brief One target a promotion carries across the boundary.
    struct Promotion
    {
        /// @brief The sub-rect source id (declared sampled).
        ResourceId SourceId;
        /// @brief The allocation-sized target the pass writes.
        ResourceId OutputId;
        /// @brief Bindless slot for the source.
        TextureHandle SourceHandle;
        /// @brief The allocation the source lives in.
        uvec2 SourceExtent;
        /// @brief Which of the frame's valid extents bounds the source's written region.
        PromotionSource Source = PromotionSource::SceneColor;
    };

    /// @brief Promotes finished scene-side targets to the post-resolve allocation ahead of the tail.
    ///
    /// It sits on the boundary between the render side and the post-resolve tail: the scene chain
    /// finished in the render allocation (or a dynamic-resolution sub-rect of it), and this
    /// resamples it across the post-resolve allocation so the post-process effects, a pre-bloom
    /// overlay, bloom, the metering and the tonemap all run there.
    ///
    /// The scene colour and the bloom mask each cross on their own condition. The scene colour's
    /// promotion runs only when that colour is not already the post-resolve allocation, so an
    /// unscaled frame — and any frame a temporal-upscaling resolve already reconstructed — pays no
    /// pass at all. The bloom mask's runs whenever the rasterized sub-rect is not that allocation,
    /// which a temporal resolve does nothing about: it reconstructs the colour and leaves the mask
    /// the translucent pass wrote on the scene side. When both run, one pass carries both as two
    /// attachments; otherwise a pass carries the one that runs.
    class SceneUpscaleScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context  Renderer context for bindless access.
        /// @param pipeline The fullscreen upscale pipeline: one attachment per promotion carried.
        /// @param primary  The promotion written to the first attachment.
        /// @param mask     The bloom mask promoted beside a scene-colour @p primary as the second
        ///                 attachment (the paired pipeline), or none.
        /// @param sampler  Shared linear clamp-to-edge sampler slot.
        /// @param extent   The post-resolve allocation this pass writes; updated via Resize.
        SceneUpscaleScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                              const Promotion& primary, const optional<Promotion>& mask,
                              SamplerHandle sampler, uvec2 extent)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_Primary(primary), m_Mask(mask),
              m_Sampler(sampler), m_Extent(extent)
        {
        }

        /// @brief Updates the post-resolve allocation extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the upscale pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The fullscreen upscale pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The promotion written to the first attachment.
        Promotion m_Primary;
        /// @brief The bloom mask carried beside the scene colour, when the pass pairs them.
        optional<Promotion> m_Mask;
        /// @brief Shared sampler bindless slot.
        SamplerHandle m_Sampler;
        /// @brief The post-resolve allocation extent.
        uvec2 m_Extent;
    };
}
