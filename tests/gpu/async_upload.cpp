// Async upload + sample round-trip: the join of the planset's async upload
// path. An image is uploaded on a worker through Image::Upload(tasks, ...) — the
// copy is recorded on a per-worker transfer command buffer, submitted on the
// transfer queue under the submission lock, and the staging scratch retired on
// the transfer timeline. The image is then sampled in a graphics pass driven by
// a headless frame submit so the render graph's first graphics use acquires the
// image and folds the transfer-timeline wait into the frame submit. Reading the
// expected pixels back proves the timeline wait + ownership acquire produce a
// correct, race-free result — and gives the validation gate coverage of the
// async path.
//
// A regression guard alongside it checks the blocking Image::UploadSync path is
// unchanged.

#include <array>
#include <vector>

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
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Task/TaskSystem.h>

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

    Ref<GraphicsPipeline> CreateSamplePipeline(Context& context, Ref<PipelineLayout>& outLayout,
                                               const Ref<ShaderModule>& vertexModule, const Ref<ShaderModule>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(context, {
            .Name = "Async Upload Sample Layout",
            .PushConstantRanges = {
                PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
            },
        });

        return GraphicsPipeline::Create(context, {
            .Name = "Async Upload Sample Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
            .PipelineLayout = outLayout,
            .ShaderStages = {
                {.Stage = ShaderStage::Vertex, .Module = vertexModule},
                {.Stage = ShaderStage::Fragment, .Module = fragmentModule},
            },
        });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "async upload: upload on a worker, then sample with a frame transfer-wait")
{
    // The solid color uploaded into the source image, exactly representable in
    // RGBA8Unorm: red, fully opaque.
    constexpr std::array<u8, 4> expected = {255, 0, 0, 255};

    // The async path needs the worker pool and the per-worker transfer pools the
    // Application wires up; the bare GpuFixture has neither.
    TaskSystem tasks{TaskSystemInfo{.WorkerCount = 2}};
    Context.InitializeTransferPools(tasks);

    auto sourceImage = Image::Create(Context, {
        .Name = "Async Upload Source",
        .Extent = {Size, Size, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
    });
    auto sourceView = ImageView::Create(Context, {.Name = "Async Upload Source View", .Image = sourceImage});

    auto outputImage = Image::Create(Context, {
        .Name = "Async Upload Output",
        .Extent = {Size, Size, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
    });
    auto outputView = ImageView::Create(Context, {.Name = "Async Upload Output View", .Image = outputImage});

    auto sampler = Sampler::Create(Context, {
        .Name = "Async Upload Sampler",
        .AddressModeU = AddressMode::ClampToEdge,
        .AddressModeV = AddressMode::ClampToEdge,
        .AddressModeW = AddressMode::ClampToEdge,
    });

    // Upload the source image off the main thread. Blocking on the returned task
    // only waits the *submit*; the GPU copy is gated by the transfer timeline the
    // sample pass folds into the frame submit below.
    std::vector<u8> texels(static_cast<size_t>(Size) * Size * 4);
    for (size_t pixel = 0; pixel < static_cast<size_t>(Size) * Size; pixel++)
    {
        texels[pixel * 4 + 0] = expected[0];
        texels[pixel * 4 + 1] = expected[1];
        texels[pixel * 4 + 2] = expected[2];
        texels[pixel * 4 + 3] = expected[3];
    }

    Task<void> upload = sourceImage->Upload(tasks, texels);
    const Result<std::monostate> uploadResult = upload.Get();
    REQUIRE(uploadResult.has_value());

    // Bindless registration is the main thread's job (the single-threaded set-0
    // contract) — the worker uploaded an unregistered image.
    auto& bindless = Context.GetBindlessRegistry();
    const TextureHandle textureHandle = bindless.Register(sourceView);
    const SamplerHandle samplerHandle = bindless.Register(sampler);
    REQUIRE(textureHandle.IsValid());
    REQUIRE(samplerHandle.IsValid());

    AssetManager assets(Context, Tasks);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F44});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateSamplePipeline(Context, layout, vertexAsset->Get()->Module, fragmentAsset->Get()->Module);

    // Sample inside a headless frame: EndFrame's SubmitFrame folds in the
    // transfer-timeline wait the Sample pass registers on first graphics use, so
    // the sample cannot run before the worker's copy completes.
    CommandBuffer& cmd = Context.BeginFrame();

    RenderGraph graph(Context);
    const ResourceId outputId = graph.Import("Output");
    const ResourceId sourceId = graph.Import("Source");
    graph.AddPass("Sample Async-Uploaded Texture")
        .Color({
            .Resource = outputId,
            .Load = LoadOp::Clear,
            .Store = StoreOp::Store,
            .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
        })
        .Sample(sourceId)
        .Execute([&](PassContext& ctx)
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
        {outputId, outputView},
        {sourceId, sourceView},
    };
    graph.Compile()->Execute(cmd, bindings);

    Context.EndFrame();
    Context.WaitIdle();

    const vector<u8> pixels = outputImage->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    bindless.Release(textureHandle);
    bindless.Release(samplerHandle);

    // Drain the worker before the fixture tears the Context down: a live worker
    // must not outlive the device.
    tasks.WaitForAll();
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "blocking UploadSync still produces the uploaded pixels (regression guard)")
{
    constexpr std::array<u8, 4> expected = {0, 200, 0, 255};

    auto image = Image::Create(Context, {
        .Name = "Sync Upload Source",
        .Extent = {Size, Size, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::Sampled | ImageUsage::TransferDst | ImageUsage::TransferSrc,
    });

    std::vector<u8> texels(static_cast<size_t>(Size) * Size * 4);
    for (size_t pixel = 0; pixel < static_cast<size_t>(Size) * Size; pixel++)
    {
        texels[pixel * 4 + 0] = expected[0];
        texels[pixel * 4 + 1] = expected[1];
        texels[pixel * 4 + 2] = expected[2];
        texels[pixel * 4 + 3] = expected[3];
    }

    image->UploadSync(texels);

    const vector<u8> pixels = image->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));
}
