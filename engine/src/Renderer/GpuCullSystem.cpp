#include "GpuCullSystem.h"

#include "DrawPlan.h"
#include "GpuBlocks.h"

#include <algorithm>
#include <bit>
#include <span>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>

namespace Veng::Renderer
{
    namespace
    {
        // The hi-Z max-Z reduction compute shader.
        constexpr AssetId HiZReduceCompId{0xCB20C4EF8A20ADBCULL};

        // The GPU occlusion-cull → indirect-draw compute shader.
        constexpr AssetId OcclusionCullCompId{0x5FE19B500FD44B52ULL};

        // Single-channel float for the hi-Z pyramid; the reduction stores max depth.
        constexpr Format HiZFormat = Format::R32Sfloat;

        // The hi-Z reduction push block: the destination and source mip extents, so a
        // boundary invocation skips out-of-range texels and an odd parent dimension
        // folds its dropped row/column into the max (matches hi_z_reduce.comp).
        struct HiZReducePush
        {
            uvec2 DestExtent;
            uvec2 SourceExtent;
        };

        // The cull compute push block, matching occlusion_cull.comp PushConstants.
        struct OcclusionCullPush
        {
            mat4 PrevViewProj;
            uvec2 HiZBaseExtent;
            u32 CandidateCount;
            u32 HistoryValid;
            f32 DepthBias;
            u32 FrameBase;
            u32 CountIndex;
        };
    }

    Unique<GpuCullSystem> GpuCullSystem::Create(Context& context, AssetManager& assets,
                                                const SceneRendererSettings& settings)
    {
        return Unique<GpuCullSystem>(new GpuCullSystem(context, assets, settings));
    }

    GpuCullSystem::GpuCullSystem(Context& context, AssetManager& assets,
                                 const SceneRendererSettings& settings)
        : m_Context(context)
    {
        ResolveActiveCullMode(settings);

        auto LoadShader = [&assets](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "GpuCullSystem: {} shader load failed: {}", what,
                      result.has_value() ? "" : result.error().Detail);
            return *result;
        };

        // The hi-Z reduction compute pipeline. Set 1 (binding 0 sampled source, binding 1
        // storage dest) is off bindless — a closed producer→consumer reduction needs no
        // global registration, and a dedicated set sidesteps the set-0 storage-image
        // argument-buffer path on MoltenVK.
        const AssetHandle<Veng::Shader> hiZReduceCs =
            LoadShader(HiZReduceCompId, "hi-Z reduce compute");
        m_HiZReduceSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer HiZ Reduce Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_HiZReduceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer HiZ Reduce Layout",
                .DescriptorSetLayouts = {m_HiZReduceSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<HiZReducePush>(ShaderStage::Compute)},
            });
        m_HiZReducePipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer HiZ Reduce Pipeline",
                .PipelineLayout = m_HiZReduceLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = hiZReduceCs.Get()->Module},
            });

        // The GPU cull path's buffers + pipeline build only where the device supports it.
        if (!m_Context.IsGpuDrivenCullingSupported())
        {
            return;
        }

        const u32 framesInFlight = m_Context.GetMaxFramesInFlight();

        const u64 candidateRegion = static_cast<u64>(MaxCullCandidates) * sizeof(GpuCullCandidate);
        m_CullCandidateBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Cull Candidates",
                                          .Size = candidateRegion * framesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });

        const u64 indirectRegion =
            static_cast<u64>(MaxCullCandidates) * sizeof(DrawIndexedIndirectCommand);
        m_IndirectBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Indirect Commands",
                                          .Size = indirectRegion * framesInFlight,
                                          .Usage = BufferUsage::Storage | BufferUsage::Indirect,
                                      });

        m_CullCountBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Cull Count",
                                          .Size = static_cast<u64>(framesInFlight) * sizeof(u32),
                                          .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                          .HostMapped = true,
                                      });

        const AssetHandle<Veng::Shader> cullCs =
            LoadShader(OcclusionCullCompId, "occlusion-cull compute");

        m_CullSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Cull Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_CullLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Cull Layout",
                           .DescriptorSetLayouts = {m_CullSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<OcclusionCullPush>(
                               ShaderStage::Compute)},
                       });
        m_CullPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Cull Pipeline",
                .PipelineLayout = m_CullLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = cullCs.Get()->Module},
            });
    }

    GpuCullSystem::~GpuCullSystem()
    {
        m_Context.GetBindlessRegistry().Release(m_HiZSampleHandle);
    }

    void GpuCullSystem::ResizeHiZ(const uvec2 extent, const Ref<ImageView>& depthView)
    {
        m_Extent = extent;

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_HiZSampleHandle);

        // A full mip chain over the depth extent: floor(log2(max(w,h))) + 1 levels.
        const u32 maxDim = std::max(m_Extent.x, m_Extent.y);
        const u32 mipCount = maxDim == 0 ? 1 : (std::bit_width(maxDim));

        m_HiZImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer HiZ",
                                         .Extent = {m_Extent.x, m_Extent.y, 1},
                                         .MipLevels = mipCount,
                                         .Format = HiZFormat,
                                         .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                     });

        // One single-mip storage view per level (the reduction writes each), plus a
        // whole-chain sampled view for the occlusion test.
        m_HiZMips.clear();
        m_HiZMips.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            m_HiZMips.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer HiZ Mip {} View", level),
                               .Image = m_HiZImage,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
        }
        m_HiZSampleView = ImageView::Create(m_Context, {
                                                           .Name = "SceneRenderer HiZ Sample View",
                                                           .Image = m_HiZImage,
                                                           .MipLevels = mipCount,
                                                       });
        m_HiZSampleHandle = bindless.Register(m_HiZSampleView);

        // Per-mip reduction descriptor sets: set k binds mip k's source (the depth
        // target for k=0, hi-Z mip k-1 otherwise) and mip k's destination storage view.
        m_HiZReduceSets.clear();
        m_HiZReduceSets.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer HiZ Reduce Set {}", level),
                               .Layout = m_HiZReduceSetLayout,
                           });
            const Ref<ImageView>& source = level == 0 ? depthView : m_HiZMips[level - 1];
            set->Write(0, source);
            set->Write(1, m_HiZMips[level]);
            m_HiZReduceSets.push_back(std::move(set));
        }

        // The freshly created pyramid carries no last-frame depth; the next Execute must
        // skip occlusion rather than test against an undefined/stale chain.
        m_HiZHistoryReset = true;

        // The cull set samples the pyramid through binding 0, and the pyramid is recreated on
        // Resize/Configure — but the live set may still be referenced by an in-flight frame's
        // command buffer and its bindings are not update-after-bind, so bind the new view into
        // a fresh set rather than writing the old one in place (the replaced set retires through
        // the deferred-destruction path, and the cull pass reads the member at record time). Only
        // when the GPU cull cluster exists (a supported device).
        if (m_CullCandidateBuffer)
        {
            m_CullSet = DescriptorSet::Create(m_Context, {
                                                             .Name = "SceneRenderer Cull Set",
                                                             .Layout = m_CullSetLayout,
                                                         });
            m_CullSet->Write(0, m_HiZSampleView);
            m_CullSet->Write(1, m_CullCandidateBuffer);
            m_CullSet->Write(2, m_IndirectBuffer);
            m_CullSet->Write(3, m_CullCountBuffer);
        }
    }

    void GpuCullSystem::ResolveActiveCullMode(const SceneRendererSettings& settings)
    {
        const bool gpuRequested = settings.Cull == SceneRendererSettings::CullMode::GPU;
        const bool gpuSupported = m_Context.IsGpuDrivenCullingSupported();

        // The GPU path is an optimization, not a correctness requirement: a device lacking
        // multiDrawIndirect / drawIndirectFirstInstance silently runs the CPU path, which
        // renders the same image. The fallback is logged once so it is observable, and
        // GetActiveCull() reports the real mode for the debug UI and tests.
        if (gpuRequested && !gpuSupported && !m_GpuCullWarned)
        {
            Log::Warn("SceneRenderer: CullMode::GPU requested but the device lacks "
                      "multiDrawIndirect / drawIndirectFirstInstance; using CullMode::CPU.");
            m_GpuCullWarned = true;
        }

        m_ActiveCull = (gpuRequested && gpuSupported) ? SceneRendererSettings::CullMode::GPU
                                                      : SceneRendererSettings::CullMode::CPU;
    }

    ResourceId GpuCullSystem::ImportIndirect(RenderGraph& graph)
    {
        // The GPU cull arm imports the indirect command buffer so the cull compute pass
        // (StorageBufferWrite) and the geometry pass (IndirectRead) share it through the
        // graph-derived buffer barrier.
        m_IndirectId = ResourceId{};
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            m_IndirectId = graph.ImportBuffer("SceneRenderer Indirect Commands");
        }
        return m_IndirectId;
    }

    void GpuCullSystem::ImportHiZChain(RenderGraph& graph)
    {
        // Import the hi-Z chain once: the GPU cull samples last frame's pyramid (declared
        // .Sample by DeclareCull for the graph-derived transition into ShaderReadOnly before the
        // cull) and the reduction at the tail writes this frame's pyramid into the same slots.
        m_HiZChainId =
            graph.ImportImageMips("SceneRenderer HiZ", static_cast<u32>(m_HiZMips.size()));
    }

    void GpuCullSystem::DeclareCull(RenderGraph& graph, const GBufferDrawPlan* plan)
    {
        // StorageBufferWrite on the indirect import drives the graph-derived
        // StorageBufferWrite → IndirectRead barrier feeding the geometry pass.
        RenderGraph::PassBuilder builder = graph.AddComputePass("Scene GPU Cull");
        builder.StorageBufferWrite(m_IndirectId);

        // The cull samples last frame's hi-Z pyramid (through m_CullSet, off the graph's
        // descriptor binding). Declaring .Sample on each mip drives the graph-derived
        // transition into ShaderReadOnly before the cull — the pyramid is a cross-frame
        // renderer-owned resource the graph would otherwise leave in its prior layout.
        for (u32 level = 0; level < m_HiZMips.size(); ++level)
        {
            builder.Sample(m_HiZChainId.Level(level));
        }

        builder.Execute(
            [this, plan](PassContext& inner)
            {
                if (plan->Cull != SceneRendererSettings::CullMode::GPU || plan->Slots.empty())
                {
                    return;
                }

                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(m_CullPipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {m_CullSet},
                    .FirstSet = 1, // set 0 is reserved for the bindless registry
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(OcclusionCullPush{
                    .PrevViewProj = m_CullPrevViewProj,
                    .HiZBaseExtent = m_Extent,
                    .CandidateCount = m_CullCandidateCount,
                    .HistoryValid = m_CullHistoryValid,
                    // Small bias absorbing reduction quantization; a tie draws.
                    .DepthBias = 0.001f,
                    .FrameBase = m_CullFrameBase,
                    .CountIndex = m_CullCountIndex,
                });
                cmd.Dispatch((m_CullCandidateCount + 63) / 64, 1, 1);
            });
    }

    void GpuCullSystem::DeclareHiZReduction(RenderGraph& graph, const ResourceId depthId)
    {
        const u32 mipCount = static_cast<u32>(m_HiZMips.size());

        // One compute dispatch per mip. Dispatch k reads mip k's source and writes mip
        // k; the per-mip graph surface derives the read-after-write barrier between
        // dispatch k's write of mip k and dispatch k+1's read of it. Mip 0's source is
        // the depth target (declared .Sample, reusing the depth import so the barrier
        // chains off the lighting pass's read); a source mip n-1 is declared .Sample.
        for (u32 level = 0; level < mipCount; level++)
        {
            // Mip extents (image extent >> level, floored at 1).
            const u32 dstW = std::max(m_Extent.x >> level, 1u);
            const u32 dstH = std::max(m_Extent.y >> level, 1u);
            const u32 srcW = level == 0 ? m_Extent.x : std::max(m_Extent.x >> (level - 1), 1u);
            const u32 srcH = level == 0 ? m_Extent.y : std::max(m_Extent.y >> (level - 1), 1u);

            // The source (depth target or prior mip) binds as a sampled image, so it
            // must be in ShaderReadOnly — declared .Sample, not .StorageRead. The
            // destination is a storage write (General). A prior mip therefore goes
            // General (its write) → ShaderReadOnly (its read as the next source), a
            // graph-derived per-mip transition.
            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("HiZ Reduce Mip {}", level));
            if (level == 0)
            {
                builder.Sample(depthId);
            }
            else
            {
                builder.Sample(m_HiZChainId.Level(level - 1));
            }
            builder.StorageWrite(m_HiZChainId.Level(level));

            const Ref<ComputePipeline> pipeline = m_HiZReducePipeline;
            const Ref<DescriptorSet> set = m_HiZReduceSets[level];
            const HiZReducePush push{
                .DestExtent = {dstW, dstH},
                .SourceExtent = {srcW, srcH},
            };
            builder.Execute(
                [pipeline, set, push](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1, // set 0 is reserved for the bindless registry
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch((push.DestExtent.x + 7) / 8, (push.DestExtent.y + 7) / 8, 1);
                });
        }
    }

    GpuCullCandidate* GpuCullSystem::BeginFrameUpload(const u32 frameIndex)
    {
        if (!m_CullCandidateBuffer)
        {
            return nullptr;
        }
        return static_cast<GpuCullCandidate*>(m_CullCandidateBuffer->GetMappedData()) +
               static_cast<usize>(frameIndex) * MaxCullCandidates;
    }

    void GpuCullSystem::RecordFrame(const u32 candidateCount, const u32 frameBase,
                                    const u32 frameIndex, const mat4& prevViewProj,
                                    const bool occlusionEnabled)
    {
        // The GPU cull dispatch reads the candidate region this frame; zero its survivor
        // count so the next-frame readback reflects only this dispatch. The push members the
        // cull pass reads are set here (per-frame), not in the recompile-time declaration.
        m_CullCandidateCount = candidateCount;
        m_CullFrameBase = frameBase;
        m_CullCountIndex = frameIndex;
        m_CullPrevViewProj = prevViewProj;
        m_CullHistoryValid = (occlusionEnabled && m_HiZHistoryValid) ? 1u : 0u;

        auto* counts = static_cast<u32*>(m_CullCountBuffer->GetMappedData());
        counts[frameIndex] = 0;

        // Record the region the readback reads one frame late.
        m_GpuCandidateCount = candidateCount;
        m_GpuReadbackRegion = frameIndex;
        m_GpuReadbackValid = true;
    }

    void GpuCullSystem::ReadSurvivorCount(const u32 frameIndex)
    {
        // Read the GPU survivor count the previous Execute wrote into this frame's region
        // before RecordFrame zeroes it again — the host-visible count is one frame late, so
        // it never gates this frame's draw.
        if (!m_CullCountBuffer)
        {
            return;
        }
        const auto* counts = static_cast<const u32*>(m_CullCountBuffer->GetMappedData());
        m_LastGpuSurvivorCount = counts[frameIndex];
    }

    void GpuCullSystem::EvaluateHistory(const HiZHistoryState& current, const f32 sceneDiagonal)
    {
        // The reset flag (frame 0 / post-resize / post-configure) forces invalid regardless of
        // the view delta; otherwise the device-free metric compares this frame's camera against
        // last frame's. The result feeds the GPU cull (occlusion skipped when invalid).
        m_HiZHistoryValid =
            !m_HiZHistoryReset && Renderer::IsHiZHistoryValid(m_PreviousHiZState, current,
                                                              sceneDiagonal, HiZHistorySettings{});
    }

    void GpuCullSystem::CommitHistory(const HiZHistoryState& current)
    {
        m_PreviousHiZState = current;
        // The pyramid now holds this frame's depth, so the next Execute may test against it
        // (subject to the view-delta metric).
        m_HiZHistoryReset = false;
    }

    vector<u32> GpuCullSystem::ReadbackGpuSurvivorFlags() const
    {
        if (!m_GpuReadbackValid || m_GpuCandidateCount == 0 || !m_IndirectBuffer)
        {
            return {};
        }

        // Download the full indirect buffer and pull each candidate command's instanceCount
        // from this frame's region; 1 = drawn, 0 = occluded.
        const vector<u8> bytes = m_IndirectBuffer->Download();
        const auto* commands = reinterpret_cast<const DrawIndexedIndirectCommand*>(bytes.data());
        const u32 base = m_GpuReadbackRegion * MaxCullCandidates;

        vector<u32> flags(m_GpuCandidateCount);
        for (u32 i = 0; i < m_GpuCandidateCount; ++i)
        {
            flags[i] = commands[base + i].InstanceCount;
        }
        return flags;
    }
}
