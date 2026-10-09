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

    // The grab copy push block, matching scene_color_copy.frag PushConstants: the source slot, the
    // shared sampler, and the frame's dynamic-resolution sub-rect mapping (source and destination
    // share one allocation extent, so one mapping serves the sample and the clamp).
    struct SceneColorCopyPush
    {
        u32 SourceTexture;
        u32 Sampler;
        u32 Pad0;
        u32 Pad1;
        vec2 ScaleUV;
        vec2 MaxUV;
    };

    /// @brief Construction parameters for a SceneColorCopyScenePass.
    struct SceneColorCopyPassInfo
    {
        /// @brief The graph pass name (what DidRecordPassLastFrame reports).
        string_view Name;
        /// @brief The single-attachment copy pipeline writing the target's format.
        Ref<GraphicsPipeline> Pipeline;
        /// @brief Source id (declared sampled for barrier order).
        ResourceId SourceId;
        /// @brief Bindless slot for the source.
        TextureHandle SourceHandle;
        /// @brief The grab target this pass writes.
        ResourceId TargetId;
        /// @brief What the target clears to, which a texel outside the rendered sub-rect reads.
        ClearColor Clear;
        /// @brief Shared sampler bindless slot.
        SamplerHandle Sampler;
        /// @brief Initial render extent; updated via Resize.
        uvec2 Extent{0, 0};
        /// @brief The frame's flag, set when a draw this frame samples the target; the pass skips
        ///        every other frame.
        const bool* Sampled = nullptr;
    };

    /// @brief Copies one pre-translucent source — the lit scene color or the opaque depth — into its
    ///        grab intermediate.
    ///
    /// Runs ahead of the translucent pass so a translucent fragment can sample the opaque scene
    /// behind itself — and depth-test its distorted samples — through the view block's SceneColor
    /// handles. A fullscreen draw (not a transfer): the source is sampled through its sub-rect
    /// mapping, so the copy is correct under dynamic resolution. The colour and the depth are
    /// separate passes so a frame whose materials read only the depth records only its copy.
    class SceneColorCopyScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context Renderer context for bindless access.
        /// @param info    The pass's source, target, pipeline and per-frame flag.
        SceneColorCopyScenePass(Context& context, const SceneColorCopyPassInfo& info)
            : m_Context(context), m_Name(info.Name), m_Pipeline(info.Pipeline),
              m_SourceId(info.SourceId), m_SourceHandle(info.SourceHandle),
              m_TargetId(info.TargetId), m_Clear(info.Clear), m_Sampler(info.Sampler),
              m_Extent(info.Extent), m_Sampled(info.Sampled)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the copy pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The graph pass name.
        string m_Name;
        /// @brief The single-attachment copy pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Source id.
        ResourceId m_SourceId;
        /// @brief Bindless slot for the source.
        TextureHandle m_SourceHandle;
        /// @brief The grab target.
        ResourceId m_TargetId;
        /// @brief The target's clear value.
        ClearColor m_Clear;
        /// @brief Shared sampler bindless slot.
        SamplerHandle m_Sampler;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief The frame's target-sampled flag (borrowed from the renderer).
        const bool* m_Sampled;
    };
}
