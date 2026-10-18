#include "ParticleSimulation.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/PipelineLayout.h>

namespace Veng::Renderer
{
    namespace
    {
        // The chain's compute shaders in the engine core pack.
        constexpr AssetId ResetShaderId{0xBC3E1DD216A0CD28ULL};
        constexpr AssetId SpawnShaderId{0xD688E7DB5F9AA705ULL};
        constexpr AssetId PrepareShaderId{0x1D104E5182C281DEULL};
        constexpr AssetId UpdateShaderId{0x355C303B51430ABCULL};
        constexpr AssetId SortLocalShaderId{0x4215BED2C3F1ED41ULL};
        constexpr AssetId SortGlobalShaderId{0xF0329881A7EF4B75ULL};
        constexpr AssetId SortMergeShaderId{0xFCCA6A43EBEF6E7CULL};
        constexpr AssetId FinalizeShaderId{0x7C4E5BF79D2F956DULL};

        // The set-3 bindings, as particle_types.slang names them.
        enum Binding : u32
        {
            ParticlesBinding = 0,
            DeadListBinding = 1,
            LiveListBinding = 2,
            AdditiveBinding = 3,
            AlphaKeysBinding = 4,
            DrawsBinding = 5,
            CountersBinding = 6,
            ArgsBinding = 7,
            InstancesBinding = 8,
            EmittersBinding = 9,
            RangesBinding = 10,
            StatsBinding = 11,
            FrameScalesBinding = 12,
            BindingCount = 13,
        };

        // The sort's block (groupshared), and the threads one of its compare-exchange dispatches runs
        // per group.
        constexpr u32 SortBlock = 1024;
        constexpr u32 SortGlobalThreads = 256;
        constexpr u32 ThreadsPerGroup = 64;

        // The counters buffer: dead, live[2], alpha, additive, spawned, dropped, and a pad word.
        constexpr u64 CounterBytes = 8 * sizeof(u32);
        // The argument buffer: the update's dispatch, then the alpha and additive draws.
        constexpr u64 ArgsBytes = 48;
        // One frame's stats region, aligned for any device's storage-offset alignment.
        constexpr u64 StatsRegionBytes = 256;

        // Matches particle_compute.slang's ParticleComputePush.
        struct ParticleComputePush
        {
            u32 Cur = 0;
            u32 Capacity = 0;
            u32 SpawnTotal = 0;
            u32 RangeCount = 0;
            u32 SortStage = 0;
            u32 SortStep = 0;
            u32 SortSize = 0;
            u32 ViewConstantsIndex = 0;
        };

        Ref<ComputePipeline> BuildPipeline(Context& context, AssetManager& assets,
                                           const AssetId shader, const char* name,
                                           const Ref<PipelineLayout>& layout)
        {
            const AssetResult<AssetHandle<Veng::Shader>> module =
                assets.LoadSync<Veng::Shader>(shader);
            VE_ASSERT(module.has_value(), "ParticleSimulation: {} shader load failed: {}", name,
                      module.error().Detail);
            return ComputePipeline::Create(
                context,
                {
                    .Name = string("Particle ") + name + " Pipeline",
                    .PipelineLayout = layout,
                    .ShaderStage = {.Stage = ShaderStage::Compute, .Module = module->Get()->Module},
                });
        }

        Ref<Buffer> DeviceBuffer(Context& context, const char* name, const u64 size,
                                 const BufferUsage usage = BufferUsage::Storage)
        {
            return Buffer::Create(context, {.Name = name, .Size = size, .Usage = usage});
        }

        Ref<Buffer> RingBuffer(Context& context, const char* name, const u64 regionBytes,
                               const u32 frames)
        {
            return Buffer::Create(context, {
                                               .Name = name,
                                               .Size = regionBytes * frames,
                                               .Usage = BufferUsage::Storage,
                                               .HostMapped = true,
                                           });
        }

        u8* Region(const Ref<Buffer>& buffer, const u64 regionBytes, const u32 frame)
        {
            return static_cast<u8*>(buffer->GetMappedData()) + regionBytes * frame;
        }

        constexpr u64 InstanceRegionBytes = MaxParticleInstances * sizeof(GpuParticleInstance);
        constexpr u64 EmitterRegionBytes = MaxParticleEmittersPerFrame * sizeof(GpuParticleEmitter);
        constexpr u64 RangeRegionBytes = MaxParticleSpawnRanges * sizeof(GpuParticleSpawnRange);
        constexpr u64 FrameScaleRegionBytes = MaxParticleFrameScalesPerFrame * sizeof(f32);
    }

    Unique<ParticleSimulation> ParticleSimulation::Create(Context& context, AssetManager& assets,
                                                          const u32 capacity,
                                                          const ForwardLightingSets& forward)
    {
        return Unique<ParticleSimulation>(
            new ParticleSimulation(context, assets, capacity, forward));
    }

    ParticleSimulation::ParticleSimulation(Context& context, AssetManager& assets,
                                           const u32 capacity, const ForwardLightingSets& forward)
        : m_Context(context), m_Assets(assets), m_Capacity(capacity),
          m_FramesInFlight(context.GetMaxFramesInFlight())
    {
        VE_ASSERT(std::has_single_bit(capacity) && capacity <= DefaultParticleCapacity,
                  "ParticleSimulation: capacity {} must be a power of two of at most {}", capacity,
                  DefaultParticleCapacity);

        const u64 n = capacity;
        m_Particles = DeviceBuffer(context, "Particle Pool", n * sizeof(GpuParticle));
        m_DeadList = DeviceBuffer(context, "Particle Dead List", n * sizeof(u32));
        m_LiveList = DeviceBuffer(context, "Particle Live List", 2 * n * sizeof(u32));
        m_AdditiveList = DeviceBuffer(context, "Particle Additive List", n * sizeof(u32));
        m_AlphaKeys = DeviceBuffer(context, "Particle Alpha Keys",
                                   std::max<u64>(n, SortBlock) * sizeof(uvec2));
        m_Draws = DeviceBuffer(context, "Particle Draw Records", n * sizeof(GpuParticleDraw));
        m_Counters = DeviceBuffer(context, "Particle Counters", CounterBytes);
        m_Args = DeviceBuffer(context, "Particle Arguments", ArgsBytes,
                              BufferUsage::Storage | BufferUsage::Indirect);
        m_Instances =
            RingBuffer(context, "Particle Instances", InstanceRegionBytes, m_FramesInFlight);
        m_Emitters = RingBuffer(context, "Particle Emitters", EmitterRegionBytes, m_FramesInFlight);
        m_Ranges = RingBuffer(context, "Particle Spawn Ranges", RangeRegionBytes, m_FramesInFlight);
        m_StatsRing = RingBuffer(context, "Particle Stats", StatsRegionBytes, m_FramesInFlight);
        m_FrameScales =
            RingBuffer(context, "Particle Frame Scales", FrameScaleRegionBytes, m_FramesInFlight);
        std::memset(m_StatsRing->GetMappedData(), 0, StatsRegionBytes * m_FramesInFlight);
        m_SlotSequence.assign(m_FramesInFlight, 0);

        vector<DescriptorBinding> bindings;
        for (u32 binding = 0; binding < BindingCount; ++binding)
        {
            bindings.push_back(
                {.Binding = binding,
                 .Type = DescriptorType::StorageBuffer,
                 .Count = 1,
                 .Stages = ShaderStage::Compute | ShaderStage::Vertex | ShaderStage::Fragment});
        }
        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {.Name = "Particle Set Layout", .Bindings = std::move(bindings)});

        // One set per frame in flight, each bound once to its own ring regions: rewriting a shared
        // set still referenced by a pending command buffer is a validation error.
        for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
        {
            Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {.Name = "Particle Set", .Layout = m_SetLayout});
            set->Write(ParticlesBinding, m_Particles);
            set->Write(DeadListBinding, m_DeadList);
            set->Write(LiveListBinding, m_LiveList);
            set->Write(AdditiveBinding, m_AdditiveList);
            set->Write(AlphaKeysBinding, m_AlphaKeys);
            set->Write(DrawsBinding, m_Draws);
            set->Write(CountersBinding, m_Counters);
            set->Write(ArgsBinding, m_Args);
            set->Write(InstancesBinding, m_Instances, InstanceRegionBytes * frame,
                       InstanceRegionBytes);
            set->Write(EmittersBinding, m_Emitters, EmitterRegionBytes * frame, EmitterRegionBytes);
            set->Write(RangesBinding, m_Ranges, RangeRegionBytes * frame, RangeRegionBytes);
            set->Write(StatsBinding, m_StatsRing, StatsRegionBytes * frame, StatsRegionBytes);
            set->Write(FrameScalesBinding, m_FrameScales, FrameScaleRegionBytes * frame,
                       FrameScaleRegionBytes);
            m_Sets.push_back(std::move(set));
        }

        m_ComputeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "Particle Compute Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<ParticleComputePush>(
                               ShaderStage::Compute)},
                       });
        m_ResetPipeline =
            BuildPipeline(m_Context, m_Assets, ResetShaderId, "Reset", m_ComputeLayout);
        m_SpawnPipeline =
            BuildPipeline(m_Context, m_Assets, SpawnShaderId, "Spawn", m_ComputeLayout);
        m_PreparePipeline =
            BuildPipeline(m_Context, m_Assets, PrepareShaderId, "Prepare", m_ComputeLayout);
        m_SortLocalPipeline =
            BuildPipeline(m_Context, m_Assets, SortLocalShaderId, "Sort Local", m_ComputeLayout);
        m_SortGlobalPipeline =
            BuildPipeline(m_Context, m_Assets, SortGlobalShaderId, "Sort Global", m_ComputeLayout);
        m_SortMergePipeline =
            BuildPipeline(m_Context, m_Assets, SortMergeShaderId, "Sort Merge", m_ComputeLayout);
        m_FinalizePipeline =
            BuildPipeline(m_Context, m_Assets, FinalizeShaderId, "Finalize", m_ComputeLayout);
        BuildUpdatePipeline(forward);
    }

    ParticleSimulation::~ParticleSimulation() = default;

    void ParticleSimulation::BuildUpdatePipeline(const ForwardLightingSets& forward)
    {
        m_UpdateIblLayout = forward.IblLayout;
        m_UpdateShadowLayout = forward.ShadowLayout;
        m_UpdateLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "Particle Update Layout",
                .DescriptorSetLayouts = {m_SetLayout, forward.IblLayout, forward.ShadowLayout},
                .PushConstantRanges = {PushConstantRange::Of<ParticleComputePush>(
                    ShaderStage::Compute)},
            });
        m_UpdatePipeline =
            BuildPipeline(m_Context, m_Assets, UpdateShaderId, "Update", m_UpdateLayout);
    }

    void ParticleSimulation::Prepare(const ParticleFramePlan& plan)
    {
        const u32 frame = m_Context.GetCurrentFrameInFlight();

        // This slot's frame has retired (its fence was waited when the slot was acquired), so the
        // stats its finalize wrote are readable before this frame overwrites them; a slot whose
        // last finalize is older than the stats already taken is passed over.
        ++m_Sequence;
        if (m_SlotSequence[frame] > m_StatsSequence)
        {
            m_StatsSequence = m_SlotSequence[frame];
            u32 words[8] = {};
            std::memcpy(words, Region(m_StatsRing, StatsRegionBytes, frame), sizeof(words));
            m_Stats = ParticleGpuStats{
                .Live = words[0],
                .Spawned = words[1],
                .Dropped = words[2],
                .AlphaCount = words[3],
                .AlphaSorted = words[4],
            };
        }

        const auto instances =
            static_cast<u32>(std::min<usize>(plan.Instances.size(), MaxParticleInstances));
        std::memcpy(Region(m_Instances, InstanceRegionBytes, frame), plan.Instances.data(),
                    instances * sizeof(GpuParticleInstance));
        std::memcpy(Region(m_Emitters, EmitterRegionBytes, frame), plan.Emitters.data(),
                    plan.Emitters.size() * sizeof(GpuParticleEmitter));
        std::memcpy(Region(m_Ranges, RangeRegionBytes, frame), plan.Ranges.data(),
                    plan.Ranges.size() * sizeof(GpuParticleSpawnRange));
        std::memcpy(Region(m_FrameScales, FrameScaleRegionBytes, frame), plan.FrameScales.data(),
                    plan.FrameScales.size() * sizeof(f32));

        m_SpawnTotal = plan.SpawnTotal;
        m_RangeCount = static_cast<u32>(plan.Ranges.size());
        m_AliveBound = plan.AliveBound;
        m_UpdateRuns = m_GpuMayHoldLive || m_SpawnTotal > 0;
        m_SortSize = 0;
        if (m_UpdateRuns && plan.AlphaBound > 0)
        {
            m_SortSize = std::max(SortBlock, std::bit_ceil(plan.AlphaBound));
        }
        // An idle frame writes no stats; what the last finalize reported stands, and the update
        // before an idle frame left nothing alive.
        m_SlotSequence[frame] = m_UpdateRuns ? m_Sequence : 0;
    }

    void ParticleSimulation::Commit()
    {
        m_NeedsReset = false;
        if (m_UpdateRuns)
        {
            m_Cur ^= 1u;
            // That update killed every particle not counted alive, including those of instances
            // just left; with none counted, nothing can remain.
            m_GpuMayHoldLive = m_AliveBound > 0;
        }
    }

    void ParticleSimulation::BindCompute(CommandBuffer& cmd,
                                         const Ref<ComputePipeline>& pipeline) const
    {
        cmd.BindPipeline(pipeline);
        m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {m_Sets[m_Context.GetCurrentFrameInFlight()]},
            .FirstSet = BindlessRegistry::FirstUserSet,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
    }

    void ParticleSimulation::RecordSort(CommandBuffer& cmd) const
    {
        const u32 size = m_SortSize;
        const auto barrier = [&]
        {
            Backend::TransitionBuffer(
                cmd, *m_AlphaKeys, vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite, vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        };
        ParticleComputePush push{.Cur = m_Cur, .Capacity = m_Capacity, .SortSize = size};

        BindCompute(cmd, m_SortLocalPipeline);
        cmd.PushConstants(push);
        cmd.Dispatch(size / SortBlock, 1, 1);
        for (u32 stage = 2 * SortBlock; stage <= size; stage <<= 1)
        {
            for (u32 step = stage >> 1; step >= SortBlock; step >>= 1)
            {
                barrier();
                BindCompute(cmd, m_SortGlobalPipeline);
                push.SortStage = stage;
                push.SortStep = step;
                cmd.PushConstants(push);
                cmd.Dispatch(size / 2 / SortGlobalThreads, 1, 1);
            }
            barrier();
            BindCompute(cmd, m_SortMergePipeline);
            push.SortStage = stage;
            push.SortStep = 0;
            cmd.PushConstants(push);
            cmd.Dispatch(size / SortBlock, 1, 1);
        }
    }

    void ParticleSimulation::Declare(RenderGraph& graph, const PassIO& io,
                                     const ForwardLightingSets& forward)
    {
        m_Forward = forward;
        if (forward.IblLayout != m_UpdateIblLayout || forward.ShadowLayout != m_UpdateShadowLayout)
        {
            BuildUpdatePipeline(forward);
        }

        m_ParticlesId = graph.ImportBuffer("Particle Pool");
        m_DeadListId = graph.ImportBuffer("Particle Dead List");
        m_LiveListId = graph.ImportBuffer("Particle Live List");
        m_AdditiveId = graph.ImportBuffer("Particle Additive List");
        m_AlphaKeysId = graph.ImportBuffer("Particle Alpha Keys");
        m_DrawsId = graph.ImportBuffer("Particle Draw Records");
        m_CountersId = graph.ImportBuffer("Particle Counters");
        m_ArgsId = graph.ImportBuffer("Particle Arguments");

        const auto idle = [this] { return !m_UpdateRuns; };

        graph.AddComputePass("Particle Reset")
            .StorageBufferWrite(m_DeadListId)
            .StorageBufferWrite(m_CountersId)
            .SkipWhen([this] { return !m_NeedsReset; })
            .Execute(
                [this](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    BindCompute(cmd, m_ResetPipeline);
                    cmd.PushConstants(ParticleComputePush{.Capacity = m_Capacity});
                    cmd.Dispatch((m_Capacity + ThreadsPerGroup - 1) / ThreadsPerGroup, 1, 1);
                });

        graph.AddComputePass("Particle Spawn")
            .StorageBufferRead(m_DeadListId)
            .StorageBufferWrite(m_DeadListId)
            .StorageBufferWrite(m_ParticlesId)
            .StorageBufferWrite(m_LiveListId)
            .StorageBufferRead(m_CountersId)
            .StorageBufferWrite(m_CountersId)
            .SkipWhen([this] { return m_SpawnTotal == 0; })
            .Execute(
                [this](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    BindCompute(cmd, m_SpawnPipeline);
                    cmd.PushConstants(ParticleComputePush{
                        .Cur = m_Cur,
                        .Capacity = m_Capacity,
                        .SpawnTotal = m_SpawnTotal,
                        .RangeCount = m_RangeCount,
                    });
                    cmd.Dispatch((m_SpawnTotal + ThreadsPerGroup - 1) / ThreadsPerGroup, 1, 1);
                });

        graph.AddComputePass("Particle Prepare")
            .StorageBufferRead(m_CountersId)
            .StorageBufferWrite(m_CountersId)
            .StorageBufferWrite(m_ArgsId)
            .SkipWhen(idle)
            .Execute(
                [this](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    BindCompute(cmd, m_PreparePipeline);
                    cmd.PushConstants(ParticleComputePush{.Cur = m_Cur, .Capacity = m_Capacity});
                    cmd.Dispatch(1, 1, 1);
                });

        RenderGraph::PassBuilder update = graph.AddComputePass("Particle Update");
        update.IndirectRead(m_ArgsId)
            .StorageBufferRead(m_ParticlesId)
            .StorageBufferRead(m_LiveListId)
            .StorageBufferWrite(m_LiveListId)
            .StorageBufferWrite(m_DeadListId)
            .StorageBufferRead(m_CountersId)
            .StorageBufferWrite(m_CountersId)
            .StorageBufferWrite(m_DrawsId)
            .StorageBufferWrite(m_AlphaKeysId)
            .StorageBufferWrite(m_AdditiveId);
        // The six-way light gather reads the shadow atlases in this compute stage.
        if (io.ShadowMap.IsValid())
        {
            update.Sample(io.ShadowMap, AccessKind::SampleCompute);
        }
        if (io.PunctualShadowMap.IsValid())
        {
            update.Sample(io.PunctualShadowMap, AccessKind::SampleCompute);
        }
        update.SkipWhen(idle).Execute(
            [this](PassContext& pass)
            {
                CommandBuffer& cmd = pass.Cmd();
                const u32 frame = m_Context.GetCurrentFrameInFlight();
                cmd.BindPipeline(m_UpdatePipeline);
                m_Context.GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                // The shadow rings are framesInFlight-deep, so their dynamic offsets are the
                // frame-in-flight index, as the lighting and sprite passes bind them.
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {m_Sets[frame], m_Forward.IblSet, m_Forward.ShadowSet},
                    .FirstSet = BindlessRegistry::FirstUserSet,
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                    .DynamicOffsets = {frame * m_Forward.ShadowRingStride,
                                       frame * m_Forward.PunctualRingStride},
                });
                cmd.PushConstants(ParticleComputePush{
                    .Cur = m_Cur,
                    .Capacity = m_Capacity,
                    .ViewConstantsIndex =
                        m_Context.GetBindlessRegistry().GetCurrentViewConstantsIndex(),
                });
                cmd.DispatchIndirect(m_Args, 0);
            });

        graph.AddComputePass("Particle Sort")
            .StorageBufferRead(m_CountersId)
            .StorageBufferRead(m_AlphaKeysId)
            .StorageBufferWrite(m_AlphaKeysId)
            .SkipWhen([this] { return m_SortSize == 0; })
            .Execute([this](PassContext& pass) { RecordSort(pass.Cmd()); });

        graph.AddComputePass("Particle Finalize")
            .StorageBufferRead(m_CountersId)
            .StorageBufferWrite(m_ArgsId)
            .SkipWhen(idle)
            .Execute(
                [this](PassContext& pass)
                {
                    CommandBuffer& cmd = pass.Cmd();
                    BindCompute(cmd, m_FinalizePipeline);
                    cmd.PushConstants(ParticleComputePush{
                        .Cur = m_Cur, .Capacity = m_Capacity, .SortSize = m_SortSize});
                    cmd.Dispatch(1, 1, 1);
                });
    }

    void
    ParticleSimulation::AppendImportBindings(vector<RenderGraph::ImportBinding>& bindings) const
    {
        bindings.push_back({.Id = m_ParticlesId, .Buffer = m_Particles});
        bindings.push_back({.Id = m_DeadListId, .Buffer = m_DeadList});
        bindings.push_back({.Id = m_LiveListId, .Buffer = m_LiveList});
        bindings.push_back({.Id = m_AdditiveId, .Buffer = m_AdditiveList});
        bindings.push_back({.Id = m_AlphaKeysId, .Buffer = m_AlphaKeys});
        bindings.push_back({.Id = m_DrawsId, .Buffer = m_Draws});
        bindings.push_back({.Id = m_CountersId, .Buffer = m_Counters});
        bindings.push_back({.Id = m_ArgsId, .Buffer = m_Args});
    }
}
