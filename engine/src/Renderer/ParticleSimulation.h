#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>

#include "ParticleGather.h"
#include "Passes/TranslucentScenePass.h"

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Buffer;
    class ComputePipeline;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class PipelineLayout;

    /// @brief What the particle pool's GPU reported, frames-in-flight late, and the frame's sort.
    struct ParticleGpuStats
    {
        /// @brief Particles alive after the last reported update.
        u32 Live = 0;
        /// @brief Particles spawned since the pool was allocated.
        u32 Spawned = 0;
        /// @brief Spawns dropped since the pool was allocated because no slot was free.
        u32 Dropped = 0;
        /// @brief Alpha particles the last reported frame drew.
        u32 AlphaCount = 0;
        /// @brief The length the last reported frame's sort ran over; 0 when it sorted nothing.
        u32 AlphaSorted = 0;
    };

    /// @brief The renderer's particle battery: the pool, its lists and rings, and the compute chain
    ///        that spawns, ages, culls and sorts it.
    ///
    /// Owns every buffer the chain reads and writes, its pipelines and its graph contribution, and
    /// is created when particles are first wired and destroyed when they are unwired, so a renderer
    /// that never sees an effect allocates none of it. Each particle's state is written once, when
    /// it spawns; the update evaluates the closed-form trajectory at the particle's age and writes
    /// nothing back to the pool. The persistent buffers are imported into the graph, so every
    /// barrier between the passes is derived from their declared uses; only the sort's successive
    /// dispatches inside one pass record their own.
    ///
    /// The chain, in declaration order: Particle Reset (the first frame after allocation), Particle
    /// Spawn, Particle Prepare, Particle Update, Particle Sort and Particle Finalize. Whether each
    /// records is decided on the CPU each frame from the gather's exact bounds (Prepare), so a wired
    /// but idle chain records no dispatch.
    class ParticleSimulation
    {
    public:
        /// @brief Allocates a pool of @p capacity particles and builds the chain's pipelines.
        /// @param context   Renderer context.
        /// @param assets    The asset manager the core particle shaders load through.
        /// @param capacity  The pool's capacity: a power of two, at most DefaultParticleCapacity.
        /// @param forward   The renderer's IBL and shadow sets, whose layouts the update binds at 4
        ///                  and 5 to gather six-way light.
        /// @return The battery.
        [[nodiscard]] static Unique<ParticleSimulation> Create(Context& context,
                                                               AssetManager& assets, u32 capacity,
                                                               const ForwardLightingSets& forward);

        /// @brief Releases the buffers and pipelines.
        ~ParticleSimulation();

        ParticleSimulation(const ParticleSimulation&) = delete;
        ParticleSimulation& operator=(const ParticleSimulation&) = delete;

        /// @brief Returns the pool's capacity.
        [[nodiscard]] u32 GetCapacity() const { return m_Capacity; }

        /// @brief Uploads a frame's plan into this frame's rings and decides what records.
        ///
        /// Call once per Execute, after the gather and before the graph replays. Reads the stats
        /// this frame slot's finalize wrote frames-in-flight ago first.
        /// @param plan  The frame's gather.
        void Prepare(const ParticleFramePlan& plan);

        /// @brief Advances the chain's frame-to-frame state once the graph has replayed.
        void Commit();

        /// @brief Declares the chain's imports and compute passes.
        /// @param graph    The renderer's graph being built.
        /// @param io       The pass wiring (the shadow atlases the update samples).
        /// @param forward  The renderer's IBL and shadow sets, bound by the update.
        void Declare(RenderGraph& graph, const PassIO& io, const ForwardLightingSets& forward);

        /// @brief Appends the persistent buffers' import bindings.
        /// @param bindings  The frame's bindings.
        void AppendImportBindings(vector<RenderGraph::ImportBinding>& bindings) const;

        /// @brief Returns whether this frame draws: the update ran.
        [[nodiscard]] bool IsDrawing() const { return m_UpdateRuns; }

        /// @brief Returns the set the draw binds at set 3 for a frame slot.
        [[nodiscard]] const Ref<DescriptorSet>& GetSet(u32 frame) const { return m_Sets[frame]; }

        /// @brief Returns the chain's set-3 layout, shared by the draw.
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetSetLayout() const { return m_SetLayout; }

        /// @brief Returns the draw-argument buffer: the alpha draw at byte 16, the additive at 32.
        [[nodiscard]] const Ref<Buffer>& GetArgs() const { return m_Args; }

        /// @brief Returns the draw records' graph id.
        [[nodiscard]] ResourceId GetDrawsId() const { return m_DrawsId; }

        /// @brief Returns the sorted alpha keys' graph id.
        [[nodiscard]] ResourceId GetAlphaKeysId() const { return m_AlphaKeysId; }

        /// @brief Returns the additive list's graph id.
        [[nodiscard]] ResourceId GetAdditiveId() const { return m_AdditiveId; }

        /// @brief Returns the argument buffer's graph id.
        [[nodiscard]] ResourceId GetArgsId() const { return m_ArgsId; }

        /// @brief Returns what the GPU last reported.
        [[nodiscard]] const ParticleGpuStats& GetStats() const { return m_Stats; }

        /// @brief Byte offset of the alpha draw's VkDrawIndirectCommand.
        static constexpr u64 AlphaDrawOffset = 16;
        /// @brief Byte offset of the additive draw's VkDrawIndirectCommand.
        static constexpr u64 AdditiveDrawOffset = 32;

    private:
        /// @brief Constructs; use Create.
        ParticleSimulation(Context& context, AssetManager& assets, u32 capacity,
                           const ForwardLightingSets& forward);

        /// @brief Builds the update pipeline against the forward sets' layouts.
        void BuildUpdatePipeline(const ForwardLightingSets& forward);

        /// @brief Binds a compute pipeline, the bindless sets and this frame's chain set.
        void BindCompute(CommandBuffer& cmd, const Ref<ComputePipeline>& pipeline) const;

        /// @brief Records the sort's dispatches.
        void RecordSort(CommandBuffer& cmd) const;

        /// @brief Renderer context.
        Context& m_Context;
        /// @brief The asset manager the shaders load through.
        AssetManager& m_Assets;
        /// @brief The pool's capacity N.
        u32 m_Capacity;
        /// @brief Frames in flight, the depth of every ring.
        u32 m_FramesInFlight;

        /// @brief N particles.
        Ref<Buffer> m_Particles;
        /// @brief N free slots.
        Ref<Buffer> m_DeadList;
        /// @brief Two halves of N live slots.
        Ref<Buffer> m_LiveList;
        /// @brief N additive draw indices.
        Ref<Buffer> m_AdditiveList;
        /// @brief max(N, 1024) alpha sort keys.
        Ref<Buffer> m_AlphaKeys;
        /// @brief N draw records.
        Ref<Buffer> m_Draws;
        /// @brief The chain's counters.
        Ref<Buffer> m_Counters;
        /// @brief The update's dispatch and the two draws' arguments.
        Ref<Buffer> m_Args;
        /// @brief Host-mapped rings, one region per frame in flight: instances, emitters, spawn
        ///        ranges, stats and frame scales.
        Ref<Buffer> m_Instances;
        /// @brief See m_Instances.
        Ref<Buffer> m_Emitters;
        /// @brief See m_Instances.
        Ref<Buffer> m_Ranges;
        /// @brief See m_Instances.
        Ref<Buffer> m_StatsRing;
        /// @brief See m_Instances.
        Ref<Buffer> m_FrameScales;

        /// @brief The chain's set-3 layout.
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief One set per frame in flight, bound to its own ring regions.
        vector<Ref<DescriptorSet>> m_Sets;
        /// @brief The layout every compute stage but the update uses.
        Ref<PipelineLayout> m_ComputeLayout;
        /// @brief The update's layout, with the IBL and shadow sets at 4 and 5.
        Ref<PipelineLayout> m_UpdateLayout;
        /// @brief The forward layouts the update layout was built against.
        Ref<DescriptorSetLayout> m_UpdateIblLayout;
        /// @brief See m_UpdateIblLayout.
        Ref<DescriptorSetLayout> m_UpdateShadowLayout;
        /// @brief The chain's pipelines.
        Ref<ComputePipeline> m_ResetPipeline;
        /// @brief See m_ResetPipeline.
        Ref<ComputePipeline> m_SpawnPipeline;
        /// @brief See m_ResetPipeline.
        Ref<ComputePipeline> m_PreparePipeline;
        /// @brief See m_ResetPipeline.
        Ref<ComputePipeline> m_UpdatePipeline;
        /// @brief See m_ResetPipeline.
        Ref<ComputePipeline> m_SortLocalPipeline;
        /// @brief See m_ResetPipeline.
        Ref<ComputePipeline> m_SortGlobalPipeline;
        /// @brief See m_ResetPipeline.
        Ref<ComputePipeline> m_SortMergePipeline;
        /// @brief See m_ResetPipeline.
        Ref<ComputePipeline> m_FinalizePipeline;

        /// @brief The graph ids of the imported buffers.
        ResourceId m_ParticlesId;
        /// @brief See m_ParticlesId.
        ResourceId m_DeadListId;
        /// @brief See m_ParticlesId.
        ResourceId m_LiveListId;
        /// @brief See m_ParticlesId.
        ResourceId m_AdditiveId;
        /// @brief See m_ParticlesId.
        ResourceId m_AlphaKeysId;
        /// @brief See m_ParticlesId.
        ResourceId m_DrawsId;
        /// @brief See m_ParticlesId.
        ResourceId m_CountersId;
        /// @brief See m_ParticlesId.
        ResourceId m_ArgsId;

        /// @brief The IBL and shadow sets the update binds, refreshed each Declare.
        ForwardLightingSets m_Forward;

        /// @brief Whether the pool still needs its first reset.
        bool m_NeedsReset = true;
        /// @brief Whether a particle may still be alive on the GPU.
        bool m_GpuMayHoldLive = false;
        /// @brief Which live-list half holds the particles going into this frame's update.
        u32 m_Cur = 0;
        /// @brief This frame's spawn threads and ranges.
        u32 m_SpawnTotal = 0;
        /// @brief See m_SpawnTotal.
        u32 m_RangeCount = 0;
        /// @brief This frame's gather bound on the particles alive after its spawns.
        u32 m_AliveBound = 0;
        /// @brief Whether this frame's update (and prepare, finalize and draw) runs.
        bool m_UpdateRuns = false;
        /// @brief This frame's sort length P; 0 when nothing sorts.
        u32 m_SortSize = 0;
        /// @brief Counts Prepares, ordering the stats each slot reports.
        u64 m_Sequence = 0;
        /// @brief The Prepare whose finalize each frame slot last ran; 0 when it ran none.
        vector<u64> m_SlotSequence;
        /// @brief The Prepare whose stats m_Stats holds.
        u64 m_StatsSequence = 0;
        /// @brief What the GPU last reported.
        ParticleGpuStats m_Stats;
    };
}
