#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

#include "../DrawPlan.h"

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    /// @brief The entity-id picking pass.
    ///
    /// A depth-tested geometry pass that re-draws the same static + skinned survivors as the
    /// g-buffer pass through the id-writing pipeline variants (surface[_skinned].vert +
    /// entity_id.frag), writing each entity's pick id (DrawData.EntityIndex + 1) into the
    /// R32Uint EntityId target. Its own RenderingInfo binds the EntityId color attachment and a
    /// dedicated depth buffer, so the shipping g-buffer RenderingInfo is untouched. The
    /// pipelines are built lazily, so the pass reads them through pointers to the renderer's
    /// member Refs at record time.
    class PickingScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context         Renderer context for bindless access.
        /// @param extent          Initial render extent; updated via Resize.
        /// @param plan            Borrowed per-frame draw plan (shared with the g-buffer pass).
        /// @param staticPipeline  Pointer to the renderer's lazily-built static id pipeline.
        /// @param skinnedPipeline Pointer to the renderer's lazily-built skinned id pipeline.
        /// @param entityIdId      The R32Uint EntityId target this pass writes.
        /// @param depthId         The dedicated picking depth target.
        PickingScenePass(Context& context, uvec2 extent, const GBufferDrawPlan* plan,
                         const Ref<GraphicsPipeline>* staticPipeline,
                         const Ref<GraphicsPipeline>* skinnedPipeline, ResourceId entityIdId,
                         ResourceId depthId)
            : m_Context(context), m_Extent(extent), m_Plan(plan), m_StaticPipeline(staticPipeline),
              m_SkinnedPipeline(skinnedPipeline), m_EntityIdId(entityIdId), m_DepthId(depthId)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the picking pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Records the static and skinned id draws into the EntityId target.
        void Record(const ScenePassContext& ctx) const;

        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief Borrowed per-frame draw plan.
        const GBufferDrawPlan* m_Plan = nullptr;
        /// @brief The renderer's static id pipeline, read at record time.
        const Ref<GraphicsPipeline>* m_StaticPipeline = nullptr;
        /// @brief The renderer's skinned id pipeline, read at record time.
        const Ref<GraphicsPipeline>* m_SkinnedPipeline = nullptr;
        /// @brief The EntityId color target.
        ResourceId m_EntityIdId;
        /// @brief The dedicated picking depth target.
        ResourceId m_DepthId;
    };
}
