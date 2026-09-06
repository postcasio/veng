// GeneratedTextureService and AsyncReadback against a live device.
//
// The properties the service exists to guarantee:
//   1. Amortization is invisible to the result. The same many-tick job run under a budget of 1, 2
//      and unlimited ticks per frame produces byte-identical texels — only how many frames it took
//      differs.
//   2. A cancelled job releases its targets and never fires its completion; its bindless slot comes
//      back once the deferred release window has cycled.
//   3. A raster-tick job works: six clear-by-face ticks over a 6-layer target complete, and each
//      layer holds the colour its tick wrote.
//   4. The frame-deferred readback returns exactly the texels a synchronous Image::Download of the
//      same image returns, and not before GetMaxFramesInFlight() frames have passed. There is no
//      wait path to assert the render thread stays out of — the primitive has none to call.
//   5. The persistent cache is transparent end to end: a keyed job's result is stored once its
//      ticks have run, a later job under the same cache key completes from disk with none of its
//      ticks running and the identical texels, and with the cache's files gone the same request
//      simply runs its ticks again.
//   6. A request allocates nothing on the calling thread: the targets are created on a worker, the
//      set-0 slot is minted when they land, and the allocation hold and the cache-probe hold
//      compose so the job is never selectable at the seam between them.
//
// The ticks here are raster clears rather than compute dispatches deliberately: a clear over a
// render area needs no pipeline, no shader and no descriptor set, so what the cases measure is the
// service's scheduling and barriers rather than a shader's correctness.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <array>
#include <atomic>
#include <filesystem>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Path.h>
#include <Veng/Persistence/DerivedDataCache.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>
#include <support/TempPath.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 StripeEdge = 8;
    constexpr u32 StripeTicks = StripeEdge;

    // The clear colour tick i writes, chosen so its unorm8 encoding is exact (i * 32 / 255 round
    // trips to i * 32) and so no two ticks share a value.
    ClearColor StripeColor(const u32 tick)
    {
        return {.R = static_cast<f32>(tick * 32) / 255.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f};
    }

    // Clears row `TickIndex` of the target to that tick's colour.
    void StripeTick(CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        cmd.BeginRendering({
            .Offset = {0, static_cast<i32>(context.TickIndex)},
            .Extent = {StripeEdge, 1},
            .ColorAttachments = {{
                .ImageView = context.Targets[0]->GetView(0),
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = StripeColor(context.TickIndex),
            }},
        });
        cmd.EndRendering();
    }

    // A job whose tick i clears row i of an 8x8 target: many ticks, each a bounded slice, with a
    // result that says exactly which ticks ran and in what order.
    GeneratedTextureRequest StripeJob(const GeneratedTextureKey key, const string& name)
    {
        return {
            .Key = key,
            .Name = name,
            .Targets = {{
                .Image = {.Extent = {StripeEdge, StripeEdge, 1},
                          .Format = Format::RGBA8Unorm,
                          .Usage = ImageUsage::TransferSrc},
                .ProducerAccess = AccessKind::ColorAttachment,
            }},
            .TickCount = StripeTicks,
            .OnTick = StripeTick,
        };
    }

    constexpr u32 ChainEdge = 8;
    constexpr u32 ChainMips = 4;

    // Clears mip level `TickIndex` of the target to that level's own colour, so the stored chain
    // says which level any restored texel came from.
    void ChainTick(CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        const u32 edge = std::max(1u, ChainEdge >> context.TickIndex);
        cmd.BeginRendering({
            .Extent = {edge, edge},
            .ColorAttachments = {{
                .ImageView = context.Targets[0]->GetView(context.TickIndex),
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = StripeColor(context.TickIndex),
            }},
        });
        cmd.EndRendering();
    }

    // A job filling every level of a four-level chain, each level a distinct flat colour.
    GeneratedTextureRequest ChainJob(const GeneratedTextureKey key)
    {
        return {
            .Key = key,
            .Name = "ChainBake",
            .Targets = {{
                .Image = {.Extent = {ChainEdge, ChainEdge, 1},
                          .MipLevels = ChainMips,
                          .Format = Format::RGBA8Unorm,
                          .Usage = ImageUsage::TransferSrc},
                .ProducerAccess = AccessKind::ColorAttachment,
            }},
            .TickCount = ChainMips,
            .OnTick = ChainTick,
        };
    }

    // Drives frames until `done` reports true, or gives up after a generous bound so a stalled
    // service fails the case instead of hanging it. Returns the frames driven.
    u32 DriveUntil(Renderer::Context& context, const function<bool()>& done, const u32 limit = 64)
    {
        u32 frames = 0;
        while (!done() && frames < limit)
        {
            context.BeginFrame();
            context.EndFrame();
            frames++;
        }
        return frames;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "generated texture: the tick budget changes the frame count, not the texels")
{
    GeneratedTextureService& service = Context.GetGeneratedTextures();

    std::vector<std::vector<u8>> results;
    std::vector<u32> frameCounts;

    for (const u32 budget :
         {1u, 2u, static_cast<u32>(GeneratedTextureService::UnlimitedCostBudget)})
    {
        service.SetCostBudget(budget);

        const GeneratedTextureKey key = 0x5721'0000ull + budget;
        u32 completions = 0;
        GeneratedTextureRequest request = StripeJob(key, "StripeBake");
        request.OnComplete = [&completions](const GeneratedTextureResult&) { completions++; };
        REQUIRE(service.Request(std::move(request)));

        // The same key again is dropped rather than restarting or duplicating the job.
        CHECK_FALSE(service.Request(StripeJob(key, "StripeBake")));
        CHECK(service.IsPending(key));

        const u32 frames = DriveUntil(Context, [&] { return service.IsResident(key); });
        CHECK(service.IsResident(key));
        CHECK(completions == 1u);
        frameCounts.push_back(frames);

        const optional<GeneratedTextureResult> result = service.Find(key);
        REQUIRE(result.has_value());
        REQUIRE(result->Targets.size() == 1u);

        Context.WaitIdle();
        results.push_back(result->Targets[0]->GetImage()->Download());

        CHECK(service.Release(key));
        CHECK_FALSE(service.IsResident(key));
    }

    // Every tick ran, so every row carries its own tick's colour.
    REQUIRE(results.size() == 3u);
    REQUIRE(results[0].size() == StripeEdge * StripeEdge * 4u);
    for (u32 row = 0; row < StripeEdge; row++)
    {
        const u8* texel = results[0].data() + (row * StripeEdge * 4u);
        CHECK(texel[0] == static_cast<u8>(row * 32));
        CHECK(texel[1] == 0u);
        CHECK(texel[2] == 0u);
        CHECK(texel[3] == 255u);
    }

    // And the budget moved only how long it took.
    CHECK(results[1] == results[0]);
    CHECK(results[2] == results[0]);
    CHECK(frameCounts[0] == StripeTicks);
    CHECK(frameCounts[1] == (StripeTicks + 1) / 2);
    CHECK(frameCounts[2] == 1u);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "generated texture: a cancelled job releases its targets and never completes")
{
    GeneratedTextureService& service = Context.GetGeneratedTextures();
    const BindlessRegistry& bindless = Context.GetBindlessRegistry();
    service.SetCostBudget(1);

    const u32 freeBefore = bindless.GetFreeSlots().Textures;

    constexpr GeneratedTextureKey Key = 0x5721'C0DEull;
    u32 completions = 0;
    GeneratedTextureRequest request = StripeJob(Key, "CancelledBake");
    request.Targets[0].Bindless = true;
    request.OnComplete = [&completions](const GeneratedTextureResult&) { completions++; };
    REQUIRE(service.Request(std::move(request)));

    const optional<GeneratedTextureResult> pending = service.Find(Key);
    CHECK_FALSE(pending.has_value()); // Not resident, so Find reports nothing yet.
    CHECK(bindless.GetFreeSlots().Textures == freeBefore - 1);

    // Two of the eight ticks run, then the job is torn down mid-flight.
    Context.BeginFrame();
    Context.EndFrame();
    Context.BeginFrame();
    Context.EndFrame();
    CHECK(service.IsPending(Key));
    CHECK(service.GetStats().Running == 1u);

    CHECK(service.Cancel(Key));
    CHECK_FALSE(service.Cancel(Key));
    CHECK_FALSE(service.IsPending(Key));
    CHECK_FALSE(service.IsResident(Key));
    CHECK(service.GetStats().CancelledTotal == 1u);
    CHECK(service.GetStats().CompletedTotal == 0u);

    // The remaining ticks never run and no completion ever fires.
    DriveUntil(Context, [] { return false; }, 8);
    CHECK(completions == 0u);

    // The bindless slot comes back once its deferred-release window has cycled.
    DriveUntil(Context, [] { return false; }, Context.GetMaxFramesInFlight() + 1);
    CHECK(bindless.GetFreeSlots().Textures == freeBefore);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "generated texture: a raster-tick job fills a six-layer target face by face")
{
    GeneratedTextureService& service = Context.GetGeneratedTextures();
    AsyncReadback& readback = Context.GetAsyncReadback();
    service.SetCostBudget(2);

    constexpr u32 Faces = 6;
    constexpr u32 FaceEdge = 4;
    constexpr GeneratedTextureKey Key = 0x5721'FACEull;

    REQUIRE(service.Request({
        .Key = Key,
        .Name = "FaceBake",
        .Targets = {{
            .Image = {.Extent = {FaceEdge, FaceEdge, 1},
                      .Layers = Faces,
                      .Format = Format::RGBA8Unorm,
                      .Usage = ImageUsage::TransferSrc},
            .ProducerAccess = AccessKind::ColorAttachment,
            .SampledViewType = ImageViewType::Cube,
        }},
        .TickCount = Faces,
        .OnTick =
            [](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
        {
            cmd.BeginRendering({
                .Extent = {FaceEdge, FaceEdge},
                .ColorAttachments = {{
                    .ImageView = context.Targets[0]->GetView(0, context.TickIndex),
                    .LoadOp = LoadOp::Clear,
                    .StoreOp = StoreOp::Store,
                    .ClearValue = StripeColor(context.TickIndex + 1),
                }},
            });
            cmd.EndRendering();
        },
    }));

    DriveUntil(Context, [&] { return service.IsResident(Key); });
    REQUIRE(service.IsResident(Key));

    const optional<GeneratedTextureResult> result = service.Find(Key);
    REQUIRE(result.has_value());
    const Ref<Image>& image = result->Targets[0]->GetImage();
    CHECK(image->GetLayers() == Faces);

    // Each face is read back through the frame-deferred primitive — the only way to see a layer
    // other than zero, since Image::Download reads mip 0 layer 0 alone.
    std::array<std::vector<u8>, Faces> faces;
    u32 delivered = 0;
    for (u32 face = 0; face < Faces; face++)
    {
        const AsyncReadbackHandle handle = readback.Request({
            .Name = "FaceRead",
            .Image = image,
            .ArrayLayer = face,
            .OnComplete =
                [&faces, &delivered, face](const std::span<const u8> bytes)
            {
                faces[face].assign(bytes.begin(), bytes.end());
                delivered++;
            },
        });
        CHECK(handle.IsValid());
    }
    CHECK(readback.GetPendingCount() == Faces);

    DriveUntil(Context, [&] { return delivered == Faces; });
    REQUIRE(delivered == Faces);
    CHECK(readback.GetPendingCount() == 0u);

    for (u32 face = 0; face < Faces; face++)
    {
        REQUIRE(faces[face].size() == FaceEdge * FaceEdge * 4u);
        for (u32 texel = 0; texel < FaceEdge * FaceEdge; texel++)
        {
            const u8* pixel = faces[face].data() + (texel * 4u);
            CHECK(pixel[0] == static_cast<u8>((face + 1) * 32));
            CHECK(pixel[1] == 0u);
            CHECK(pixel[2] == 0u);
            CHECK(pixel[3] == 255u);
        }
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "async readback: the same bytes as Image::Download, frames later")
{
    AsyncReadback& readback = Context.GetAsyncReadback();

    constexpr u32 Edge = 8;
    const Ref<Image> image =
        Image::Create(Context, {.Name = "ReadbackSource",
                                .Extent = {Edge, Edge, 1},
                                .Format = Format::RGBA8Unorm,
                                .Usage = ImageUsage::Sampled | ImageUsage::TransferSrc |
                                         ImageUsage::TransferDst});

    std::vector<u8> source(Edge * Edge * 4u);
    for (usize i = 0; i < source.size(); i++)
    {
        source[i] = static_cast<u8>(i * 7);
    }
    image->UploadSync(source);

    Context.WaitIdle();
    const std::vector<u8> synchronous = image->Download();
    CHECK(synchronous == source);

    std::vector<u8> asynchronous;
    bool delivered = false;
    const AsyncReadbackHandle handle = readback.Request({
        .Name = "ReadbackTest",
        .Image = image,
        .OnComplete =
            [&](const std::span<const u8> bytes)
        {
            asynchronous.assign(bytes.begin(), bytes.end());
            delivered = true;
        },
    });
    REQUIRE(handle.IsValid());

    const u32 frames = DriveUntil(Context, [&] { return delivered; });
    CHECK(delivered);
    CHECK(asynchronous == synchronous);
    // The copy rides a frame's command buffer, so the bytes cannot be read before that frame's
    // fence has been waited again — frames-in-flight later, never sooner.
    CHECK(frames >= Context.GetMaxFramesInFlight());
    CHECK(readback.GetCompletedCount() == 1u);

    // A cancelled readback is dropped and never delivered.
    u32 secondDeliveries = 0;
    const AsyncReadbackHandle cancelled = readback.Request({
        .Name = "CancelledRead",
        .Image = image,
        .OnComplete = [&secondDeliveries](std::span<const u8>) { secondDeliveries++; },
    });
    REQUIRE(cancelled.IsValid());
    CHECK(readback.Cancel(cancelled));
    CHECK_FALSE(readback.Cancel(cancelled));
    DriveUntil(Context, [] { return false; }, Context.GetMaxFramesInFlight() + 2);
    CHECK(secondDeliveries == 0u);
    CHECK(readback.GetCompletedCount() == 1u);
}

// --- the persistent cache ------------------------------------------------------------------------

namespace
{
    // A fresh, unique cache directory per case under the process's scratch tree, removed on
    // destruction.
    struct TempCacheRoot
    {
        path Dir;

        TempCacheRoot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("gtc-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempCacheRoot() { std::filesystem::remove_all(Dir); }
    };

    // The frame loop as an application runs it: continuations first, then the frame whose pump
    // reacts to them. The cache's probes and stores land through that queue, so a driver that does
    // not pump never sees a hit.
    u32 DriveWithTasks(Renderer::Context& context, TaskSystem& tasks, const function<bool()>& done,
                       const u32 limit = 64)
    {
        u32 frames = 0;
        while (!done() && frames < limit)
        {
            tasks.PumpMainThread();
            context.BeginFrame();
            context.EndFrame();
            frames++;
        }
        tasks.PumpMainThread();
        return frames;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "generated texture: a cached job is answered from disk without running a tick")
{
    const TempCacheRoot root;
    const Result<Unique<DerivedDataCache>> cache =
        DerivedDataCache::Open({.Root = root.Dir, .Generation = "gpu-case"});
    REQUIRE(cache.has_value());

    GeneratedTextureService& service = Context.GetGeneratedTextures();
    service.SetCache(cache->get(), &Tasks);
    service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);

    constexpr string_view CacheKey = "stripe/8x8";
    constexpr GeneratedTextureKey ColdKey = 0x5721'CA01ull;
    constexpr GeneratedTextureKey WarmKey = 0x5721'CA02ull;
    constexpr GeneratedTextureKey EvictedKey = 0x5721'CA03ull;

    // The cold run: nothing to answer from, so the job's ticks fill the target.
    u32 coldTicks = 0;
    GeneratedTextureRequest cold = StripeJob(ColdKey, "CachedBake");
    cold.CacheKey = string(CacheKey);
    cold.OnTick = [&coldTicks](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        coldTicks++;
        StripeTick(cmd, context);
    };
    REQUIRE(service.Request(std::move(cold)));

    DriveWithTasks(Context, Tasks, [&] { return service.IsResident(ColdKey); });
    REQUIRE(service.IsResident(ColdKey));
    CHECK(coldTicks == StripeTicks);
    CHECK(service.GetStats().RestoredTotal == 0u);

    Context.WaitIdle();
    const std::vector<u8> baked = service.Find(ColdKey)->Targets[0]->GetImage()->Download();

    // The store rides the readback pump, so it lands some frames after the completion.
    DriveWithTasks(Context, Tasks, [&] { return (*cache)->Contains(string(CacheKey)); });
    REQUIRE((*cache)->Contains(string(CacheKey)));
    CHECK(service.GetStats().StoredTotal == 1u);
    CHECK(service.Release(ColdKey));

    // The warm run: a different job key, the same cache key. Its tick would fill the target with a
    // different pattern, so texels equal to the cold run's can only have come off the disk.
    u32 warmTicks = 0;
    GeneratedTextureRequest warm = StripeJob(WarmKey, "CachedBake");
    warm.CacheKey = string(CacheKey);
    warm.OnTick = [&warmTicks](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        warmTicks++;
        cmd.BeginRendering({
            .Extent = {StripeEdge, StripeEdge},
            .ColorAttachments = {{
                .ImageView = context.Targets[0]->GetView(0),
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 1.0f, .G = 1.0f, .B = 1.0f, .A = 1.0f},
            }},
        });
        cmd.EndRendering();
    };
    REQUIRE(service.Request(std::move(warm)));

    DriveWithTasks(Context, Tasks, [&] { return service.IsResident(WarmKey); });
    REQUIRE(service.IsResident(WarmKey));
    CHECK(warmTicks == 0u);
    CHECK(service.GetStats().RestoredTotal == 1u);
    CHECK(service.GetStats().TicksLastPump == 0u);

    Context.WaitIdle();
    CHECK(service.Find(WarmKey)->Targets[0]->GetImage()->Download() == baked);
    CHECK(service.Release(WarmKey));

    // And the cache is never a source of truth: delete every file under it and the same request
    // simply runs its ticks again.
    std::filesystem::remove_all(root.Dir);
    std::filesystem::create_directories(root.Dir);

    u32 evictedTicks = 0;
    GeneratedTextureRequest evicted = StripeJob(EvictedKey, "CachedBake");
    evicted.CacheKey = string(CacheKey);
    evicted.OnTick = [&evictedTicks](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        evictedTicks++;
        StripeTick(cmd, context);
    };
    REQUIRE(service.Request(std::move(evicted)));

    DriveWithTasks(Context, Tasks, [&] { return service.IsResident(EvictedKey); });
    REQUIRE(service.IsResident(EvictedKey));
    CHECK(evictedTicks == StripeTicks);
    CHECK(service.GetStats().RestoredTotal == 1u);

    Context.WaitIdle();
    CHECK(service.Find(EvictedKey)->Targets[0]->GetImage()->Download() == baked);

    // Detach before the fixture tears the cache down beneath the service.
    service.SetCache(nullptr, nullptr);
    CHECK(service.Release(EvictedKey));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "generated texture: a target declaring a mip offset restores the chain's tail")
{
    const TempCacheRoot root;
    const Result<Unique<DerivedDataCache>> cache =
        DerivedDataCache::Open({.Root = root.Dir, .Generation = "gpu-case"});
    REQUIRE(cache.has_value());

    GeneratedTextureService& service = Context.GetGeneratedTextures();
    service.SetCache(cache->get(), &Tasks);
    service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);

    constexpr string_view CacheKey = "chain/8x8";
    constexpr GeneratedTextureKey WholeKey = 0x5721'CB01ull;
    constexpr GeneratedTextureKey TailKey = 0x5721'CB02ull;
    constexpr GeneratedTextureKey WrongKey = 0x5721'CB03ull;
    constexpr u32 TailOffset = 2;

    GeneratedTextureRequest whole = ChainJob(WholeKey);
    whole.CacheKey = string(CacheKey);
    REQUIRE(service.Request(std::move(whole)));
    DriveWithTasks(Context, Tasks, [&] { return service.IsResident(WholeKey); });
    REQUIRE(service.IsResident(WholeKey));
    DriveWithTasks(Context, Tasks, [&] { return (*cache)->Contains(string(CacheKey)); });
    REQUIRE((*cache)->Contains(string(CacheKey)));
    CHECK(service.Release(WholeKey));

    // A target two levels down the stored chain, under the same cache key. Its tick would clear it
    // white, so texels carrying level 2's own colour can only have come off the disk — and from
    // that level rather than any other, which is the whole of what the offset claims.
    const u32 tailEdge = ChainEdge >> TailOffset;
    u32 tailTicks = 0;
    const GeneratedTextureRequest tail{
        .Key = TailKey,
        .Name = "ChainTail",
        .Targets = {{
            .Image = {.Extent = {tailEdge, tailEdge, 1},
                      .MipLevels = ChainMips - TailOffset,
                      .Format = Format::RGBA8Unorm,
                      .Usage = ImageUsage::TransferSrc},
            .ProducerAccess = AccessKind::ColorAttachment,
            .CacheMipOffset = TailOffset,
        }},
        .CacheKey = string(CacheKey),
        .TickCount = 1,
        .OnTick =
            [&tailTicks, tailEdge](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
        {
            tailTicks++;
            cmd.BeginRendering({
                .Extent = {tailEdge, tailEdge},
                .ColorAttachments = {{
                    .ImageView = context.Targets[0]->GetView(0),
                    .LoadOp = LoadOp::Clear,
                    .StoreOp = StoreOp::Store,
                    .ClearValue = ClearColor{.R = 1.0f, .G = 1.0f, .B = 1.0f, .A = 1.0f},
                }},
            });
            cmd.EndRendering();
        },
    };
    const u64 storedBefore = service.GetStats().StoredTotal;
    REQUIRE(service.Request(tail));

    DriveWithTasks(Context, Tasks, [&] { return service.IsResident(TailKey); });
    REQUIRE(service.IsResident(TailKey));
    CHECK(tailTicks == 0u);

    Context.WaitIdle();
    const std::vector<u8> restored = service.Find(TailKey)->Targets[0]->GetImage()->Download();
    REQUIRE(restored.size() == static_cast<usize>(tailEdge) * tailEdge * 4);
    const auto expectedRed = static_cast<u8>(TailOffset * 32);
    usize wrongTexels = 0;
    for (usize texel = 0; texel < restored.size(); texel += 4)
    {
        wrongTexels += restored[texel] == expectedRed && restored[texel + 3] == 0xFF ? 0u : 1u;
    }
    CHECK(wrongTexels == 0u);

    // A tail holds less than the entry it read, so it must never write one back.
    DriveWithTasks(Context, Tasks, [&] { return false; }, 4);
    CHECK(service.GetStats().StoredTotal == storedBefore);
    CHECK(service.Release(TailKey));

    // And the offset is checked rather than assumed: the same target declaring the level above it
    // does not answer to a 2x2 shape, so the entry is a miss and the ticks run.
    u32 wrongTicks = 0;
    GeneratedTextureRequest wrong = tail;
    wrong.Key = WrongKey;
    wrong.Targets[0].CacheMipOffset = TailOffset - 1;
    wrong.OnTick =
        [&wrongTicks, tailEdge](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        wrongTicks++;
        cmd.BeginRendering({
            .Extent = {tailEdge, tailEdge},
            .ColorAttachments = {{
                .ImageView = context.Targets[0]->GetView(0),
                .LoadOp = LoadOp::Clear,
                .StoreOp = StoreOp::Store,
                .ClearValue = ClearColor{.R = 1.0f, .G = 1.0f, .B = 1.0f, .A = 1.0f},
            }},
        });
        cmd.EndRendering();
    };
    REQUIRE(service.Request(std::move(wrong)));
    DriveWithTasks(Context, Tasks, [&] { return service.IsResident(WrongKey); });
    REQUIRE(service.IsResident(WrongKey));
    CHECK(wrongTicks == 1u);

    service.SetCache(nullptr, nullptr);
    CHECK(service.Release(WrongKey));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "generated texture: targets are created off the frame thread, and the holds "
                  "compose")
{
    const TempCacheRoot root;
    const Result<Unique<DerivedDataCache>> cache =
        DerivedDataCache::Open({.Root = root.Dir, .Generation = "alloc-case"});
    REQUIRE(cache.has_value());

    GeneratedTextureService& service = Context.GetGeneratedTextures();
    const BindlessRegistry& bindless = Context.GetBindlessRegistry();
    service.SetCache(cache->get(), &Tasks);
    service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);

    constexpr GeneratedTextureKey BakedKey = 0x5721'A110ull;
    constexpr GeneratedTextureKey CancelledKey = 0x5721'A111ull;
    const u32 freeBefore = bindless.GetFreeSlots().Textures;

    constexpr string_view CacheKey = "alloc/8x8";

    u32 ticks = 0;
    GeneratedTextureRequest request = StripeJob(BakedKey, "AllocBake");
    request.CacheKey = string(CacheKey);
    request.Targets[0].Bindless = true;
    request.OnTick = [&ticks](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        ticks++;
        StripeTick(cmd, context);
    };
    REQUIRE(service.Request(std::move(request)));

    // Nothing was allocated on the requesting thread: no image, so no set-0 slot to mint yet.
    CHECK(service.GetStats().Allocating == 1u);
    CHECK(service.GetStats().Probing == 0u);
    CHECK(bindless.GetFreeSlots().Textures == freeBefore);

    // And a frame that runs before the targets land spends no tick on the held job.
    Context.BeginFrame();
    Context.EndFrame();
    CHECK(ticks == 0u);
    CHECK(service.GetStats().TicksLastPump == 0u);

    // The targets land on the main thread, which is where the slot is minted — and the job goes
    // straight on to its cache probe rather than becoming selectable at the seam.
    Tasks.WaitForAll();
    Tasks.PumpMainThread();
    CHECK(service.GetStats().Allocating == 0u);
    CHECK(service.GetStats().Probing == 1u);
    CHECK(bindless.GetFreeSlots().Textures == freeBefore - 1);

    DriveWithTasks(Context, Tasks, [&] { return service.IsResident(BakedKey); });
    CHECK(service.IsResident(BakedKey));
    CHECK(ticks == StripeTicks);

    // Let the store finish before the case ends: the cache is destroyed with this scope, and a
    // worker still writing into it would outlive it.
    DriveWithTasks(Context, Tasks, [&] { return (*cache)->Contains(string(CacheKey)); });
    CHECK((*cache)->Contains(string(CacheKey)));

    // A job cancelled while its targets are still being created tears down cleanly: the worker's
    // targets are dropped when they arrive, no tick ever runs, and no completion fires.
    u32 cancelledTicks = 0;
    u32 cancelledCompletions = 0;
    GeneratedTextureRequest doomed = StripeJob(CancelledKey, "CancelledAlloc");
    doomed.OnTick =
        [&cancelledTicks](CommandBuffer& cmd, const GeneratedTextureTickContext& context)
    {
        cancelledTicks++;
        StripeTick(cmd, context);
    };
    doomed.OnComplete = [&cancelledCompletions](const GeneratedTextureResult&)
    { cancelledCompletions++; };
    REQUIRE(service.Request(std::move(doomed)));
    CHECK(service.GetStats().Allocating == 1u);

    CHECK(service.Cancel(CancelledKey));
    CHECK_FALSE(service.IsPending(CancelledKey));
    Tasks.WaitForAll();
    DriveWithTasks(Context, Tasks, [] { return false; }, 3);
    CHECK(cancelledTicks == 0u);
    CHECK(cancelledCompletions == 0u);
    CHECK(service.GetStats().CancelledTotal == 1u);

    service.SetCache(nullptr, nullptr);
    Tasks.WaitForAll();
    CHECK(service.Release(BakedKey));
}
