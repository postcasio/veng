// A normalised sixteen-bit storage image, end to end. RGBA16Unorm is one of the extended
// storage-image formats, so two things have to hold before a consumer can rely on it: the device
// must carry the format's StorageImage feature, and a shader declaring the Rgba16 image format
// must actually run. Both are asserted here against the device rather than against a comment:
//
//   1. The context's own report of shaderStorageImageExtendedFormats agrees with the per-format
//      query — the guarantee the feature makes is that the format carries StorageImage. The
//      format's byte cost is device-free and is pinned in the unit band instead.
//   2. A compute dispatch writes the range floor, its ceiling, the datum, and a ramp into an
//      RGBA16Unorm storage image; the readback recovers all four to within one part in 65535,
//      which is the whole point of choosing unorm over half for a bounded field.

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The test pack's normalised-range compute shader.
    constexpr AssetId StorageImageRangeCompId{0x1F4E};

    constexpr u32 Width = 8;
    constexpr u32 Height = 4;
    constexpr u32 ChannelCount = 4;

    // The top code of a 16-bit unorm scale: 1.0 stores as UnormMax, and one step of that scale is
    // the tolerance every readback assertion below is stated in.
    constexpr u32 UnormMax = 65535;
    // The datum halfway up the range. 0.5 * 65535 falls between two codes, so either neighbour is
    // within the one-step tolerance.
    constexpr u32 DatumCode = (UnormMax + 1) / 2;

    struct RangePush
    {
        uvec2 Extent;
    };
}

TEST_CASE_FIXTURE(Test::GpuFixture, "RGBA16Unorm storage image: the extended format is reported")
{
    // RGBA16Sfloat carries the StorageImage feature on every conformant device, so this pins that
    // the query answers at all rather than reporting a blanket false.
    CHECK(Context.IsFormatStorageImageSupported(Format::RGBA16Sfloat));

    // The feature is a device-wide guarantee that every extended storage-image format carries the
    // StorageImage format feature. Where the context reports it enabled, the per-format query must
    // agree — a disagreement means the guarantee was read from the wrong place.
    if (Context.IsExtendedStorageImageFormatsSupported())
    {
        CHECK(Context.IsFormatStorageImageSupported(Format::RGBA16Unorm));
        CHECK(Context.IsFormatStorageImageSupported(Format::RG8Unorm));
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "RGBA16Unorm storage image: a compute write holds the range (skips unsupported)")
{
    if (!Context.IsFormatStorageImageSupported(Format::RGBA16Unorm))
    {
        MESSAGE("RGBA16Unorm storage images unsupported on this device; skipping the range write");
        return;
    }

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mountResult, "mount test shader pack: ", mountResult.error());

    const AssetResult<AssetHandle<Shader>> rangeCs =
        assets.LoadSync<Shader>(StorageImageRangeCompId);
    REQUIRE_MESSAGE(rangeCs.has_value(), "load storage_image_range.comp from the test pack");

    auto target = Image::Create(Context, {
                                             .Name = "Unorm Range Target",
                                             .Extent = {Width, Height, 1},
                                             .Format = Format::RGBA16Unorm,
                                             .Usage = ImageUsage::Storage | ImageUsage::TransferSrc,
                                         });
    auto storageView =
        ImageView::Create(Context, {.Name = "Unorm Range Storage View", .Image = target});

    auto setLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "Unorm Range Set Layout",
                                                 .Bindings =
                                                     {
                                                         {.Binding = 0,
                                                          .Type = DescriptorType::StorageImage,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                     },
                                             });
    auto layout = PipelineLayout::Create(
        Context, {
                     .Name = "Unorm Range Layout",
                     .DescriptorSetLayouts = {setLayout},
                     .PushConstantRanges = {PushConstantRange::Of<RangePush>(ShaderStage::Compute)},
                 });
    auto pipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "Unorm Range Pipeline",
            .PipelineLayout = layout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = rangeCs->Get()->Module},
        });

    auto set = DescriptorSet::Create(Context, {.Name = "Unorm Range Set", .Layout = setLayout});
    set->Write(0, storageView);

    const RangePush push{.Extent = {Width, Height}};

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId targetId = graph.Import("Unorm Range Target");
            RenderGraph::PassBuilder builder = graph.AddComputePass("Unorm Range Write");
            builder.StorageWrite(targetId);
            const Ref<ComputePipeline> pl = pipeline;
            const Ref<DescriptorSet> descriptors = set;
            builder.Execute(
                [pl, descriptors, push](PassContext& ctx)
                {
                    CommandBuffer& c = ctx.Cmd();
                    c.BindPipeline(pl);
                    c.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {descriptors},
                        .FirstSet = BindlessRegistry::FirstUserSet,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    c.PushConstants(push);
                    c.Dispatch(1, 1, 1);
                });

            const RenderGraph::ImportBinding binding{.Id = targetId, .View = storageView};
            graph.Compile()->Execute(cmd, {&binding, 1});
        });

    const std::vector<u8> bytes = target->Download();
    REQUIRE(bytes.size() == static_cast<size_t>(Width) * Height * ChannelCount * sizeof(u16));

    std::array<u16, ChannelCount> texel{};
    u32 worstRampError = 0;
    u32 worstDatumError = 0;
    bool floorHeld = true;
    bool ceilingHeld = true;

    // Accumulate the extrema and assert once: every texel makes the same four claims, so a check
    // per texel would be 128 assertions of four properties.
    for (u32 y = 0; y < Height; ++y)
    {
        for (u32 x = 0; x < Width; ++x)
        {
            const size_t base = ((static_cast<size_t>(y) * Width) + x) * ChannelCount;
            std::memcpy(texel.data(), bytes.data() + (base * sizeof(u16)), sizeof(texel));

            floorHeld = floorHeld && (texel[0] == 0);
            ceilingHeld = ceilingHeld && (texel[1] == UnormMax);

            const u32 datumError =
                texel[2] > DatumCode ? texel[2] - DatumCode : DatumCode - texel[2];
            worstDatumError = std::max(worstDatumError, datumError);

            // The ramp is x/(Width-1), quantised at 1/65535. Its error against the exact value is
            // what "uniform quantisation" means, and it must not depend on where in the range the
            // sample sits.
            const auto expected = static_cast<u32>(
                (static_cast<f64>(x) / static_cast<f64>(Width - 1) * UnormMax) + 0.5);
            const u32 error = texel[3] > expected ? texel[3] - expected : expected - texel[3];
            worstRampError = std::max(worstRampError, error);
        }
    }

    CHECK(floorHeld);   // 0.0 stores as code 0
    CHECK(ceilingHeld); // 1.0 stores as code 65535, so the top of the range is representable
    CHECK(worstDatumError <= 1u);
    CHECK(worstRampError <= 1u);
}
