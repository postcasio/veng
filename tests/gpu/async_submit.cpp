// Asynchronous queue submission: the driver encodes a submitted frame on its own thread after
// vkQueueSubmit returns, so anything the engine changes after the submit and before that frame
// retires must be something the frame no longer reads. This churns exactly those paths, windowed so
// the swap chain is in play, and requires the frames to read back identically in the asynchronous
// and synchronous modes:
//   - a texture created, cleared through RecordSetupCommands and registered into a bindless slot
//     every frame, sampled that frame and two frames later, then released — inside the frame on
//     even frames, between frames on odd ones, so a released slot that came back too early is
//     re-registered over a texture a submitted frame still samples;
//   - a host-written ring sliced by frame in flight;
//   - the output target recreated at a new extent mid-run, and the window resized with it;
//   - (macOS) the window minimized and restored mid-run, with frames already queued.
// Each frame's output names what it read in separate channels, so a mode that diverges points at
// the path that raced.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/QueueSubmitMode.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Window.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId FullscreenVertId{0x1F42};
    constexpr AssetId BindlessSampleFragId{0x1F44};
    constexpr AssetId AsyncSubmitFragId{0x403C93E063DBAA52ULL};

    constexpr u32 ChurnFrames = 24;
    constexpr u32 ResizeFrame = 12;
    constexpr u32 MinimizeFrame = 18;
    constexpr u32 EarlierDistance = 2;
    constexpr u64 RingSliceBytes = 256;
    constexpr uvec2 InitialExtent{8, 8};
    constexpr uvec2 ResizedExtent{12, 4};

    /// @brief Push block of async_submit.frag.
    struct ChurnPush
    {
        u32 CurrentTexture;
        u32 EarlierTexture;
        u32 SamplerIndex;
        u32 RingBuffer;
        u32 RingOffset;
    };

    /// @brief Push block of bindless_sample.frag.
    struct SamplePush
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    /// @brief The value frame @p frame's texture is cleared to; exact in half precision.
    f32 TextureValue(const u32 frame)
    {
        return static_cast<f32>(frame + 1) / 64.0f;
    }

    /// @brief The value frame @p frame writes into its ring slice; exact in half precision.
    f32 RingValue(const u32 frame)
    {
        return static_cast<f32>(frame + 1) / 128.0f;
    }

    struct ChurnRun
    {
        /// @brief Each churn frame's output, read back as RGBA16F.
        vector<vector<u8>> Frames;
        /// @brief The extent each frame's output was read back at.
        vector<uvec2> Extents;
        /// @brief The mode the context resolved.
        ResolvedQueueSubmitMode Mode;
        /// @brief Whether the window reported minimized before the parked frame.
        bool Minimized = false;
        /// @brief Whether the window reported restored once the parked frame began.
        bool Restored = false;
        /// @brief How long the parked frame's BeginFrame took, in seconds.
        f64 ParkedSeconds = 0.0;
        /// @brief The swap chain extent before the window resize.
        uvec2 SwapExtentBefore{};
        /// @brief The swap chain extent at the end of the run.
        uvec2 SwapExtentAfter{};
    };

    Ref<GraphicsPipeline> CreatePipeline(Context& context, AssetManager& assets,
                                         const AssetId fragment, const Format format,
                                         const PushConstantRange& push, const char* name)
    {
        const AssetResult<AssetHandle<Shader>> vs = assets.LoadSync<Shader>(FullscreenVertId);
        const AssetResult<AssetHandle<Shader>> fs = assets.LoadSync<Shader>(fragment);
        REQUIRE(vs.has_value());
        REQUIRE(fs.has_value());
        const Ref<PipelineLayout> layout =
            PipelineLayout::Create(context, {.Name = name, .PushConstantRanges = {push}});
        return GraphicsPipeline::Create(
            context, {
                         .Name = name,
                         .ColorAttachments = {{.Format = format}},
                         .PipelineLayout = layout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vs->Get()->Module},
                                 {.Stage = ShaderStage::Fragment, .Module = fs->Get()->Module},
                             },
                     });
    }

    /// @brief Draws a fullscreen pass into @p target, letting the graph order it.
    void DrawInto(Context& context, CommandBuffer& cmd, const Ref<ImageView>& target,
                  const uvec2 extent, const std::function<void(CommandBuffer&)>& draw)
    {
        RenderGraph graph(context);
        const ResourceId output = graph.Import("Async Submit Target");
        graph.AddPass("Async Submit Draw")
            .Color({
                .Resource = output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
            })
            .Execute(
                [&](PassContext& pass)
                {
                    CommandBuffer& passCmd = pass.Cmd();
                    passCmd.SetViewport({0, 0}, extent);
                    passCmd.SetScissor({0, 0}, extent);
                    draw(passCmd);
                });
        const RenderGraph::ImportBinding binding{.Id = output, .View = target};
        graph.Compile()->Execute(cmd, {&binding, 1});
    }

    /// @brief A 1x1 texture cleared to @p value in red through RecordSetupCommands.
    Ref<ImageView> CreateFrameTexture(Context& context, const f32 value)
    {
        const Ref<Image> image =
            Image::Create(context, {
                                       .Name = "Async Submit Frame Texture",
                                       .Extent = {1, 1, 1},
                                       .Format = Format::RGBA16Sfloat,
                                       .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                                   });
        Ref<ImageView> view =
            ImageView::Create(context, {.Name = "Async Submit Frame Texture", .Image = image});
        context.RecordSetupCommands(
            [&context, view, value](CommandBuffer& cmd)
            {
                RenderGraph graph(context);
                const ResourceId target = graph.Import("Async Submit Frame Texture");
                graph.AddPass("Async Submit Texture Clear")
                    .Color({
                        .Resource = target,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = value, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target, .View = view};
                graph.Compile()->Execute(cmd, {&binding, 1});
                cmd.PrepareForAccess(view, AccessKind::SampleGraphics);
            });
        return view;
    }

    /// @brief Pumps the window until @p predicate holds or two seconds pass.
    bool PumpUntil(Window& window, const std::function<bool()>& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            window.Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    ChurnRun RunChurn(const QueueSubmitMode mode)
    {
        ChurnRun run;
        run.Frames.resize(ChurnFrames);
        run.Extents.resize(ChurnFrames);

        const Unique<Window> window = Window::Create({
            .Extent = {320, 240},
            .Resizable = true,
            .Title = "Async Submit Test",
            .CaptureMouse = false,
        });

        Context context;
        context.Initialize({.ApplicationName = "Async Submit Test", .SubmitMode = mode},
                           window.get());
        run.Mode = context.GetQueueSubmitMode();

        TaskSystem tasks{TaskSystemInfo{.WorkerCount = 2}};
        TypeRegistry types;
        {
            AssetManager assets(context, tasks, types);
            REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

            const Ref<GraphicsPipeline> churnPipeline = CreatePipeline(
                context, assets, AsyncSubmitFragId, Format::RGBA16Sfloat,
                PushConstantRange::Of<ChurnPush>(ShaderStage::Fragment), "Async Submit Churn");
            const Ref<GraphicsPipeline> presentPipeline = CreatePipeline(
                context, assets, BindlessSampleFragId, context.GetSwapChainFormat(),
                PushConstantRange::Of<SamplePush>(ShaderStage::Fragment), "Async Submit Present");

            BindlessRegistry& bindless = context.GetBindlessRegistry();
            const SharedSampler sampler = bindless.AcquireSampler({
                .Name = "Async Submit Sampler",
                .MagFilter = Filter::Nearest,
                .MinFilter = Filter::Nearest,
                .AddressModeU = AddressMode::ClampToEdge,
                .AddressModeV = AddressMode::ClampToEdge,
                .AddressModeW = AddressMode::ClampToEdge,
            });

            const u32 framesInFlight = context.GetMaxFramesInFlight();
            const Ref<Buffer> ring =
                Buffer::Create(context, {
                                            .Name = "Async Submit Ring",
                                            .Size = RingSliceBytes * framesInFlight,
                                            .Usage = BufferUsage::Storage,
                                            .HostMapped = true,
                                        });
            const StorageBufferHandle ringHandle = bindless.Register(ring);

            Ref<Image> output;
            Ref<ImageView> outputView;
            TextureHandle outputHandle;
            uvec2 outputExtent{};
            const auto createOutput = [&](const uvec2 extent)
            {
                bindless.Release(outputHandle);
                output = Image::Create(context,
                                       {
                                           .Name = "Async Submit Output",
                                           .Extent = {extent.x, extent.y, 1},
                                           .Format = Format::RGBA16Sfloat,
                                           .Usage = ImageUsage::ColorAttachment |
                                                    ImageUsage::Sampled | ImageUsage::TransferSrc,
                                       });
                outputView =
                    ImageView::Create(context, {.Name = "Async Submit Output", .Image = output});
                outputHandle = bindless.Register(outputView);
                outputExtent = extent;
            };
            createOutput(InitialExtent);

            struct FrameTexture
            {
                Ref<ImageView> View;
                TextureHandle Handle;
            };
            vector<FrameTexture> textures(ChurnFrames);

            for (u32 frame = 0; frame < ChurnFrames; ++frame)
            {
                window->Update();

                if (frame == ResizeFrame)
                {
                    run.SwapExtentBefore = context.GetSwapChainExtent();
                    window->ApplyDisplayMode(FullscreenMode::Windowed, 0, {400, 200}, 0);
                }

#ifdef __APPLE__
                if (frame == MinimizeFrame)
                {
                    window->Minimize();
                    run.Minimized = PumpUntil(*window, [&] { return window->IsMinimized(); });
                    // Restored from the main queue while the next BeginFrame is parked, the only
                    // thing pumping the main run loop then.
                    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 300'000'000),
                                     dispatch_get_main_queue(), window.get(),
                                     [](void* target) { static_cast<Window*>(target)->Restore(); });
                }
#endif

                // Odd frames create their texture before the frame opens, so its clear is held and
                // recorded at the head of the next BeginFrame.
                if (frame % 2 == 1)
                {
                    textures[frame].View = CreateFrameTexture(context, TextureValue(frame));
                }

                const auto beginStart = std::chrono::steady_clock::now();
                CommandBuffer& cmd = context.BeginFrame();
                if (frame == MinimizeFrame)
                {
                    run.Restored = !window->IsMinimized();
                    run.ParkedSeconds =
                        std::chrono::duration<f64>(std::chrono::steady_clock::now() - beginStart)
                            .count();
                }

                if (frame % 2 == 0)
                {
                    textures[frame].View = CreateFrameTexture(context, TextureValue(frame));
                }
                textures[frame].Handle = bindless.Register(textures[frame].View);

                if (frame == ResizeFrame)
                {
                    createOutput(ResizedExtent);
                }

                const u32 slot = context.GetCurrentFrameInFlight();
                const vec4 ringValue{RingValue(frame), 0.0f, 0.0f, 0.0f};
                std::memcpy(static_cast<u8*>(ring->GetMappedData()) + (slot * RingSliceBytes),
                            &ringValue, sizeof(ringValue));

                const u32 earlier = frame >= EarlierDistance ? frame - EarlierDistance : frame;
                DrawInto(context, cmd, outputView, outputExtent,
                         [&](CommandBuffer& passCmd)
                         {
                             passCmd.BindPipeline(churnPipeline);
                             bindless.Bind(passCmd);
                             passCmd.PushConstants(ChurnPush{
                                 .CurrentTexture = textures[frame].Handle.Index,
                                 .EarlierTexture = textures[earlier].Handle.Index,
                                 .SamplerIndex = sampler.Handle.Index,
                                 .RingBuffer = ringHandle.Index,
                                 .RingOffset = static_cast<u32>(slot * RingSliceBytes),
                             });
                             passCmd.DrawFullscreenTriangle();
                         });

                run.Extents[frame] = outputExtent;
                context.GetAsyncReadback().Request({
                    .Name = "Async Submit Readback",
                    .Image = output,
                    .OnComplete = [&run, frame](const std::span<const u8> bytes)
                    { run.Frames[frame].assign(bytes.begin(), bytes.end()); },
                });

                // The swap chain shows the output, so every frame's encode acquires a drawable.
                cmd.PrepareForAccess(outputView, AccessKind::SampleGraphics);
                const uvec2 swapExtent = context.GetSwapChainExtent();
                DrawInto(context, cmd, context.GetCurrentSwapChainImageView(), swapExtent,
                         [&](CommandBuffer& passCmd)
                         {
                             passCmd.BindPipeline(presentPipeline);
                             bindless.Bind(passCmd);
                             passCmd.PushConstants(SamplePush{
                                 .TextureIndex = outputHandle.Index,
                                 .SamplerIndex = sampler.Handle.Index,
                             });
                             passCmd.DrawFullscreenTriangle();
                         });

                // The earlier texture's last reader is this frame: release it inside the frame on
                // even frames and once the frame is submitted on odd ones.
                const bool releaseInFrame = frame % 2 == 0;
                const auto releaseEarlier = [&]
                {
                    if (frame >= EarlierDistance)
                    {
                        FrameTexture& retired = textures[frame - EarlierDistance];
                        bindless.Release(retired.Handle);
                        retired = {};
                    }
                };
                if (releaseInFrame)
                {
                    releaseEarlier();
                }
                context.EndFrame();
                if (!releaseInFrame)
                {
                    releaseEarlier();
                }
            }

            // The readback of the last frame is staged by the next frame and delivered frames
            // later; run empty frames until every one has landed.
            for (u32 drain = 0; drain < framesInFlight + 2; ++drain)
            {
                window->Update();
                context.BeginFrame();
                context.EndFrame();
            }
            context.WaitIdle();
            run.SwapExtentAfter = context.GetSwapChainExtent();

            for (const FrameTexture& texture : textures)
            {
                bindless.Release(texture.Handle);
            }
            bindless.Release(outputHandle);
            bindless.Release(ringHandle);
        }
        return run;
    }

    /// @brief Counts the frames whose every pixel holds the channels the frame was meant to read.
    u32 CountExpectedFrames(const ChurnRun& run)
    {
        u32 matching = 0;
        for (u32 frame = 0; frame < ChurnFrames; ++frame)
        {
            const vector<u8>& bytes = run.Frames[frame];
            const uvec2 extent = run.Extents[frame];
            const usize pixels = static_cast<usize>(extent.x) * extent.y;
            if (bytes.size() != pixels * 4 * sizeof(u16))
            {
                continue;
            }
            const u32 earlier = frame >= EarlierDistance ? frame - EarlierDistance : frame;
            const auto* halves = reinterpret_cast<const u16*>(bytes.data());
            bool expected = true;
            for (usize pixel = 0; pixel < pixels && expected; ++pixel)
            {
                expected = glm::unpackHalf1x16(halves[(pixel * 4) + 0]) == TextureValue(frame) &&
                           glm::unpackHalf1x16(halves[(pixel * 4) + 1]) == TextureValue(earlier) &&
                           glm::unpackHalf1x16(halves[(pixel * 4) + 2]) == RingValue(frame);
            }
            matching += expected ? 1 : 0;
        }
        return matching;
    }
}

TEST_CASE("async submit: a context resolves the mode it asks for unless the environment chose")
{
    const bool environmentChose = std::getenv(QueueSubmitModeEnvironmentVariable) != nullptr;
    for (const QueueSubmitMode mode : {QueueSubmitMode::Synchronous, QueueSubmitMode::Asynchronous})
    {
        Context context;
        context.Initialize(
            {.ApplicationName = "Async Submit Mode", .HeadlessExtent = {4, 4}, .SubmitMode = mode},
            nullptr);
        const ResolvedQueueSubmitMode& resolved = context.GetQueueSubmitMode();
#ifdef __APPLE__
        if (environmentChose)
        {
            CHECK(resolved.Source == QueueSubmitModeSource::Environment);
        }
        else
        {
            CHECK(resolved.Source == QueueSubmitModeSource::Requested);
            CHECK(resolved.Mode == mode);
        }
#else
        (void)environmentChose;
        CHECK(resolved.Source == QueueSubmitModeSource::DriverDefault);
#endif
    }
}

TEST_CASE("async submit: churned frames read back identically in both submission modes")
{
    const ChurnRun asynchronous = RunChurn(QueueSubmitMode::Asynchronous);
    const ChurnRun synchronous = RunChurn(QueueSubmitMode::Synchronous);

    if (asynchronous.Mode.Source == QueueSubmitModeSource::Environment)
    {
        MESSAGE("the submission mode comes from the environment, so both runs share it");
    }

    // Every frame read back what it was meant to read, in both modes.
    CHECK(CountExpectedFrames(asynchronous) == ChurnFrames);
    CHECK(CountExpectedFrames(synchronous) == ChurnFrames);

    u32 identical = 0;
    for (u32 frame = 0; frame < ChurnFrames; ++frame)
    {
        identical += asynchronous.Frames[frame] == synchronous.Frames[frame] ? 1 : 0;
    }
    CHECK(identical == ChurnFrames);

    // The window resize reached the swap chain mid-run.
    CHECK(asynchronous.SwapExtentAfter != asynchronous.SwapExtentBefore);
    CHECK(synchronous.SwapExtentAfter != synchronous.SwapExtentBefore);

#ifdef __APPLE__
    // The minimize took, and the next frame parked until the restore landed (scheduled 0.3 s on).
    for (const ChurnRun* run : {&asynchronous, &synchronous})
    {
        CHECK(run->Minimized);
        CHECK(run->Restored);
        CHECK(run->ParkedSeconds > 0.2);
    }
#endif
}
