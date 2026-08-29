#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "../DrawPlan.h"

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    // The half-res depth reduce push block, matching halfres_depth_reduce.frag PushConstants:
    // the full-res opaque depth slot and the full-res valid sub-rect extent the 2x2 taps clamp
    // inside.
    struct HalfResDepthReducePush
    {
        u32 DepthTexture;
        u32 ValidX;
        u32 ValidY;
    };

    /// @brief Reduces the full-res opaque depth into the half-res depth target.
    ///
    /// A fullscreen draw over the half valid sub-rect writing SV_Depth as the farthest
    /// (reverse-Z minimum) of each texel's 2x2 full-res depths, so the half-resolution
    /// translucent layer depth-tests conservatively — a fragment survives wherever any of its
    /// four full-res pixels would show it, and the composite's depth-aware upsample resolves
    /// the edges the conservative test lets through.
    class HalfResDepthReduceScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context     Renderer context for bindless access.
        /// @param pipeline    The depth-reduce pipeline (no color, depth write, compare Always).
        /// @param depthId     Full-res opaque depth source id (declared sampled).
        /// @param depthHandle Bindless slot for the full-res opaque depth.
        /// @param targetId    The half-res depth target this pass writes.
        /// @param plan        The layer's draw plan — a frame that routed nothing skips the draw.
        HalfResDepthReduceScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                    ResourceId depthId, TextureHandle depthHandle,
                                    ResourceId targetId, const TranslucentDrawPlan* plan)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_DepthId(depthId),
              m_DepthHandle(depthHandle), m_TargetId(targetId), m_Plan(plan)
        {
        }

        /// @brief Updates the render extent (unused; the viewport reads the per-frame view).
        void Resize(uvec2 /*extent*/) override {}
        /// @brief Contributes the depth-reduce pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The depth-reduce pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Full-res opaque depth source id.
        ResourceId m_DepthId;
        /// @brief Bindless slot for the full-res opaque depth.
        TextureHandle m_DepthHandle;
        /// @brief The half-res depth target.
        ResourceId m_TargetId;
        /// @brief Borrowed per-frame half-res draw plan (only its emptiness is read).
        const TranslucentDrawPlan* m_Plan = nullptr;
    };
}
