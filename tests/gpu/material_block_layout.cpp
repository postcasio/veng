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
// The second case carries both claims over the array and nested-struct half of the block, where
// the question is a different one: an array's *element* stride is not the member packing above.
// Slang strides float3[3] by 12 bytes and float[3] by 4 — the tight stride, not a std430-style 16
// — and an array of a struct flattens per element into dotted members. Both arrays start at a
// non-16-aligned offset, so a packer that rounded either the start or the stride reads every
// element but the first from the wrong place.
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

    // The array members. Each element is far from its neighbours, so an element read at the wrong
    // stride lands on a different element's value rather than near the right one. The float3 array
    // is written from vec4s, whose w is dropped by the 12-byte element stride — 0.0 there would be
    // indistinguishable from an untouched block, so it carries a value the block must not show.
    constexpr std::array<vec4, 3> Tri{vec4{110.5f, 120.25f, 130.125f, -1.0f},
                                      vec4{210.5f, 220.25f, 230.125f, -2.0f},
                                      vec4{310.5f, 320.25f, 330.125f, -3.0f}};
    constexpr std::array<f32, 3> Scalars{410.5f, 420.25f, 430.125f};
    constexpr std::array<vec4, 2> Quads{vec4{510.5f, 520.25f, 530.125f, 540.0625f},
                                        vec4{610.5f, 620.25f, 630.125f, 640.0625f}};
    // The struct array's flattened leaves, addressed by their dotted names.
    constexpr std::array<vec2, 2> BandLow{vec2{710.5f, 720.25f}, vec2{810.5f, 820.25f}};
    constexpr std::array<f32, 2> BandWeight{730.125f, 830.125f};

    // The first band the array half occupies: the eleven flat member components come first.
    constexpr u32 FirstArrayBand = 11;

    // One band per member component, left to right across the output.
    constexpr u32 BandCount = 37;
    constexpr u32 BandWidth = 8;
    constexpr uvec2 Extent{BandCount * BandWidth, 4};

    // Reads one band's texel out of a downloaded RGBA32F image.
    f32 BandValue(const vector<u8>& pixels, u32 band)
    {
        const auto* texels = reinterpret_cast<const f32*>(pixels.data());
        const usize x = static_cast<usize>(band) * BandWidth + BandWidth / 2;
        return texels[(static_cast<usize>(Extent.y / 2) * Extent.x + x) * 4];
    }

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
    REQUIRE(fields.size() == 12);
    CHECK(FieldNamed(fields, "Lead").Offset == 0);
    CHECK(FieldNamed(fields, "Mid").Offset == 4);
    CHECK(FieldNamed(fields, "Region").Offset == 16);
    CHECK(FieldNamed(fields, "Tag").Offset == 32);
    CHECK(FieldNamed(fields, "Tail").Offset == 36);

    // The array half, whose claim is the element stride rather than the member packing: the
    // cursor runs on element count x tight element stride, and an array of a struct is gone by
    // the time it reaches the field table — flattened into dotted leaves.
    const MaterialField& tri = FieldNamed(fields, "Tri");
    CHECK(tri.Offset == 44);
    CHECK(tri.ElementCount == 3);
    CHECK(tri.ElementStride == 12);
    CHECK(tri.Size == 36);

    const MaterialField& scalars = FieldNamed(fields, "Scalars");
    CHECK(scalars.Offset == 80);
    CHECK(scalars.ElementCount == 3);
    CHECK(scalars.ElementStride == 4);

    const MaterialField& quads = FieldNamed(fields, "Quads");
    CHECK(quads.Offset == 92);
    CHECK(quads.ElementCount == 2);
    CHECK(quads.ElementStride == 16);

    CHECK(FieldNamed(fields, "Bands[0].Low").Offset == 124);
    CHECK(FieldNamed(fields, "Bands[0].Low").ElementCount == 1);
    CHECK(FieldNamed(fields, "Bands[0].Weight").Offset == 132);
    CHECK(FieldNamed(fields, "Bands[1].Low").Offset == 136);
    CHECK(FieldNamed(fields, "Bands[1].Weight").Offset == 144);

    // Every member written from the host; the cooked defaults are all zero, so a value that
    // arrives can only have come through the write. Two of the flat five go through a handle
    // resolved from the same name — the vector at the non-16-aligned offset and the scalar — so
    // both spellings of the setter are held to the one layout.
    material.SetParam("Lead", Lead);
    material.SetParam(material.Field("Mid"), vec4(Mid, 0.0f));
    material.SetParam("Region", Region);
    // The scalar setter copies the field's four bytes verbatim, so a uint member is written by
    // handing it the integer's bit pattern.
    material.SetParam(material.Field("Tag"), std::bit_cast<f32>(Tag));
    material.SetParam("Tail", vec4(Tail, 0.0f, 0.0f));

    // Tri and Scalars go up as one whole-array write each; Quads is written an element at a time,
    // so both spellings are held to the one stride. The flattened struct leaves are ordinary
    // fields addressed by their dotted names.
    material.SetParamArray(material.Field("Tri"), Tri);
    material.SetParamArray(material.Field("Scalars"), Scalars);
    const MaterialFieldHandle quadsHandle = material.Field("Quads");
    for (u32 i = 0; i < Quads.size(); ++i)
    {
        material.SetParam(quadsHandle, i, Quads[i]);
    }
    material.SetParam("Bands[0].Low", vec4(BandLow[0], 0.0f, 0.0f));
    material.SetParam("Bands[0].Weight", BandWeight[0]);
    material.SetParam("Bands[1].Low", vec4(BandLow[1], 0.0f, 0.0f));
    material.SetParam("Bands[1].Weight", BandWeight[1]);

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

    const auto render = [&]
    {
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

                const RenderGraph::ImportBinding bindings[] = {
                    {.Id = outputId, .View = outputView}};
                graph.Compile()->Execute(cmd, bindings);
            });

        vector<u8> pixels = outputImage->Download();
        REQUIRE(pixels.size() == static_cast<usize>(Extent.x) * Extent.y * 4 * sizeof(f32));
        return pixels;
    };

    const vector<u8> pixels = render();

    // Claim 1: each member component the shader read, band by band.
    const std::array<f32, BandCount> written{Lead,          Mid.x,        Mid.y,
                                             Mid.z,         Region.x,     Region.y,
                                             Region.z,      Region.w,     static_cast<f32>(Tag),
                                             Tail.x,        Tail.y,       Tri[0].x,
                                             Tri[0].y,      Tri[0].z,     Tri[1].x,
                                             Tri[1].y,      Tri[1].z,     Tri[2].x,
                                             Tri[2].y,      Tri[2].z,     Scalars[0],
                                             Scalars[1],    Scalars[2],   Quads[0].x,
                                             Quads[0].y,    Quads[0].z,   Quads[0].w,
                                             Quads[1].x,    Quads[1].y,   Quads[1].z,
                                             Quads[1].w,    BandLow[0].x, BandLow[0].y,
                                             BandWeight[0], BandLow[1].x, BandLow[1].y,
                                             BandWeight[1]};
    for (u32 band = 0; band < BandCount; ++band)
    {
        CHECK_MESSAGE(BandValue(pixels, band) == doctest::Approx(written[band]),
                      "member component ", band);
    }

    // An element write reaches its own element and stops. Rewriting the middle float3 leaves its
    // neighbours at the values the whole-array write gave them, which a stride that over-reached
    // — or a write bounded by the field rather than the element — would not.
    constexpr vec4 Replaced{910.5f, 920.25f, 930.125f, -9.0f};
    material.SetParam(material.Field("Tri"), 1u, Replaced);
    const vector<u8> after = render();

    CHECK(BandValue(after, FirstArrayBand + 3) == doctest::Approx(Replaced.x));
    CHECK(BandValue(after, FirstArrayBand + 4) == doctest::Approx(Replaced.y));
    CHECK(BandValue(after, FirstArrayBand + 5) == doctest::Approx(Replaced.z));
    CHECK(BandValue(after, FirstArrayBand + 2) == doctest::Approx(Tri[0].z));
    CHECK(BandValue(after, FirstArrayBand + 6) == doctest::Approx(Tri[2].x));

    std::filesystem::remove(outArchive);
}

#endif // GPU_MATERIAL_LAYOUT_FIXTURE_DIR
