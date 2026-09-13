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

    // The scene-upscale push block, matching scene_upscale.frag: the sub-rect scene-color slot,
    // the shared sampler, and this frame's sub-rect mapping (source and destination share one
    // allocation extent, so one mapping serves both the sample and the clamp).
    struct SceneUpscalePush
    {
        u32 SourceTexture;
        u32 Sampler;
        u32 Pad0;
        u32 Pad1;
        vec2 ScaleUV;
        vec2 MaxUV;
    };

    /// @brief Promotes the sub-rect scene color to the full allocation ahead of the HDR tail.
    ///
    /// The non-temporal counterpart of the temporal resolve, sitting at the same anchor: the scene
    /// rasterized into the frame's dynamic-resolution sub-rect, and this resamples it across the
    /// allocation so bloom, the point fields, a pre-bloom overlay, the metering and the tonemap all
    /// run at the allocation. Wired only for a frame actually rendering below its allocation scale,
    /// so a full-scale frame pays no pass at all.
    class SceneUpscaleScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context      Renderer context for bindless access.
        /// @param pipeline     The fullscreen upscale pipeline.
        /// @param sourceId     The sub-rect scene-color source id (declared sampled).
        /// @param outputId     The allocation-sized scene-color target this pass writes.
        /// @param sourceHandle Bindless slot for the sub-rect source.
        /// @param sampler      Shared linear clamp-to-edge sampler slot.
        /// @param extent       The allocation extent; updated via Resize.
        SceneUpscaleScenePass(Context& context, Ref<GraphicsPipeline> pipeline, ResourceId sourceId,
                              ResourceId outputId, TextureHandle sourceHandle,
                              SamplerHandle sampler, uvec2 extent)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_SourceId(sourceId),
              m_OutputId(outputId), m_SourceHandle(sourceHandle), m_Sampler(sampler),
              m_Extent(extent)
        {
        }

        /// @brief Updates the allocation extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the upscale pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The fullscreen upscale pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The sub-rect scene-color source id.
        ResourceId m_SourceId;
        /// @brief The allocation-sized scene-color target.
        ResourceId m_OutputId;
        /// @brief Bindless slot for the sub-rect source.
        TextureHandle m_SourceHandle;
        /// @brief Shared sampler bindless slot.
        SamplerHandle m_Sampler;
        /// @brief The allocation extent.
        uvec2 m_Extent;
    };
}
