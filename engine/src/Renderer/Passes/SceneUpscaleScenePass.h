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

    // The scene-upscale push block, matching scene_upscale.frag: the scene-color slot, the shared
    // sampler, and this frame's mapping of the destination UV into the source's valid region (one
    // mapping serves both the sample and the clamp).
    struct SceneUpscalePush
    {
        u32 SourceTexture;
        u32 Sampler;
        u32 Pad0;
        u32 Pad1;
        vec2 ScaleUV;
        vec2 MaxUV;
    };

    /// @brief Which per-frame extent describes the rendered region of a promotion's source.
    enum class PromotionSource : u8
    {
        /// @brief The finished HDR scene colour; its valid region is SceneView::SceneColorExtent.
        SceneColor,
        /// @brief The bloom mask; its valid region is the rasterized SceneView::RenderExtent.
        BloomMask,
    };

    /// @brief Promotes a finished scene-side target to the post-resolve allocation ahead of the tail.
    ///
    /// It sits on the boundary between the render side and the post-resolve tail: the scene chain
    /// finished in the render allocation (or a dynamic-resolution sub-rect of it), and this
    /// resamples it across the post-resolve allocation so the post-process effects, a pre-bloom
    /// overlay, bloom, the metering and the tonemap all run there.
    ///
    /// Two instances are wired, one per @ref PromotionSource, each on its own condition. The scene
    /// colour's runs only when that colour is not already the post-resolve allocation, so an
    /// unscaled frame — and any frame a temporal-upscaling resolve already reconstructed — pays no
    /// pass at all. The bloom mask's runs whenever the rasterized sub-rect is not that allocation,
    /// which a temporal resolve does nothing about: it reconstructs the colour and leaves the mask
    /// the translucent pass wrote on the scene side.
    class SceneUpscaleScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context      Renderer context for bindless access.
        /// @param pipeline     The fullscreen upscale pipeline.
        /// @param sourceId     The sub-rect scene-color source id (declared sampled).
        /// @param outputId     The allocation-sized scene-color target this pass writes.
        /// @param sourceHandle Bindless slot for the source.
        /// @param sampler      Shared linear clamp-to-edge sampler slot.
        /// @param sourceExtent The allocation the source lives in.
        /// @param extent       The post-resolve allocation this pass writes; updated via Resize.
        /// @param source       Which of the frame's valid extents bounds the source's written region.
        SceneUpscaleScenePass(Context& context, Ref<GraphicsPipeline> pipeline, ResourceId sourceId,
                              ResourceId outputId, TextureHandle sourceHandle,
                              SamplerHandle sampler, uvec2 sourceExtent, uvec2 extent,
                              PromotionSource source)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_SourceId(sourceId),
              m_OutputId(outputId), m_SourceHandle(sourceHandle), m_Sampler(sampler),
              m_SourceExtent(sourceExtent), m_Extent(extent), m_Source(source)
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
        /// @brief The scene-color source id.
        ResourceId m_SourceId;
        /// @brief The post-resolve-allocation scene-color target.
        ResourceId m_OutputId;
        /// @brief Bindless slot for the source.
        TextureHandle m_SourceHandle;
        /// @brief Shared sampler bindless slot.
        SamplerHandle m_Sampler;
        /// @brief The allocation the source lives in.
        uvec2 m_SourceExtent;
        /// @brief The post-resolve allocation extent.
        uvec2 m_Extent;
        /// @brief Which of the frame's valid extents bounds the source's written region.
        PromotionSource m_Source;
    };
}
