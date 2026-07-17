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

    // The TAA resolve push block, matching taa_resolve.frag PushConstants: the
    // current/history/depth bindless slots, the shared sampler, the view-constants
    // region, the history-validity flag, and the target extent.
    struct TaaResolvePush
    {
        u32 CurrentTexture;
        u32 HistoryTexture;
        u32 DepthTexture;
        u32 VelocityTexture;
        u32 Sampler;
        u32 ViewConstantsIndex;
        u32 HistoryValid;
        // Pads to an even u32 count so the trailing uint2 lands at its 8-byte-aligned
        // offset, matching the SPIR-V push block layout.
        u32 Pad0;
        uvec2 Extent;
    };

    // The TAA history-copy push block, matching taa_history_copy.frag PushConstants:
    // the resolved source slot and the shared sampler.
    struct TaaCopyPush
    {
        u32 SourceTexture;
        u32 Sampler;
    };

    /// @brief Temporal anti-aliasing resolve + history-copy pass.
    ///
    /// A resolve pass that reprojects the persisted history against this frame's depth and blends
    /// it with the lit color, then a history-copy pass that refreshes the history from the
    /// resolved result for next frame.
    ///
    /// The renderer routes the lighting pass into a separate lit target (litId); this pass reads
    /// it as the current frame, writes the resolved color into the HDR target (hdrId) the rest of
    /// the chain already samples, and copies that back into historyId.
    class TaaScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context          Renderer context for bindless access.
        /// @param resolvePipeline  The TAA resolve pipeline.
        /// @param copyPipeline     The TAA history-copy pipeline.
        /// @param litId            The separate lit target (current frame input).
        /// @param historyId        The persisted history target.
        /// @param hdrId            The HDR target the resolved color is written into.
        /// @param depthId          The depth target for reprojection fallback.
        /// @param velocityId       The per-object velocity target.
        /// @param litHandle        Bindless slot for the lit target.
        /// @param historyHandle    Bindless slot for the history target.
        /// @param velocityHandle   Bindless slot for the velocity target.
        /// @param historyResetPtr  Points at the renderer's history-reset flag, read at record time.
        /// @param extent           Initial render extent; updated via Resize.
        TaaScenePass(Context& context, Ref<GraphicsPipeline> resolvePipeline,
                     Ref<GraphicsPipeline> copyPipeline, ResourceId litId, ResourceId historyId,
                     ResourceId hdrId, ResourceId depthId, ResourceId velocityId,
                     TextureHandle litHandle, TextureHandle historyHandle,
                     TextureHandle velocityHandle, const bool* historyResetPtr, uvec2 extent)
            : m_Context(context), m_ResolvePipeline(std::move(resolvePipeline)),
              m_CopyPipeline(std::move(copyPipeline)), m_LitId(litId), m_HistoryId(historyId),
              m_HdrId(hdrId), m_DepthId(depthId), m_VelocityId(velocityId), m_LitHandle(litHandle),
              m_HistoryHandle(historyHandle), m_VelocityHandle(velocityHandle),
              m_HistoryResetPtr(historyResetPtr), m_Extent(extent)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the resolve and history-copy passes into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The TAA resolve pipeline.
        Ref<GraphicsPipeline> m_ResolvePipeline;
        /// @brief The TAA history-copy pipeline.
        Ref<GraphicsPipeline> m_CopyPipeline;
        /// @brief The separate lit target (current frame input).
        ResourceId m_LitId;
        /// @brief The persisted history target.
        ResourceId m_HistoryId;
        /// @brief The HDR target the resolved color is written into.
        ResourceId m_HdrId;
        /// @brief The depth target for reprojection fallback.
        ResourceId m_DepthId;
        /// @brief The per-object velocity target.
        ResourceId m_VelocityId;
        /// @brief Bindless slot for the lit target.
        TextureHandle m_LitHandle;
        /// @brief Bindless slot for the history target.
        TextureHandle m_HistoryHandle;
        /// @brief Bindless slot for the velocity target.
        TextureHandle m_VelocityHandle;
        // Points at SceneRenderer::m_TaaHistoryReset, read at record time (the graph
        // executes before Execute clears the flag, so it reflects this frame's validity).
        const bool* m_HistoryResetPtr;
        /// @brief Current render extent.
        uvec2 m_Extent;
    };
}
