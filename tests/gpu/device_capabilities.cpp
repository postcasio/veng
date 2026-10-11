// The device readout, and the two GPU features a compacting compute pipeline leans on.
//
// Every claim is a property of the operation as Vulkan defines it, not of the device the band runs
// on: the subgroup probe reads back which invocations shared a subgroup and in which lane, then
// checks each operation's answer against that layout, so the cases hold whatever lane order and
// subgroup width the device chose. A case whose operations the readout says the device lacks skips
// with a message.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <array>
#include <bit>
#include <cstring>
#include <map>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/DeviceCapabilities.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The test shader pack's ids.
    constexpr AssetId SubgroupProbeCompId{0x75DA066C704E3174ULL};
    constexpr AssetId DispatchArgsCompId{0x19CF265ADFBEE7B8ULL};
    constexpr AssetId DispatchVisitCompId{0xB48AAF3AE6866C42ULL};

    // subgroup_probe.comp's workgroup width.
    constexpr u32 ProbeInvocations = 64;

    // dispatch_visit.comp's workgroup width, and the group slots each dispatch's counters hold.
    constexpr u32 VisitGroupSize = 64;
    constexpr u32 MaxVisitGroups = 64;
    constexpr u32 VisitStride = 1 + MaxVisitGroups;

    struct DispatchArgsPush
    {
        u32 FirstGroups;
        u32 SecondGroups;
    };

    struct DispatchVisitPush
    {
        u32 Base;
    };

    Ref<DescriptorSetLayout> StorageSetLayout(Context& context, const char* name, const u32 count)
    {
        vector<DescriptorBinding> bindings;
        for (u32 binding = 0; binding < count; ++binding)
        {
            bindings.push_back({.Binding = binding,
                                .Type = DescriptorType::StorageBuffer,
                                .Count = 1,
                                .Stages = ShaderStage::Compute});
        }
        return DescriptorSetLayout::Create(context, {.Name = name, .Bindings = bindings});
    }

    Ref<ComputePipeline> MakePipeline(Context& context, const AssetHandle<Shader>& shader,
                                      const Ref<DescriptorSetLayout>& setLayout,
                                      const u32 pushBytes)
    {
        vector<PushConstantRange> push;
        if (pushBytes > 0)
        {
            push.push_back({.Stages = ShaderStage::Compute, .Offset = 0, .Size = pushBytes});
        }
        const auto layout = PipelineLayout::Create(context, {.Name = "Device Capabilities Layout",
                                                             .DescriptorSetLayouts = {setLayout},
                                                             .PushConstantRanges = push});
        return ComputePipeline::Create(
            context, {.Name = "Device Capabilities Pipeline",
                      .PipelineLayout = layout,
                      .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shader->Module}});
    }

    template <typename T>
    vector<T> DownloadAs(const Ref<Buffer>& buffer)
    {
        const vector<u8> bytes = buffer->Download();
        vector<T> out(bytes.size() / sizeof(T));
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(T));
        return out;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "The device readout reports a subgroup size and the stages Vulkan requires")
{
    const DeviceCapabilities& capabilities = Context.GetDeviceCapabilities();
    MESSAGE(DescribeDeviceCapabilities(capabilities));

    CHECK_FALSE(capabilities.DeviceName.empty());
    CHECK(std::has_single_bit(capabilities.SubgroupSize));
    CHECK(capabilities.SubgroupSize >= 4);
    CHECK(capabilities.SubgroupSize <= 128);

    // Vulkan 1.1 requires subgroups in compute with at least the basic operations.
    CHECK(capabilities.SupportsSubgroup(ShaderStage::Compute, SubgroupOperation::Basic));

    if (capabilities.SubgroupSizeControl)
    {
        CHECK(std::has_single_bit(capabilities.MinSubgroupSize));
        CHECK(std::has_single_bit(capabilities.MaxSubgroupSize));
        CHECK(capabilities.MinSubgroupSize <= capabilities.SubgroupSize);
        CHECK(capabilities.SubgroupSize <= capabilities.MaxSubgroupSize);
        CHECK(capabilities.MaxComputeWorkgroupSubgroups > 0);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "Subgroup bit counts, broadcasts and quad reads return what they define")
{
    const DeviceCapabilities& capabilities = Context.GetDeviceCapabilities();
    if (!capabilities.SupportsSubgroup(ShaderStage::Compute, SubgroupOperation::Basic |
                                                                 SubgroupOperation::Ballot |
                                                                 SubgroupOperation::Quad))
    {
        MESSAGE("The device lacks ballot or quad subgroup operations in compute; skipping");
        return;
    }

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());
    const auto shader = assets.LoadSync<Shader>(SubgroupProbeCompId);
    REQUIRE_MESSAGE(shader, shader.error().Detail);

    const auto makeOutput = [&](const char* name)
    {
        return Buffer::Create(Context, {.Name = name,
                                        .Size = ProbeInvocations * sizeof(uvec4),
                                        .Usage = BufferUsage::Storage | BufferUsage::TransferSrc});
    };
    const Ref<Buffer> lanesBuffer = makeOutput("Subgroup Probe Lanes");
    const Ref<Buffer> resultsBuffer = makeOutput("Subgroup Probe Results");

    const Ref<DescriptorSetLayout> setLayout = StorageSetLayout(Context, "Subgroup Probe Set", 2);
    const Ref<ComputePipeline> pipeline = MakePipeline(Context, *shader, setLayout, 0);
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "Subgroup Probe Set", .Layout = setLayout});
    set->Write(0, lanesBuffer);
    set->Write(1, resultsBuffer);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            cmd.BindDescriptorSets(
                {.Sets = {set}, .FirstSet = 3, .PipelineBindPoint = PipelineBindPoint::Compute});
            cmd.Dispatch(1, 1, 1);
        });

    const vector<uvec4> lanes = DownloadAs<uvec4>(lanesBuffer);
    const vector<uvec4> results = DownloadAs<uvec4>(resultsBuffer);
    REQUIRE(lanes.size() == ProbeInvocations);
    REQUIRE(results.size() == ProbeInvocations);

    // A subgroup is named by its first lane's local index, which WaveReadLaneFirst broadcast; with
    // every invocation active, that lane is the subgroup's lowest.
    std::map<u32, vector<u32>> subgroups;
    for (u32 invocation = 0; invocation < ProbeInvocations; ++invocation)
    {
        subgroups[lanes[invocation].w].push_back(invocation);
    }
    MESSAGE("subgroups: " << subgroups.size() << ", lane count " << lanes[0].z);

    u32 layoutViolations = 0;
    u32 countViolations = 0;
    u32 broadcastViolations = 0;
    u32 quadViolations = 0;
    for (const auto& [first, members] : subgroups)
    {
        const u32 laneCount = lanes[members.front()].z;
        // SPIR-V before 1.6 with no varying-size request runs at the device's subgroup size.
        layoutViolations += laneCount == capabilities.SubgroupSize ? 0 : 1;
        layoutViolations += members.size() <= laneCount ? 0 : 1;

        std::map<u32, u32> byLane;
        u32 predicateCount = 0;
        for (const u32 member : members)
        {
            layoutViolations += lanes[member].x == member ? 0 : 1;
            layoutViolations += lanes[member].z == laneCount ? 0 : 1;
            layoutViolations += lanes[member].y < laneCount ? 0 : 1;
            layoutViolations += byLane.emplace(lanes[member].y, member).second ? 0 : 1;
            predicateCount += results[member].w;
        }

        broadcastViolations += byLane.begin()->second == first ? 0 : 1;
        for (const u32 member : members)
        {
            const u32 lane = lanes[member].y;
            countViolations += results[member].x == predicateCount ? 0 : 1;

            u32 below = 0;
            for (const auto& [otherLane, other] : byLane)
            {
                below += otherLane < lane ? results[other].w : 0;
            }
            countViolations += results[member].y == below ? 0 : 1;

            const auto partner = byLane.find(lane ^ 1u);
            quadViolations +=
                partner != byLane.end() && results[member].z == partner->second ? 0 : 1;
        }
    }

    CHECK(layoutViolations == 0);
    CHECK(countViolations == 0);
    CHECK(broadcastViolations == 0);
    CHECK(quadViolations == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "An indirect dispatch runs the group counts a compute pass wrote for it")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());
    const auto argsShader = assets.LoadSync<Shader>(DispatchArgsCompId);
    REQUIRE_MESSAGE(argsShader, argsShader.error().Detail);
    const auto visitShader = assets.LoadSync<Shader>(DispatchVisitCompId);
    REQUIRE_MESSAGE(visitShader, visitShader.error().Detail);

    // Two records in one buffer, read at offsets 0 and 12, so the offset is exercised too.
    constexpr std::array<u32, 2> Groups{37, 5};
    constexpr u32 RecordBytes = 3 * sizeof(u32);

    // Seeded with counts the compute pass must overwrite before the dispatch reads them.
    const std::array<u32, 6> seed{9, 9, 9, 9, 9, 9};
    const Ref<Buffer> arguments =
        Buffer::Create(Context, {.Name = "Dispatch Arguments",
                                 .Size = sizeof(seed),
                                 .Usage = BufferUsage::Indirect | BufferUsage::Storage |
                                          BufferUsage::TransferDst | BufferUsage::TransferSrc});
    arguments->UploadSync(std::span(reinterpret_cast<const u8*>(seed.data()), sizeof(seed)));

    const vector<u32> zeros(static_cast<usize>(VisitStride) * Groups.size(), 0);
    const Ref<Buffer> visits =
        Buffer::Create(Context, {.Name = "Dispatch Visits",
                                 .Size = zeros.size() * sizeof(u32),
                                 .Usage = BufferUsage::Storage | BufferUsage::TransferDst |
                                          BufferUsage::TransferSrc});
    visits->UploadSync(
        std::span(reinterpret_cast<const u8*>(zeros.data()), zeros.size() * sizeof(u32)));

    const Ref<DescriptorSetLayout> setLayout = StorageSetLayout(Context, "Dispatch Set", 1);
    const Ref<ComputePipeline> argsPipeline =
        MakePipeline(Context, *argsShader, setLayout, sizeof(DispatchArgsPush));
    const Ref<ComputePipeline> visitPipeline =
        MakePipeline(Context, *visitShader, setLayout, sizeof(DispatchVisitPush));
    const Ref<DescriptorSet> argsSet =
        DescriptorSet::Create(Context, {.Name = "Dispatch Arguments Set", .Layout = setLayout});
    argsSet->Write(0, arguments);
    const Ref<DescriptorSet> visitSet =
        DescriptorSet::Create(Context, {.Name = "Dispatch Visits Set", .Layout = setLayout});
    visitSet->Write(0, visits);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId argumentsId = graph.ImportBuffer("Arguments");
            const ResourceId visitsId = graph.ImportBuffer("Visits");

            graph.AddComputePass("Write Arguments")
                .StorageBufferWrite(argumentsId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        ctx.Cmd().BindPipeline(argsPipeline);
                        ctx.Cmd().BindDescriptorSets(
                            {.Sets = {argsSet},
                             .FirstSet = 3,
                             .PipelineBindPoint = PipelineBindPoint::Compute});
                        ctx.Cmd().PushConstants(
                            DispatchArgsPush{.FirstGroups = Groups[0], .SecondGroups = Groups[1]});
                        ctx.Cmd().Dispatch(1, 1, 1);
                    });

            graph.AddComputePass("Indirect Dispatch")
                .IndirectRead(argumentsId)
                .StorageBufferWrite(visitsId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        ctx.Cmd().BindPipeline(visitPipeline);
                        ctx.Cmd().BindDescriptorSets(
                            {.Sets = {visitSet},
                             .FirstSet = 3,
                             .PipelineBindPoint = PipelineBindPoint::Compute});
                        for (u32 record = 0; record < Groups.size(); ++record)
                        {
                            ctx.Cmd().PushConstants(
                                DispatchVisitPush{.Base = record * VisitStride});
                            ctx.Cmd().DispatchIndirect(ctx.ResolvedBuffer(argumentsId),
                                                       record * RecordBytes);
                        }
                    });

            auto compiled = graph.Compile();
            compiled->Execute(
                cmd, std::array{RenderGraph::ImportBinding{.Id = argumentsId, .Buffer = arguments},
                                RenderGraph::ImportBinding{.Id = visitsId, .Buffer = visits}});
        });

    const vector<u32> counted = DownloadAs<u32>(visits);
    REQUIRE(counted.size() == zeros.size());
    for (u32 record = 0; record < Groups.size(); ++record)
    {
        CAPTURE(record);
        const u32 base = record * VisitStride;
        CHECK(counted[base] == Groups[record] * VisitGroupSize);

        u32 visitedOnce = 0;
        u32 visitedOutside = 0;
        for (u32 group = 0; group < MaxVisitGroups; ++group)
        {
            const u32 visitsOfGroup = counted[base + 1 + group];
            if (group < Groups[record])
            {
                visitedOnce += visitsOfGroup == 1 ? 1 : 0;
            }
            else
            {
                visitedOutside += visitsOfGroup;
            }
        }
        CHECK(visitedOnce == Groups[record]);
        CHECK(visitedOutside == 0);
    }
}
