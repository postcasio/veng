#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/HiZHistory.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Veng.h>

#include <vector>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
    class Buffer;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ComputePipeline;
    class PipelineLayout;
    struct GBufferDrawPlan;
    struct GpuCullCandidate;

    /// @brief Owns the GPU occlusion-cull cluster and the hi-Z pyramid it tests against.
    ///
    /// One subsystem because the pyramid and the cull are one feature: the hi-Z pyramid exists to
    /// be tested against, the history-validity gate spans both, and the GPU-band tests exercise
    /// them together. It owns the max-Z reduction (set layout, pipeline layout, compute pipeline,
    /// pyramid image + per-mip storage views + whole-chain sampled view + reduction sets), the
    /// cross-frame history-validity state, the cull compute cluster (candidate/indirect/count
    /// buffers, the compute pipeline/layout/set), the active cull mode after the device-support
    /// fallback, and the per-frame dispatch + readback state.
    ///
    /// The hi-Z reduce pipeline layout and set layout are borrowed by the SSR chain's min-Z reduce
    /// (its pipeline is built on the pipeline layout and its reduce sets allocate from the set
    /// layout), so this subsystem is constructed before the SSR chain, and its layouts are exposed
    /// by reference. The pyramid is recreated from the g-buffer create/recreate tail so its
    /// reduction sets bind the fresh depth view.
    class GpuCullSystem
    {
    public:
        /// @brief Creates the hi-Z reduce and cull pipelines/layouts (the pyramid + sets are built by ResizeHiZ).
        ///
        /// The cull compute cluster (candidate/indirect/count buffers, the compute pipeline, the
        /// set layout) is built only where Context::IsGpuDrivenCullingSupported() is true; the
        /// active cull mode is resolved from @p settings with the device fallback.
        /// @param context  The render context the resources are created on.
        /// @param assets   Asset manager used to load the reduce/cull compute shaders.
        /// @param settings The active renderer settings (Cull selects the mode; the device gates it).
        /// @return A new GpuCullSystem.
        static Unique<GpuCullSystem> Create(Context& context, AssetManager& assets,
                                            const SceneRendererSettings& settings);

        /// @brief Releases the whole-chain hi-Z bindless slot; the images retire through the frame bin.
        ~GpuCullSystem();

        GpuCullSystem(const GpuCullSystem&) = delete;
        GpuCullSystem& operator=(const GpuCullSystem&) = delete;

        /// @brief Recreates the hi-Z pyramid, per-mip views, and reduction sets at @p extent.
        ///
        /// Sized to @p extent with a full mip chain, not cleared (it carries data across frames).
        /// Reduction set k binds mip k's source (@p depthView for k=0, the prior mip otherwise) and
        /// mip k's destination storage view, so the fresh depth view must be passed from the
        /// g-buffer create/recreate tail. Rebinds the cull set (when the GPU cluster exists) against
        /// the fresh pyramid, and forces the next occlusion test to skip history.
        /// @param extent    The g-buffer extent the pyramid is sized to.
        /// @param depthView The live depth target the reduction's mip-0 source binds.
        void ResizeHiZ(uvec2 extent, const Ref<ImageView>& depthView);

        /// @brief Re-resolves the active cull mode from @p settings and the device-support fallback.
        ///
        /// CullMode::GPU survives only where Context::IsGpuDrivenCullingSupported() is true;
        /// otherwise it degrades to CullMode::CPU, logged once. Called at Create and every Configure.
        /// @param settings The active renderer settings.
        void ResolveActiveCullMode(const SceneRendererSettings& settings);

        /// @brief Imports the indirect command buffer into the graph (GPU mode only); returns its id.
        ///
        /// Under CullMode::GPU the cull pass (StorageBufferWrite) and the geometry pass
        /// (IndirectRead) share this import; otherwise the id is empty. Declared before the geometry
        /// pass so the graph derives the write→read barrier.
        /// @param graph The renderer's internal graph being rebuilt.
        /// @return The indirect-buffer import id, or an empty id under CullMode::CPU.
        ResourceId ImportIndirect(RenderGraph& graph);

        /// @brief Imports the hi-Z pyramid mip chain into the graph.
        ///
        /// The cull samples last frame's pyramid and the reduction writes this frame's into the same
        /// per-mip slots. Called on every Rebuild before the cull pass and the reduction.
        /// @param graph The renderer's internal graph being rebuilt.
        void ImportHiZChain(RenderGraph& graph);

        /// @brief Declares the GPU occlusion-cull compute pass into the graph (GPU mode only).
        ///
        /// Writes each indirect command's instanceCount against last frame's pyramid and the uploaded
        /// candidates; declared before the geometry pass so the graph derives the write→read barrier.
        /// @param graph The renderer's internal graph being rebuilt.
        /// @param plan  Borrowed per-frame draw plan the pass reads at record time (cull mode + slots).
        void DeclareCull(RenderGraph& graph, const GBufferDrawPlan* plan);

        /// @brief Declares the max-Z reduction compute chain into the graph after the tail passes.
        ///
        /// One dispatch per mip: mip 0 reads @p depthId, mip n>0 reads pyramid mip n-1, each writing
        /// its hi-Z mip. Declared last so it reduces this frame's completed depth.
        /// @param graph   The renderer's internal graph being rebuilt.
        /// @param depthId The depth import the mip-0 reduction reads.
        void DeclareHiZReduction(RenderGraph& graph, ResourceId depthId);

        /// @brief Returns the mapped candidate span for @p frameIndex's ring region, or null under CPU.
        ///
        /// The renderer-coordinated write surface: PrepareDraws fills one GpuCullCandidate per opaque
        /// static survivor slot through this span. Null when the GPU cluster is absent (CPU mode or
        /// an unsupported device).
        /// @param frameIndex This frame's frame-in-flight index.
        /// @return The candidate span base for this frame, or nullptr under CullMode::CPU.
        [[nodiscard]] GpuCullCandidate* BeginFrameUpload(u32 frameIndex);

        /// @brief Records this frame's cull dispatch parameters and arms the survivor readback (GPU mode).
        ///
        /// Zeroes this frame's survivor count so the one-frame-late readback reflects only this
        /// dispatch, and captures the candidate count, ring bases, previous-frame view-projection,
        /// and the resolved history-valid flag the cull push reads at record time.
        /// @param candidateCount    Opaque static candidate slots filled this frame.
        /// @param frameBase         Candidate/command region base (frameIndex * MaxCullCandidates).
        /// @param frameIndex        This frame's frame-in-flight index (the count-buffer slot).
        /// @param prevViewProj      The previous-frame camera world→clip the cull screen-bounds with.
        /// @param occlusionEnabled  Whether Settings.Occlusion permits the occlusion test this frame.
        void RecordFrame(u32 candidateCount, u32 frameBase, u32 frameIndex,
                         const mat4& prevViewProj, bool occlusionEnabled);

        /// @brief Reads the survivor count the previous Execute wrote into @p frameIndex's region.
        ///
        /// The host-visible count is one frame late, so it never gates this frame's draw. A no-op
        /// when the GPU cluster is absent.
        /// @param frameIndex This frame's frame-in-flight index.
        void ReadSurvivorCount(u32 frameIndex);

        /// @brief Evaluates whether last frame's pyramid is trustworthy this frame.
        ///
        /// Combines the reset gate (frame 0 / post-resize / post-configure) with the device-free
        /// view-delta metric against the previous captured camera state.
        /// @param current       This frame's camera state.
        /// @param sceneDiagonal Length of the world-space scene-bound diagonal (>= 0).
        void EvaluateHistory(const HiZHistoryState& current, f32 sceneDiagonal);

        /// @brief Captures @p current as the previous state and clears the reset gate for next frame.
        /// @param current This frame's camera state.
        void CommitHistory(const HiZHistoryState& current);

        /// @brief The hi-Z reduce pipeline layout the SSR min-Z reduce pipeline builds on.
        [[nodiscard]] const Ref<PipelineLayout>& GetHiZReduceLayout() const
        {
            return m_HiZReduceLayout;
        }

        /// @brief The hi-Z reduce set layout the SSR min-Z reduce sets allocate from.
        [[nodiscard]] const Ref<DescriptorSetLayout>& GetHiZReduceSetLayout() const
        {
            return m_HiZReduceSetLayout;
        }

        /// @brief The cull mode actually in effect, after the device-support fallback.
        [[nodiscard]] SceneRendererSettings::CullMode GetActiveCull() const { return m_ActiveCull; }

        /// @brief The indirect command buffer import id (empty under CullMode::CPU).
        [[nodiscard]] ResourceId GetIndirectId() const { return m_IndirectId; }

        /// @brief The indirect command buffer the geometry pass issues (null when the GPU cluster is absent).
        [[nodiscard]] const Ref<Buffer>& GetIndirectBuffer() const { return m_IndirectBuffer; }

        /// @brief The hi-Z pyramid mip chain import id.
        [[nodiscard]] const MipChainId& GetHiZChainId() const { return m_HiZChainId; }

        /// @brief The whole-chain sampled view of the hi-Z pyramid (registered into bindless).
        [[nodiscard]] const Ref<ImageView>& GetHiZSampleView() const { return m_HiZSampleView; }

        /// @brief The per-mip hi-Z storage views, bound to their per-mip import slots each Execute.
        [[nodiscard]] const std::vector<Ref<ImageView>>& GetHiZMipViews() const
        {
            return m_HiZMips;
        }

        /// @brief Whether the previous-frame pyramid is valid to occlusion-test against this frame.
        [[nodiscard]] bool IsHiZHistoryValid() const { return m_HiZHistoryValid; }

        /// @brief The GPU cull's survivor count read back from the previous Execute (diagnostic).
        [[nodiscard]] u32 GetLastGpuSurvivorCount() const { return m_LastGpuSurvivorCount; }

        /// @brief Reads back the per-candidate instanceCount verdicts from the last GPU Execute.
        ///
        /// One entry per opaque static candidate (in dispatch order), each 1 (drawn) or 0 (occluded),
        /// downloaded from the indirect command buffer. Blocks on a device read; empty when no GPU
        /// Execute has run.
        /// @return The per-candidate verdicts, or empty when no GPU Execute has filled a region.
        [[nodiscard]] vector<u32> ReadbackGpuSurvivorFlags() const;

    private:
        GpuCullSystem(Context& context, AssetManager& assets,
                      const SceneRendererSettings& settings);

        /// @brief Maximum per-frame candidates the cull buffers hold (must match SceneRenderer::MaxCullCandidates).
        static constexpr u32 MaxCullCandidates = 4096;

        Context& m_Context;

        /// @brief The active cull mode after the device-support fallback (CPU if GPU unsupported).
        SceneRendererSettings::CullMode m_ActiveCull = SceneRendererSettings::CullMode::CPU;
        /// @brief Set once the GPU-unsupported fallback has logged, so the WARN fires only once.
        bool m_GpuCullWarned = false;

        /// @brief The pyramid extent (set by ResizeHiZ); drives the reduction dispatch sizing.
        uvec2 m_Extent{1};

        /// @brief Compute pipeline that reduces one depth/hi-Z mip into the next (max-Z).
        Ref<ComputePipeline> m_HiZReducePipeline;
        /// @brief Layout for m_HiZReducePipeline: set 1 (sampled source + storage dest) + push block.
        Ref<PipelineLayout> m_HiZReduceLayout;
        /// @brief Set-1 layout for the reduction: binding 0 sampled source, binding 1 storage dest.
        Ref<DescriptorSetLayout> m_HiZReduceSetLayout;

        /// @brief Hi-Z depth pyramid: a max-Z mip chain reduced from the depth target.
        Ref<Image> m_HiZImage;
        /// @brief One storage view per hi-Z mip level (the reduction writes each).
        vector<Ref<ImageView>> m_HiZMips;
        /// @brief Whole-chain sampled view of the hi-Z pyramid, registered into bindless.
        Ref<ImageView> m_HiZSampleView;
        /// @brief Bindless slot for the whole-chain sampled hi-Z view.
        TextureHandle m_HiZSampleHandle;
        /// @brief One reduction descriptor set per destination mip, written on ResizeHiZ.
        vector<Ref<DescriptorSet>> m_HiZReduceSets;
        /// @brief The hi-Z pyramid mip chain import id.
        MipChainId m_HiZChainId;

        /// @brief Last frame's camera state, for the history-validity comparison.
        HiZHistoryState m_PreviousHiZState;
        /// @brief Whether the previous-frame pyramid is valid to occlusion-test against this frame.
        bool m_HiZHistoryValid = false;
        /// @brief Set whenever ResizeHiZ recreates the pyramid, forcing the next Execute invalid.
        bool m_HiZHistoryReset = true;

        /// @brief Cull compute pipeline (occlusion test → instanceCount), GPU mode only.
        Ref<ComputePipeline> m_CullPipeline;
        /// @brief Layout for m_CullPipeline: set 1 (hi-Z, candidates, commands, count) + push block.
        Ref<PipelineLayout> m_CullLayout;
        /// @brief Set-1 layout for the cull pass.
        Ref<DescriptorSetLayout> m_CullSetLayout;
        /// @brief Descriptor set for the cull pass, written on ResizeHiZ once the GPU cluster exists.
        Ref<DescriptorSet> m_CullSet;

        /// @brief Uploaded camera-frustum survivors (world bounds + draw args), GPU mode only.
        Ref<Buffer> m_CullCandidateBuffer;
        /// @brief Indirect command buffer the cull writes and the geometry pass reads.
        Ref<Buffer> m_IndirectBuffer;
        /// @brief GPU survivor-count buffer the cull atomically increments, read back for the stat.
        Ref<Buffer> m_CullCountBuffer;
        /// @brief Imported buffer id for the indirect command buffer in the internal graph.
        ResourceId m_IndirectId;

        /// @brief Previous-frame camera world→clip the cull pass screen-bounds candidates with.
        mat4 m_CullPrevViewProj{1.0f};
        /// @brief Candidate count the cull dispatch reads.
        u32 m_CullCandidateCount = 0;
        /// @brief Candidate/command region base (currentFrame * MaxCullCandidates).
        u32 m_CullFrameBase = 0;
        /// @brief This frame's slot in the survivor-count buffer.
        u32 m_CullCountIndex = 0;
        /// @brief 1 when the previous-frame pyramid is valid to occlude against, else 0 (frustum-only).
        u32 m_CullHistoryValid = 0;

        /// @brief Survivor count read back from the previous GPU Execute (the debug stat).
        mutable u32 m_LastGpuSurvivorCount = 0;
        /// @brief Number of candidate slots the GPU cull wrote this Execute (for the readback span).
        u32 m_GpuCandidateCount = 0;
        /// @brief The frame-in-flight region the previous GPU Execute wrote, for the count/flag readback.
        u32 m_GpuReadbackRegion = 0;
        /// @brief True once a GPU Execute has filled a region to read back.
        bool m_GpuReadbackValid = false;
    };
}
