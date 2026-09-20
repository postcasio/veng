// Parameter-block byte-layout proof (GPU). The cooker packs a material's MaterialParams on a
// tight 4-byte cursor because that is the layout Slang's ByteAddressBuffer.Load<T> reads; the
// uniform/std140 offset disagrees with it the moment a vector follows a scalar. The cooker-side
// cases assert the cooker's own computed offsets, which is the cooker agreeing with itself —
// this is the end-to-end claim, on a block whose vectors deliberately land at offsets 4 and 36.
//
// Two distinct claims:
//   1. Transport — every member the host writes through SetParam is the value the shader reads.
//      This is what fails if Slang's Load<T> lowering stops being tight.
//   2. Offsets — the cooked field table reads {0, 4, 16, 32, 36}, which separates "the compiler
//      moved" from "the cooker moved" when the transport claim fails.
//
// The fixture is a PostProcess material, so the value under test is the rendered output: one
// fullscreen draw into an RGBA32F target whose fragment writes one member component per vertical
// band, read back exactly with no encode in the way.

#include <array>
#include <bit>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Path.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include "support/TempPath.h"

#include <gpu/fixture.h>

#ifdef GPU_MATERIAL_LAYOUT_FIXTURE_DIR

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The fixture material's cooked default instance, as its .vmat names it.
    constexpr AssetId BlockLayoutInstanceId{0xD2C398A7C469E011ULL};

    // The written values: distinct and far apart, so a member reading a neighbour's bytes is
    // unambiguous rather than a near miss, and each exactly representable in an f32.
    constexpr f32 Lead = 1.5f;
    constexpr vec3 Mid{20.25f, 30.5f, 40.75f};
    constexpr vec4 Region{50.125f, 60.25f, 70.375f, 80.5f};
    constexpr vec2 Tail{90.625f, 100.75f};
    // Between 2^23 and 2^24, so its bit pattern is a normal f32 (the scalar setter carries it)
    // and the shader's float(Tag) is exact.
    constexpr u32 Tag = 12345678;

    // One band per member component, left to right across the output.
    constexpr u32 BandCount = 11;
    constexpr u32 BandWidth = 8;
    constexpr uvec2 Extent{BandCount * BandWidth, 4};

    const MaterialField& FieldNamed(std::span<const MaterialField> fields, std::string_view name)
    {
        for (const MaterialField& field : fields)
        {
            if (field.Name == name)
            {
                return field;
            }
        }
        FAIL("no cooked field named ", name);
        return fields.front();
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "material block: a vector at a non-16-aligned offset reaches the shader where "
                  "the cooker packed it")
{
    const path fixtureDir = path(GPU_MATERIAL_LAYOUT_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_block_layout.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cooked =
        cooker.CookPack(fixtureDir / "block_layout_pack.json", outArchive, {}, nullptr, nullptr,
                        nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> instance =
        assets.LoadSync<MaterialInstance>(BlockLayoutInstanceId);
    const string loadErr = instance.has_value() ? string{} : instance.error().Detail;
    REQUIRE_MESSAGE(instance.has_value(), loadErr);
    MaterialInstance& material = *instance->Get();

    // Claim 2: the cooked offsets are the tight cursor's, not a 16-aligning packer's.
    const std::span<const MaterialField> fields = material.GetParent().Get()->GetFields();
    REQUIRE(fields.size() == 5);
    CHECK(FieldNamed(fields, "Lead").Offset == 0);
    CHECK(FieldNamed(fields, "Mid").Offset == 4);
    CHECK(FieldNamed(fields, "Region").Offset == 16);
    CHECK(FieldNamed(fields, "Tag").Offset == 32);
    CHECK(FieldNamed(fields, "Tail").Offset == 36);

    // Every member written from the host; the cooked defaults are all zero, so a value that
    // arrives can only have come through the write. Two of the five go through a handle resolved
    // from the same name — the vector at the non-16-aligned offset and the scalar — so both
    // spellings of the setter are held to the one layout.
    material.SetParam("Lead", Lead);
    material.SetParam(material.Field("Mid"), vec4(Mid, 0.0f));
    material.SetParam("Region", Region);
    // The scalar setter copies the field's four bytes verbatim, so a uint member is written by
    // handing it the integer's bit pattern.
    material.SetParam(material.Field("Tag"), std::bit_cast<f32>(Tag));
    material.SetParam("Tail", vec4(Tail, 0.0f, 0.0f));

    const Ref<Image> outputImage =
        Image::Create(Context, {
                                   .Name = "Block Layout Output",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA32Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> outputView =
        ImageView::Create(Context, {.Name = "Block Layout Output View", .Image = outputImage});

    // The material's own layout and stages, against this target's format — the pipeline a
    // PostProcess pass would build, without a pass's upstream-source contract.
    const Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
        Context,
        {
            .Name = "Block Layout Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA32Sfloat}},
            .PipelineLayout = material.GetPipelineLayout(),
            .ShaderStages =
                {
                    {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                    {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                },
        });

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId outputId = graph.Import("Block Layout Output");

            graph.AddPass("Block Layout")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& passCmd = ctx.Cmd();
                        passCmd.BindPipeline(pipeline);
                        passCmd.SetViewport({0, 0}, Extent);
                        passCmd.SetScissor({0, 0}, Extent);
                        Context.GetBindlessRegistry().Bind(passCmd);
                        material.Bind(passCmd);
                        passCmd.DrawFullscreenTriangle();
                    });

            const RenderGraph::ImportBinding bindings[] = {{.Id = outputId, .View = outputView}};
            graph.Compile()->Execute(cmd, bindings);
        });

    const vector<u8> pixels = outputImage->Download();
    REQUIRE(pixels.size() == static_cast<usize>(Extent.x) * Extent.y * 4 * sizeof(f32));
    const auto* texels = reinterpret_cast<const f32*>(pixels.data());

    // Claim 1: each member component the shader read, band by band.
    const std::array<f32, BandCount> written{
        Lead,   Mid.x, Mid.y, Mid.z, Region.x, Region.y, Region.z, Region.w, static_cast<f32>(Tag),
        Tail.x, Tail.y};
    for (u32 band = 0; band < BandCount; ++band)
    {
        const usize x = static_cast<usize>(band) * BandWidth + BandWidth / 2;
        const usize texel = (static_cast<usize>(Extent.y / 2) * Extent.x + x) * 4;
        CHECK_MESSAGE(texels[texel] == doctest::Approx(written[band]), "member component ", band);
    }

    std::filesystem::remove(outArchive);
}

#endif // GPU_MATERIAL_LAYOUT_FIXTURE_DIR
