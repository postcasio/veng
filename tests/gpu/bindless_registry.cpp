// BindlessRegistry cases: exercises the global bindless
// descriptor subsystem (set 0) end to end.
//
//   1. Register an image view and a sampler, bind set 0, and sample the
//      registered texture in a draw — proves the registry's descriptor
//      writes and Bind() produce a working binding through the
//      texture2D[]/sampler[] arrays in bindless_sample.frag.
//   2. Register, release, and re-register through a sequence of
//      BeginFrame/EndFrame cycles — proves a released slot is not reused
//      while still possibly in flight, and is reclaimed once
//      Context::AcquireNextFrame() has cycled back to the frame-in-flight
//      index the release happened on (SlotArray::PendingRelease, mirroring
//      the per-frame retire bins).
//   3. Build a batch of textures asking for one sampling description and read
//      the sampler array's occupancy across it — proves AcquireSampler bounds
//      the array by the distinct descriptions in play rather than by how many
//      resources sample through them, and that a description differing in a
//      field the GPU acts on still takes a slot of its own.

#include <array>
#include <string>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 4;

    struct SamplePushConstants
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    // Builds a pipeline that draws a fullscreen triangle sampling a single
    // bindless-registered texture/sampler pair selected by push constants.
    Ref<GraphicsPipeline> CreateSamplePipeline(Context& context, Ref<PipelineLayout>& outLayout,
                                               const Ref<ShaderModule>& vertexModule,
                                               const Ref<ShaderModule>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(
            context, {
                         .Name = "Bindless Sample Layout",
                         .PushConstantRanges =
                             {
                                 PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
                             },
                     });

        return GraphicsPipeline::Create(
            context, {
                         .Name = "Bindless Sample Pipeline",
                         .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
                         .PipelineLayout = outLayout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vertexModule},
                                 {.Stage = ShaderStage::Fragment, .Module = fragmentModule},
                             },
                     });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: register, bind, and sample a registered texture")
{
    // Exactly representable in RGBA8Unorm: blue, fully opaque.
    constexpr std::array<u8, 4> expected = {0, 0, 255, 255};

    auto sourceImage =
        Image::Create(Context, {
                                   .Name = "Bindless Source",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                               });
    auto sourceView =
        ImageView::Create(Context, {.Name = "Bindless Source View", .Image = sourceImage});

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Bindless Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Bindless Output View", .Image = outputImage});

    auto sampler = Sampler::Create(Context, {
                                                .Name = "Bindless Test Sampler",
                                                .AddressModeU = AddressMode::ClampToEdge,
                                                .AddressModeV = AddressMode::ClampToEdge,
                                                .AddressModeW = AddressMode::ClampToEdge,
                                            });

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F44});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateSamplePipeline(Context, layout, vertexAsset->Get()->Module,
                                         fragmentAsset->Get()->Module);

    auto& bindless = Context.GetBindlessRegistry();
    const TextureHandle textureHandle = bindless.Register(sourceView);
    const SamplerHandle samplerHandle = bindless.Register(sampler);

    CHECK(textureHandle.IsValid());
    CHECK(samplerHandle.IsValid());

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId sourceId = graph.Import("Source");
            const ResourceId outputId = graph.Import("Output");

            graph.AddPass("Clear Source")
                .Color({
                    .Resource = sourceId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 1.0f, .A = 1.0f},
                })
                .Execute([](PassContext&) {});

            graph.AddPass("Sample Bindless")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Sample(sourceId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& cmd = ctx.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.SetViewport({0, 0}, {Size, Size});
                        cmd.SetScissor({0, 0}, {Size, Size});
                        bindless.Bind(cmd);
                        cmd.PushConstants(SamplePushConstants{
                            .TextureIndex = textureHandle.Index,
                            .SamplerIndex = samplerHandle.Index,
                        });
                        cmd.DrawFullscreenTriangle();
                    });

            const RenderGraph::ImportBinding bindings[] = {
                {.Id = sourceId, .View = sourceView},
                {.Id = outputId, .View = outputView},
            };
            graph.Compile()->Execute(cmd, bindings);
        });

    const vector<u8> pixels = outputImage->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    bindless.Release(textureHandle);
    bindless.Release(samplerHandle);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "bindless registry: released slots are not reused until "
                                          "they cycle through every frame in flight")
{
    auto& bindless = Context.GetBindlessRegistry();

    auto MakeView = [&](string_view name)
    {
        auto image = Image::Create(Context, {
                                                .Name = string(name),
                                                .Extent = {Size, Size, 1},
                                                .Format = Format::RGBA8Unorm,
                                                .Usage = ImageUsage::Sampled,
                                            });

        return ImageView::Create(Context, {.Name = string(name) + " View", .Image = image});
    };

    auto viewA = MakeView("Slot A");
    auto viewB = MakeView("Slot B");
    auto viewC = MakeView("Slot C");
    auto viewD = MakeView("Slot D");

    const TextureHandle handleA = bindless.Register(viewA);
    const TextureHandle handleB = bindless.Register(viewB);

    bindless.Release(handleA);

    // The slot freed by Release(handleA) must not be handed out immediately —
    // it's still potentially sampled by in-flight frames.
    const TextureHandle handleC = bindless.Register(viewC);
    CHECK(handleC.Index != handleA.Index);

    // Cycle through every frame-in-flight index so AcquireNextFrame() (called
    // from BeginFrame/EndFrame) processes handleA's deferred release.
    const u32 framesInFlight = Context.GetMaxFramesInFlight();
    for (u32 i = 0; i < framesInFlight + 1; i++)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }

    const TextureHandle handleD = bindless.Register(viewD);
    CHECK(handleD.Index == handleA.Index);

    bindless.Release(handleB);
    bindless.Release(handleC);
    bindless.Release(handleD);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: textures sharing a sampling description share one slot")
{
    AssetManager assets(Context, Tasks, Types);
    const BindlessRegistry& bindless = Context.GetBindlessRegistry();

    constexpr std::array<u8, 4> pixel = {255, 255, 255, 255};

    // Every texture but the last asks for the same sampling and a different debug name, so the
    // name plays no part in what is shared.
    const auto BuildTexture = [&](const string& name, const AddressMode address)
    {
        return assets.BuildSync<Texture>(TextureData{
            .Name = name,
            .Extent = {1, 1},
            .Format = Format::RGBA8Unorm,
            .Pixels = pixel,
            .Sampler =
                {
                    .AddressModeU = address,
                    .AddressModeV = address,
                    .AddressModeW = address,
                },
        });
    };

    // The baseline is taken after one build rather than before it: the first texture in a run is
    // what mints the shared sampler, so the readings bracket what a further texture costs instead
    // of asserting the first one away.
    vector<AssetHandle<Texture>> textures;
    textures.push_back(BuildTexture("Shared Sampler Warm-up", AddressMode::ClampToEdge));

    const BindlessCapacity before = bindless.GetFreeSlots();
    REQUIRE(before.Samplers > 0);

    constexpr u32 Count = 32;
    for (u32 i = 0; i < Count; i++)
    {
        textures.push_back(
            BuildTexture("Shared Sampler " + std::to_string(i), AddressMode::ClampToEdge));
    }

    const BindlessCapacity after = bindless.GetFreeSlots();

    // A sampler is pure state, so an identical description is the same sampler: the whole batch
    // reads through the slot the warm-up took. Each texture still holds an image slot of its own,
    // which is what makes the sampler reading a statement about sharing rather than about nothing
    // having been built.
    CHECK(after.Samplers == before.Samplers);
    CHECK(after.Textures == before.Textures - Count);

    const u32 sharedSlot = textures.front().Get()->GetSamplerHandle().Index;
    for (const AssetHandle<Texture>& texture : textures)
    {
        REQUIRE(texture.Get() != nullptr);
        CHECK(texture.Get()->GetSamplerHandle().Index == sharedSlot);
    }

    // A description that differs in a field the GPU acts on takes a slot of its own — the cache
    // matches on the sampling, not on the asking.
    const AssetHandle<Texture> repeating = BuildTexture("Repeating", AddressMode::Repeat);
    CHECK(bindless.GetFreeSlots().Samplers == before.Samplers - 1);
    CHECK(repeating.Get()->GetSamplerHandle().Index != sharedSlot);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: a typed registry holds exactly its own view type")
{
    // The homogeneity invariant is the whole point of the typed sets: a 3D view registers into the
    // volume set and a cube view into the cube set, each leaving the 2D array untouched — which is
    // what keeps a non-2D descriptor out of set 0's Metal argument buffer. Proved by the free-slot
    // accounting: registering into one set decrements that set's count and no other.
    BindlessRegistry& bindless = Context.GetBindlessRegistry();

    const Ref<Image> volumeImage =
        Image::Create(Context, {
                                   .Name = "Homogeneity Volume",
                                   .Extent = {4, 4, 4},
                                   .Format = Format::RGBA16Sfloat,
                                   .Type = ImageType::Type3D,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    const Ref<ImageView> volumeView =
        ImageView::Create(Context, {.Name = "Homogeneity Volume View",
                                    .Image = volumeImage,
                                    .ViewType = ImageViewType::Type3D});

    const Ref<Image> cubeImage =
        Image::Create(Context, {
                                   .Name = "Homogeneity Cube",
                                   .Extent = {4, 4, 1},
                                   .Layers = 6,
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    const Ref<ImageView> cubeView = ImageView::Create(Context, {.Name = "Homogeneity Cube View",
                                                                .Image = cubeImage,
                                                                .ViewType = ImageViewType::Cube,
                                                                .ArrayLayers = 6});

    const BindlessCapacity before = bindless.GetFreeSlots();

    const VolumeHandle volume = bindless.RegisterVolume(volumeView);
    REQUIRE(volume.IsValid());
    const BindlessCapacity afterVolume = bindless.GetFreeSlots();
    CHECK(afterVolume.Volumes == before.Volumes - 1);
    CHECK(afterVolume.Textures == before.Textures); // the 2D array is untouched
    CHECK(afterVolume.Cubes == before.Cubes);

    const CubeHandle cube = bindless.RegisterCube(cubeView);
    REQUIRE(cube.IsValid());
    const BindlessCapacity afterCube = bindless.GetFreeSlots();
    CHECK(afterCube.Cubes == before.Cubes - 1);
    CHECK(afterCube.Volumes == afterVolume.Volumes); // the volume array is untouched
    CHECK(afterCube.Textures == before.Textures);

    bindless.Release(volume);
    bindless.Release(cube);
}
