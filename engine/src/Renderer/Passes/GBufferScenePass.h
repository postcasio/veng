#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>

#include "../DrawPlan.h"
#include "../FrameTopology.h"

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class GBufferShadingOverride;
    class InstancedMeshSystem;
    class SphereTerrainSystem;
    class SurfaceShadowPipelines;

    /// @brief Records a draw group's instanced runs: one DrawIndexed per run.
    ///
    /// Each run draws its first slot's index range with one instance per slot, firstInstance the
    /// first slot's candidate id, so instance i reads candidate firstInstance + i. The caller has
    /// bound the group's pipeline, sets, and mesh buffers.
    /// @param cmd   The command buffer to record into.
    /// @param slots The plan's slots the runs index.
    /// @param runs  The plan's runs the group indexes.
    /// @param group The group whose runs to draw.
    void RecordInstanceRuns(CommandBuffer& cmd, std::span<const DrawSlot> slots,
                            std::span<const InstanceRun> runs, const DrawGroup& group);

    /// @brief Records a draw group's GPU-culled draws from the cull-written command run.
    ///
    /// Count-driven where the plan says so (as many draws as the cull counted for the group),
    /// otherwise every slot of the run, a culled one drawing nothing. The caller has bound the
    /// group's pipeline, sets, and mesh buffers.
    /// @param cmd   The command buffer to record into.
    /// @param plan  The frame's draw plan, under CullMode::GPU.
    /// @param group The group to draw.
    void RecordGroupIndirect(CommandBuffer& cmd, const GBufferDrawPlan& plan,
                             const DrawGroup& group);

    /// @brief Records the deferred g-buffer geometry pass from the per-frame draw plan.
    ///
    /// Rasterizes the static and skinned survivors into the five g-buffer MRT channels
    /// plus depth, binding each draw group's own pipeline. Under CullMode::GPU the static
    /// draws issue one DrawIndexedIndirect per group over the cull-written command run;
    /// under CullMode::CPU a direct DrawIndexed per surviving slot. Under the shading override each
    /// group binds its material's override pipeline instead of the material's own, and records the
    /// same draws. A material whose fragment reads the shadow maps binds the pipeline built for it
    /// against the renderer's shadow layout, and the shadow set at SurfaceShadowMapSet, so the pass
    /// declares the shadow atlases sampled.
    class GBufferScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context     Renderer context for bindless access.
        /// @param extent      Initial render extent; updated via Resize.
        /// @param plan        Borrowed per-frame draw plan the renderer fills before each replay.
        /// @param cull        Active cull mode selecting the submission shape.
        /// @param indirectId  The cull-written indirect-command buffer id (GPU mode).
        /// @param countId     The cull-written draw-count buffer id (GPU mode, count-driven).
        /// @param stores      Which colour channels the pass stores; the rest are discarded.
        /// @param shadingOverride The prepared override pipelines to draw through, or null to bind
        ///                    each group's own material pipeline.
        /// @param instanced   The instanced mesh sets to draw after the entity draws, or null while
        ///                    the scene carries none.
        /// @param terrain     The renderer's sphere terrains, drawn after the entity draws.
        /// @param shadowReaders Binds each material's pipeline, and the shadow set for a material
        ///                    reading the shadow maps.
        GBufferScenePass(Context& context, uvec2 extent, const GBufferDrawPlan* plan,
                         SceneRendererSettings::CullMode cull, ResourceId indirectId,
                         ResourceId countId, const GBufferChannelStates& stores,
                         const GBufferShadingOverride* shadingOverride,
                         const InstancedMeshSystem* instanced, const SphereTerrainSystem* terrain,
                         SurfaceShadowPipelines& shadowReaders)
            : m_Context(context), m_Extent(extent), m_Plan(plan), m_Cull(cull),
              m_IndirectId(indirectId), m_CountId(countId), m_Stores(stores),
              m_ShadingOverride(shadingOverride), m_Instanced(instanced), m_Terrain(terrain),
              m_ShadowReaders(&shadowReaders)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the g-buffer geometry pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Records the static and skinned draws into the bound g-buffer attachments.
        void Record(const ScenePassContext& ctx) const;

        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief Borrowed per-frame draw plan.
        const GBufferDrawPlan* m_Plan = nullptr;
        /// @brief Active cull mode.
        SceneRendererSettings::CullMode m_Cull = SceneRendererSettings::CullMode::CPU;
        /// @brief The indirect-command buffer id read under CullMode::GPU.
        ResourceId m_IndirectId;
        /// @brief The draw-count buffer id read under a count-driven CullMode::GPU, or empty.
        ResourceId m_CountId;
        /// @brief Which colour channels the pass stores for a later reader.
        GBufferChannelStates m_Stores;
        /// @brief The override pipelines drawn through in place of the materials', or null.
        const GBufferShadingOverride* m_ShadingOverride = nullptr;
        /// @brief The instanced mesh sets drawn after the entity draws, or null.
        const InstancedMeshSystem* m_Instanced = nullptr;
        /// @brief The sphere terrains drawn after the entity draws.
        const SphereTerrainSystem* m_Terrain = nullptr;
        /// @brief Binds the materials' pipelines, and the shadow set for one reading it.
        SurfaceShadowPipelines* m_ShadowReaders = nullptr;
    };
}
