// Ring-buffered material parameter cases: prove the N-buffered, host-mapped
// per-material block buffer (set 0, binding 4) is written safely per frame and
// read correctly through the per-frame dynamic offset.
//
//   1. Per-frame mutation: a material's param changes every frame; the draw
//      that frame must read that frame's value (no tearing, no stale read from
//      a prior in-flight region).
//   2. Write-once stability: a material registered once and never updated reads
//      the same value on every frame across the in-flight window (the value
//      flushed to every region).
//   3. Reuse after release: a released material's owed flushes do not follow its
//      range into the material allocated over it.
//   4. Two views, one frame: the ring is per frame-in-flight and not per view, so a
//      write made between two views is the value both of them read at submit.
//   5. Ranged writes: a write of one field's bytes reaches every region, leaving the
//      block's other fields intact in all of them, and two writes before a frame
//      advance both reach every region.
//   6. Packed neighbours: interleaved ranged writes to two materials whose blocks are
//      adjacent in the arena leave each other's bytes untouched.
//
// Each frame draws a fullscreen triangle whose fragment shader loads the
// material block at the selector it is pushed and outputs the param as color;
// the value is read back from the off-screen target.

#include <array>
#include <cstdlib>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 4;

    struct MaterialPush
    {
        u32 MaterialOffset;
    };

    Ref<GraphicsPipeline> CreateMaterialPipeline(Context& context, Ref<PipelineLayout>& outLayout,
                                                 const Ref<ShaderModule>& vertexModule,
                                                 const Ref<ShaderModule>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(
            context, {
                         .Name = "Material Param Layout",
                         .PushConstantRanges =
                             {
                                 PushConstantRange::Of<MaterialPush>(ShaderStage::Fragment),
                             },
                     });

        return GraphicsPipeline::Create(
            context, {
                         .Name = "Material Param Pipeline",
                         .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
                         .PipelineLayout = outLayout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vertexModule},
                                 {.Stage = ShaderStage::Fragment, .Module = fragmentModule},
                             },
                     });
    }

    // A block whose first 16 bytes hold one float4 the test shader reads.
    std::array<std::byte, 16> MakeBlock(const vec4& value)
    {
        std::array<std::byte, 16> block{};
        std::memcpy(block.data(), &value, sizeof(value));
        return block;
    }

    // A block holding two float4s the test shader reads one at a time, by pushing the field's
    // byte offset beside the block's.
    std::array<std::byte, 32> MakePairBlock(const vec4& first, const vec4& second)
    {
        std::array<std::byte, 32> block{};
        std::memcpy(block.data(), &first, sizeof(first));
        std::memcpy(block.data() + sizeof(first), &second, sizeof(second));
        return block;
    }

    // The bytes of one float4, for a ranged write of a single field.
    std::array<std::byte, 16> FieldBytes(const vec4& value)
    {
        std::array<std::byte, 16> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(value));
        return bytes;
    }

    // Renders one frame sampling the material block at the given byte offset within it and returns
    // the center pixel. An empty beforeDraw is the ordinary frame; a non-empty one runs while the
    // frame's command buffer is recording, so the writes it makes take the immediate write path
    // into the region this frame's draw reads.
    std::array<u8, 4> RenderFrame(Context& context, BindlessRegistry& bindless,
                                  const Ref<GraphicsPipeline>& pipeline,
                                  const Ref<Image>& outputImage, const Ref<ImageView>& outputView,
                                  MaterialHandle material, u32 fieldOffset = 0,
                                  const function<void()>& beforeDraw = {})
    {
        CommandBuffer& cmd = context.BeginFrame();

        RenderGraph graph(context);
        const ResourceId outputId = graph.Import("Output");

        graph.AddPass("Draw Material Param")
            .Color({
                .Resource = outputId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Execute(
                [&](PassContext& ctx)
                {
                    if (beforeDraw)
                    {
                        beforeDraw();
                    }
                    CommandBuffer& passCmd = ctx.Cmd();
                    passCmd.BindPipeline(pipeline);
                    passCmd.SetViewport({0, 0}, {Size, Size});
                    passCmd.SetScissor({0, 0}, {Size, Size});
                    bindless.Bind(passCmd);
                    // Fold the current frame's region base into the selector so the
                    // shader's load reads this frame's region.
                    passCmd.PushConstants(
                        MaterialPush{.MaterialOffset = bindless.GetCurrentFrameBase() +
                                                       material.Offset + fieldOffset});
                    passCmd.DrawFullscreenTriangle();
                });

        const RenderGraph::ImportBinding bindings[] = {{.Id = outputId, .View = outputView}};
        graph.Compile()->Execute(cmd, bindings);

        context.EndFrame();
        context.WaitIdle();

        const vector<u8> pixels = outputImage->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
        return {pixels[0], pixels[1], pixels[2], pixels[3]};
    }

    // One frame, two passes of different extents — the shape two Viewport::Renders take — with a
    // parameter write between them. Returns each pass's center pixel.
    struct TwoViewPixels
    {
        std::array<u8, 4> First;
        std::array<u8, 4> Second;
    };

    TwoViewPixels RenderTwoViews(Context& context, BindlessRegistry& bindless,
                                 const Ref<GraphicsPipeline>& pipeline,
                                 const Ref<Image>& firstImage, const Ref<ImageView>& firstView,
                                 const Ref<Image>& secondImage, const Ref<ImageView>& secondView,
                                 MaterialHandle material, const vec4& betweenViews)
    {
        CommandBuffer& cmd = context.BeginFrame();

        RenderGraph graph(context);
        const ResourceId firstId = graph.Import("First");
        const ResourceId secondId = graph.Import("Second");

        const auto draw = [&](PassContext& ctx, const uvec2 extent)
        {
            CommandBuffer& passCmd = ctx.Cmd();
            passCmd.BindPipeline(pipeline);
            passCmd.SetViewport({0, 0}, extent);
            passCmd.SetScissor({0, 0}, extent);
            bindless.Bind(passCmd);
            passCmd.PushConstants(
                MaterialPush{.MaterialOffset = bindless.GetCurrentFrameBase() + material.Offset});
            passCmd.DrawFullscreenTriangle();
        };

        graph.AddPass("First View")
            .Color({
                .Resource = firstId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Execute([&](PassContext& ctx) { draw(ctx, {Size, Size}); });

        graph.AddPass("Second View")
            .Color({
                .Resource = secondId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Execute(
                [&](PassContext& ctx)
                {
                    // The second view's own value, written while the first view's draw is already
                    // recorded into this frame's command buffer.
                    const auto block = MakeBlock(betweenViews);
                    bindless.UpdateMaterial(material, std::span<const std::byte>(block));
                    draw(ctx, {Size * 2, Size * 2});
                });

        const RenderGraph::ImportBinding bindings[] = {{.Id = firstId, .View = firstView},
                                                       {.Id = secondId, .View = secondView}};
        graph.Compile()->Execute(cmd, bindings);

        context.EndFrame();
        context.WaitIdle();

        const vector<u8> first = firstImage->Download();
        const vector<u8> second = secondImage->Download();
        return {.First = {first[0], first[1], first[2], first[3]},
                .Second = {second[0], second[1], second[2], second[3]}};
    }

    // Renders to RGBA8Unorm, so a [0,1] float channel quantizes to ~round(c*255);
    // allow a 1-LSB tolerance for the device's rounding of the sRGB-free unorm
    // store.
    bool ChannelNear(u8 actual, f32 expected)
    {
        const i32 want = static_cast<i32>(expected * 255.0f + 0.5f);
        return std::abs(static_cast<i32>(actual) - want) <= 1;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ring-buffered material: a per-frame param change reads its own value each frame")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Ring Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Ring Output View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();

    // A distinct value per frame, spanning more than framesInFlight frames so a
    // stale region from a prior in-flight frame would be caught as a wrong pixel.
    const std::array<vec4, 6> perFrame = {
        vec4{0.2f, 0.0f, 0.0f, 1.0f}, vec4{0.4f, 0.0f, 0.0f, 1.0f}, vec4{0.6f, 0.0f, 0.0f, 1.0f},
        vec4{0.8f, 0.0f, 0.0f, 1.0f}, vec4{0.1f, 0.0f, 0.0f, 1.0f}, vec4{0.9f, 0.0f, 0.0f, 1.0f},
    };

    const auto initial = MakeBlock(perFrame[0]);
    const MaterialHandle material = bindless.RegisterMaterial(std::span<const std::byte>(initial));
    CHECK(material.IsValid());

    for (usize i = 0; i < perFrame.size(); i++)
    {
        // Update before recording this frame; the draw must read this value, not
        // a value left in the region by an earlier in-flight frame.
        const auto block = MakeBlock(perFrame[i]);
        bindless.UpdateMaterial(material, std::span<const std::byte>(block));

        const std::array<u8, 4> pixel =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material);

        CHECK(ChannelNear(pixel[0], perFrame[i].x));
    }

    bindless.Release(material);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ring-buffered material: a released material's owed flushes do not reach the "
                  "material allocated over its range")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Ring Output Reuse",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Ring Output Reuse View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();
    const u32 window = Context.GetMaxFramesInFlight();

    // Settle whatever earlier cases left parked, so the arena is one run and first fit is
    // predictable: the reuse this case is about has to be the reuse it observes.
    const MaterialHandle none{.Offset = 0};
    for (u32 i = 0; i < window + 1; i++)
    {
        RenderFrame(Context, bindless, pipeline, outputImage, outputView, none);
    }

    // A guard below the range under test, so the reclaimed range is the lowest free run rather
    // than a hole the arena's start would be handed out ahead of.
    const auto guardBlock = MakeBlock(vec4{0.0f, 1.0f, 0.0f, 1.0f});
    const MaterialHandle guard = bindless.RegisterMaterial(std::span<const std::byte>(guardBlock));
    REQUIRE(guard.IsValid());

    // Register, update so the entry owes a flush to every other region, then release with that
    // debt outstanding. Blocks are packed and offsets are reused, so an entry that kept its debt
    // would write these bytes into whatever material takes the range next.
    const vec4 departed{0.9f, 0.0f, 0.0f, 1.0f};
    const auto departedBlock = MakeBlock(departed);
    const MaterialHandle first =
        bindless.RegisterMaterial(std::span<const std::byte>(departedBlock));
    REQUIRE(first.IsValid());
    bindless.UpdateMaterial(first, std::span<const std::byte>(departedBlock));
    bindless.Release(first);

    // Cycle past the deferred-release window so the range is allocatable again, then take it with
    // a same-sized block — first fit hands back the range just reclaimed.
    for (u32 i = 0; i < window + 1; i++)
    {
        RenderFrame(Context, bindless, pipeline, outputImage, outputView, guard);
    }

    const vec4 arrived{0.0f, 0.0f, 0.4f, 1.0f};
    const auto arrivedBlock = MakeBlock(arrived);
    const MaterialHandle second =
        bindless.RegisterMaterial(std::span<const std::byte>(arrivedBlock));
    REQUIRE(second.IsValid());
    REQUIRE(second.Offset == first.Offset);

    // The new material's bytes hold across the whole window a stale flush would have landed in.
    for (u32 i = 0; i < window + 3; i++)
    {
        const std::array<u8, 4> pixel =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, second);
        CHECK(ChannelNear(pixel[0], arrived.x));
        CHECK(ChannelNear(pixel[2], arrived.z));
    }

    bindless.Release(guard);
    bindless.Release(second);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "ring-buffered material: a write-once material is stable across the in-flight window")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Ring Output Stable",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Ring Output Stable View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();

    const vec4 value{0.0f, 0.5f, 0.0f, 1.0f};
    const auto block = MakeBlock(value);
    const MaterialHandle material = bindless.RegisterMaterial(std::span<const std::byte>(block));
    CHECK(material.IsValid());

    // Never updated again: the registered value must have flushed to every region
    // over the first framesInFlight frames, so every later frame reads it.
    const u32 frames = Context.GetMaxFramesInFlight() + 3;
    for (u32 i = 0; i < frames; i++)
    {
        const std::array<u8, 4> pixel =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material);

        CHECK(ChannelNear(pixel[1], value.y));
    }

    bindless.Release(material);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ring-buffered material: a block written between two views of one frame is the "
                  "written value for both of them")
{
    // The material arena rings by frame-in-flight and not by view, so this is what a material's
    // block *means* across two views: both views' draws read the host value standing at submit, and
    // the view that wrote last decides it for every view. The contract
    // BindlessRegistry::MaterialArenaBytes states, executable — a consumer needing a per-view value
    // needs a per-view instance, and this is the behaviour that makes that necessary.
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    // Two targets of different extents, the case MaxViewsPerFrame's own doc names: an editor
    // renders one viewport per visible panel and they are not the same size.
    auto firstImage =
        Image::Create(Context, {
                                   .Name = "Two View First",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto firstView =
        ImageView::Create(Context, {.Name = "Two View First View", .Image = firstImage});
    auto secondImage =
        Image::Create(Context, {
                                   .Name = "Two View Second",
                                   .Extent = {Size * 2, Size * 2, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto secondView =
        ImageView::Create(Context, {.Name = "Two View Second View", .Image = secondImage});

    auto& bindless = Context.GetBindlessRegistry();

    const vec4 firstValue{0.8f, 0.0f, 0.0f, 1.0f};
    const vec4 secondValue{0.2f, 0.0f, 0.0f, 1.0f};
    const auto initial = MakeBlock(firstValue);
    const MaterialHandle material = bindless.RegisterMaterial(std::span<const std::byte>(initial));
    REQUIRE(material.IsValid());

    const TwoViewPixels pixels = RenderTwoViews(Context, bindless, pipeline, firstImage, firstView,
                                                secondImage, secondView, material, secondValue);

    // Both views read the second value: the first view's draw was recorded before the write but
    // reads the buffer at submit, after it.
    CHECK(ChannelNear(pixels.First[0], secondValue.x));
    CHECK(ChannelNear(pixels.Second[0], secondValue.x));

    bindless.Release(material);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ring-buffered material: a ranged write reaches every region and leaves the "
                  "block's other fields standing")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Ranged Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Ranged Output View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();

    const vec4 registeredSecond{0.0f, 0.6f, 0.0f, 1.0f};
    const auto initial = MakePairBlock(vec4{0.1f, 0.0f, 0.0f, 1.0f}, registeredSecond);
    const MaterialHandle material = bindless.RegisterMaterial(std::span<const std::byte>(initial));
    REQUIRE(material.IsValid());

    // One field written by range, the other left alone: the replication into the regions this
    // frame did not touch must carry the written bytes and nothing else.
    const vec4 written{0.9f, 0.0f, 0.0f, 1.0f};
    const auto writtenBytes = FieldBytes(written);
    bindless.UpdateMaterial(material, 0, std::span<const std::byte>(writtenBytes));

    const u32 frames = Context.GetMaxFramesInFlight() + 2;
    for (u32 i = 0; i < frames; i++)
    {
        const std::array<u8, 4> first =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material, 0);
        const std::array<u8, 4> second =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material, 16);
        CHECK(ChannelNear(first[0], written.x));
        CHECK(ChannelNear(second[1], registeredSecond.y));
    }

    // Two ranged writes with no frame advance between them: the owed replication covers both,
    // so no region holds one field's new bytes beside the other's old ones.
    const vec4 pairFirst{0.2f, 0.0f, 0.0f, 1.0f};
    const vec4 pairSecond{0.0f, 0.4f, 0.0f, 1.0f};
    const auto pairFirstBytes = FieldBytes(pairFirst);
    const auto pairSecondBytes = FieldBytes(pairSecond);
    bindless.UpdateMaterial(material, 0, std::span<const std::byte>(pairFirstBytes));
    bindless.UpdateMaterial(material, 16, std::span<const std::byte>(pairSecondBytes));

    for (u32 i = 0; i < frames; i++)
    {
        const std::array<u8, 4> first =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material, 0);
        const std::array<u8, 4> second =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material, 16);
        CHECK(ChannelNear(first[0], pairFirst.x));
        CHECK(ChannelNear(second[1], pairSecond.y));
    }

    bindless.Release(material);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ring-buffered material: interleaved ranged writes to adjacent blocks leave each "
                  "other's bytes untouched")
{
    // Blocks are packed, so a ranged write's destination is the block offset plus the field
    // offset: getting that arithmetic wrong writes into the next material rather than into this
    // one's padding, which is the failure mode the packed arena introduces.
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Adjacent Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Adjacent Output View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();
    const u32 window = Context.GetMaxFramesInFlight();

    // Settle whatever earlier cases left parked, so the arena is one run and first fit places the
    // two blocks next to each other — which is what this case is about.
    const MaterialHandle none{.Offset = 0};
    for (u32 i = 0; i < window + 1; i++)
    {
        RenderFrame(Context, bindless, pipeline, outputImage, outputView, none);
    }

    const auto lowerInitial =
        MakePairBlock(vec4{0.0f, 0.0f, 0.0f, 1.0f}, vec4{0.0f, 0.0f, 0.0f, 1.0f});
    const MaterialHandle lower =
        bindless.RegisterMaterial(std::span<const std::byte>(lowerInitial));
    const MaterialHandle upper =
        bindless.RegisterMaterial(std::span<const std::byte>(lowerInitial));
    REQUIRE(lower.IsValid());
    REQUIRE(upper.IsValid());
    REQUIRE(upper.Offset == lower.Offset + BindlessRegistry::MaterialGranuleBytes);

    // Interleaved, so a write that landed in the neighbour would be overwritten by the
    // neighbour's own next write rather than standing where a single ordering would show it.
    const vec4 lowerFirst{0.9f, 0.0f, 0.0f, 1.0f};
    const vec4 upperFirst{0.3f, 0.0f, 0.0f, 1.0f};
    const vec4 lowerSecond{0.0f, 0.7f, 0.0f, 1.0f};
    const vec4 upperSecond{0.0f, 0.1f, 0.0f, 1.0f};
    const auto writeAll = [&]
    {
        const auto lowerFirstBytes = FieldBytes(lowerFirst);
        const auto upperFirstBytes = FieldBytes(upperFirst);
        const auto lowerSecondBytes = FieldBytes(lowerSecond);
        const auto upperSecondBytes = FieldBytes(upperSecond);
        bindless.UpdateMaterial(lower, 0, std::span<const std::byte>(lowerFirstBytes));
        bindless.UpdateMaterial(upper, 0, std::span<const std::byte>(upperFirstBytes));
        bindless.UpdateMaterial(lower, 16, std::span<const std::byte>(lowerSecondBytes));
        bindless.UpdateMaterial(upper, 16, std::span<const std::byte>(upperSecondBytes));
    };

    struct Read
    {
        MaterialHandle Material;
        u32 FieldOffset;
        u32 Channel;
        f32 Expected;
    };
    const std::array<Read, 4> reads = {
        Read{.Material = lower, .FieldOffset = 0, .Channel = 0, .Expected = lowerFirst.x},
        Read{.Material = lower, .FieldOffset = 16, .Channel = 1, .Expected = lowerSecond.y},
        Read{.Material = upper, .FieldOffset = 0, .Channel = 0, .Expected = upperFirst.x},
        Read{.Material = upper, .FieldOffset = 16, .Channel = 1, .Expected = upperSecond.y},
    };

    // Written mid-frame, so each read is of the region the writes landed in directly — the
    // immediate path, where a destination off by a block lands in the neighbour and stays there.
    for (const Read& read : reads)
    {
        const std::array<u8, 4> pixel =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, read.Material,
                        read.FieldOffset, writeAll);
        CHECK(ChannelNear(pixel[read.Channel], read.Expected));
    }

    // And the replication into the other regions keeps them apart the same way.
    for (u32 i = 0; i < window + 2; i++)
    {
        for (const Read& read : reads)
        {
            const std::array<u8, 4> pixel =
                RenderFrame(Context, bindless, pipeline, outputImage, outputView, read.Material,
                            read.FieldOffset);
            CHECK(ChannelNear(pixel[read.Channel], read.Expected));
        }
    }

    bindless.Release(lower);
    bindless.Release(upper);
}
