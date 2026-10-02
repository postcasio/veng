#pragma once

#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Veng.h>

#include <span>
#include <string>

namespace Veng::Renderer
{
    class ComputePipeline;
    class DescriptorSet;

    // The coarse-tail push block, matching scene_color_downsample_tail.comp PushConstants: this
    // frame's valid level-0 extent, the chain's allocated level-0 extent, and the tail's first level
    // and length (every level's sub-rect map is derived from these).
    struct SceneColorDownsampleTailPush
    {
        uvec2 RenderExtent;
        uvec2 AllocBase;
        u32 FirstLevel;
        u32 LevelCount;
    };

    /// @brief Halves the refraction grab's coarse tail — its smallest levels — in one dispatch.
    ///
    /// Stands in for the SceneColorDownsampleScenePass of every level from the tail's first on: one
    /// workgroup computes them in order in shared memory, each exactly as that pass would, and writes
    /// each level whole (its valid sub-rect, and the clear outside it).
    class SceneColorDownsampleTailScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the tail pass.
        /// @param pipeline   The tail compute pipeline.
        /// @param set        The tail set: the level before the tail sampled, a storage view per slot.
        /// @param sourceId   The level before the tail (declared sampled for barrier order).
        /// @param levelIds   The tail's levels, first to coarsest (declared storage-written).
        /// @param firstLevel The tail's first level, 1 or deeper.
        /// @param extent     The grab's base allocation extent; updated via Resize.
        SceneColorDownsampleTailScenePass(Ref<ComputePipeline> pipeline, Ref<DescriptorSet> set,
                                          ResourceId sourceId, std::span<const ResourceId> levelIds,
                                          u32 firstLevel, uvec2 extent);

        /// @brief Updates the grab's base allocation extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the tail's one compute pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The tail compute pipeline.
        Ref<ComputePipeline> m_Pipeline;
        /// @brief The tail descriptor set.
        Ref<DescriptorSet> m_Set;
        /// @brief The level before the tail.
        ResourceId m_SourceId;
        /// @brief The tail's levels, first to coarsest.
        vector<ResourceId> m_LevelIds;
        /// @brief The tail's first level.
        u32 m_FirstLevel;
        /// @brief The grab's base allocation extent.
        uvec2 m_Extent;
        /// @brief This pass's graph name, owned here because AddComputePass takes a view.
        string m_Name;
    };
}
