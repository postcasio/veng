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

    /// @brief Promotes the finished scene color to the post-resolve allocation ahead of the tail.
    ///
    /// It sits on the boundary between the render side and the post-resolve tail: the scene chain
    /// finished in the render allocation (or a dynamic-resolution sub-rect of it), and this
    /// resamples it across the post-resolve allocation so the post-process effects, a pre-bloom
    /// overlay, bloom, the metering and the tonemap all run there. Wired only when the scene color
    /// is not already that allocation, so an unscaled frame — and any frame a temporal-upscaling
    /// resolve already reconstructed — pays no pass at all.
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
        /// @param sourceExtent The allocation the source scene color lives in.
        /// @param extent       The post-resolve allocation this pass writes; updated via Resize.
        SceneUpscaleScenePass(Context& context, Ref<GraphicsPipeline> pipeline, ResourceId sourceId,
                              ResourceId outputId, TextureHandle sourceHandle,
                              SamplerHandle sampler, uvec2 sourceExtent, uvec2 extent)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_SourceId(sourceId),
              m_OutputId(outputId), m_SourceHandle(sourceHandle), m_Sampler(sampler),
              m_SourceExtent(sourceExtent), m_Extent(extent)
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
        /// @brief The allocation the source scene color lives in.
        uvec2 m_SourceExtent;
        /// @brief The post-resolve allocation extent.
        uvec2 m_Extent;
    };
}
