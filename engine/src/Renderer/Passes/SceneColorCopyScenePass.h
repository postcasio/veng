#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    // The refraction scene-color copy push block, matching scene_color_copy.frag
    // PushConstants: the lit source and opaque depth slots, the shared sampler, and the
    // frame's dynamic-resolution sub-rect mapping (sources and destinations share one
    // allocation extent, so one mapping serves the sample and the clamp).
    struct SceneColorCopyPush
    {
        u32 SourceTexture;
        u32 DepthTexture;
        u32 Sampler;
        u32 Pad0;
        vec2 ScaleUV;
        vec2 MaxUV;
    };

    /// @brief Copies the lit scene color and opaque depth into the refraction intermediates.
    ///
    /// Runs ahead of the translucent pass so a translucent fragment can sample the opaque scene
    /// behind itself — and depth-test its distorted samples — through the view block's SceneColor
    /// handles. A fullscreen MRT draw (not a transfer): the sources are sampled through their
    /// sub-rect mapping, so the copy is correct under dynamic resolution.
    class SceneColorCopyScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context      Renderer context for bindless access.
        /// @param pipeline     The two-attachment scene-color copy pipeline.
        /// @param sourceId     Lit scene-color source id (declared sampled for barrier order).
        /// @param sourceHandle Bindless slot for the lit source.
        /// @param depthId      Opaque depth source id (declared sampled).
        /// @param depthHandle  Bindless slot for the opaque depth.
        /// @param copyId       The scene-color grab target this pass writes.
        /// @param depthCopyId  The depth-copy target this pass writes.
        /// @param sampler      Shared sampler bindless slot.
        /// @param extent       Initial render extent; updated via Resize.
        SceneColorCopyScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                ResourceId sourceId, TextureHandle sourceHandle, ResourceId depthId,
                                TextureHandle depthHandle, ResourceId copyId,
                                ResourceId depthCopyId, SamplerHandle sampler, uvec2 extent)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_SourceId(sourceId),
              m_SourceHandle(sourceHandle), m_DepthId(depthId), m_DepthHandle(depthHandle),
              m_CopyId(copyId), m_DepthCopyId(depthCopyId), m_Sampler(sampler), m_Extent(extent)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the scene-color copy pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The two-attachment scene-color copy pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Lit scene-color source id.
        ResourceId m_SourceId;
        /// @brief Bindless slot for the lit source.
        TextureHandle m_SourceHandle;
        /// @brief Opaque depth source id.
        ResourceId m_DepthId;
        /// @brief Bindless slot for the opaque depth.
        TextureHandle m_DepthHandle;
        /// @brief The scene-color grab target.
        ResourceId m_CopyId;
        /// @brief The depth-copy target.
        ResourceId m_DepthCopyId;
        /// @brief Shared sampler bindless slot.
        SamplerHandle m_Sampler;
        /// @brief Current render extent.
        uvec2 m_Extent;
    };
}
