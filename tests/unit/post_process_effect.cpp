// Post-process effect resolve unit cases. ResolveActivePostProcessEffects decides which of a
// scene's PostProcessEffect components run and in what order — the device-free core the renderer
// gathers each Execute, mirroring FrameTopology's pure-data resolve. The filtering (enabled +
// loaded), the Order sort, and its stability are pinned here rather than only through a rendered
// image. Pure data → data; no Context, no driver, no scene.

#include <doctest/doctest.h>

#include <array>
#include <vector>

#include "Renderer/PostProcessEffectResolver.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A loaded, enabled effect at the given order and material id — the common case a test then
    // perturbs one field of.
    PostProcessEffectInput Effect(const i32 order, const u64 materialId)
    {
        return {.Order = order, .Enabled = true, .MaterialLoaded = true, .MaterialId = materialId};
    }

    // The run's material ids, in run order — what a downstream ping-pong chain would drive.
    std::vector<u64> RunIds(std::span<const PostProcessEffectEntry> run)
    {
        std::vector<u64> ids;
        for (const PostProcessEffectEntry& e : run)
        {
            ids.push_back(e.MaterialId);
        }
        return ids;
    }
}

TEST_CASE("post-process resolve: no components resolves to an empty, inert run")
{
    const std::vector<PostProcessEffectInput> inputs;
    CHECK(ResolveActivePostProcessEffects(inputs).empty());
}

TEST_CASE("post-process resolve: a disabled effect is dropped")
{
    std::array inputs{Effect(0, 10), Effect(0, 20)};
    inputs[0].Enabled = false;

    const std::vector<PostProcessEffectEntry> run = ResolveActivePostProcessEffects(inputs);
    REQUIRE(run.size() == 1);
    CHECK(run[0].MaterialId == 20);
}

TEST_CASE("post-process resolve: an effect whose material is not resident is dropped")
{
    std::array inputs{Effect(0, 10), Effect(0, 20)};
    inputs[1].MaterialLoaded = false;

    const std::vector<PostProcessEffectEntry> run = ResolveActivePostProcessEffects(inputs);
    REQUIRE(run.size() == 1);
    CHECK(run[0].MaterialId == 10);
}

TEST_CASE("post-process resolve: effects run in ascending Order")
{
    // Authored out of order; the run is by Order.
    const std::array inputs{Effect(30, 3), Effect(-5, 1), Effect(10, 2)};

    const std::vector<PostProcessEffectEntry> run = ResolveActivePostProcessEffects(inputs);
    CHECK(RunIds(run) == std::vector<u64>{1, 2, 3});
}

TEST_CASE("post-process resolve: ties on Order keep scene iteration order (stable)")
{
    // Every effect shares one Order, so the run is the input order unchanged — a total order that
    // does not move between frames, so two equal effects cannot trade places.
    const std::array inputs{Effect(0, 100), Effect(0, 200), Effect(0, 300)};

    const std::vector<PostProcessEffectEntry> run = ResolveActivePostProcessEffects(inputs);
    CHECK(RunIds(run) == std::vector<u64>{100, 200, 300});
}

TEST_CASE("post-process resolve: SourceIndex maps a run entry back to its component")
{
    // The renderer reads each entry's material through SourceIndex into the gather; the reorder must
    // carry the original index, not the run position.
    const std::array inputs{Effect(20, 1), Effect(10, 2)};

    const std::vector<PostProcessEffectEntry> run = ResolveActivePostProcessEffects(inputs);
    REQUIRE(run.size() == 2);
    CHECK(run[0].SourceIndex == 1); // Order 10, the second input
    CHECK(run[1].SourceIndex == 0); // Order 20, the first input
}

TEST_CASE("post-process resolve: the (id, order) signature detects set changes, not param edits")
{
    // The renderer recompiles when the (material id, order) signature moves and replays otherwise.
    // Build the signature the way the renderer does and check each edit class.
    const auto signature = [](std::span<const PostProcessEffectInput> in)
    {
        const std::vector<PostProcessEffectEntry> run = ResolveActivePostProcessEffects(in);
        std::vector<std::pair<u64, i32>> sig;
        for (const PostProcessEffectEntry& e : run)
        {
            sig.emplace_back(e.MaterialId, e.Order);
        }
        return sig;
    };

    const std::array base{Effect(0, 1), Effect(10, 2)};
    const std::vector<std::pair<u64, i32>> baseSig = signature(base);

    // Identical set replays (no recompile).
    CHECK(signature(std::array{Effect(0, 1), Effect(10, 2)}) == baseSig);

    // Adding an effect changes the signature.
    CHECK(signature(std::array{Effect(0, 1), Effect(10, 2), Effect(20, 3)}) != baseSig);

    // Removing one changes it.
    CHECK(signature(std::array{Effect(0, 1)}) != baseSig);

    // Reordering (an Order change that swaps the run) changes it.
    CHECK(signature(std::array{Effect(20, 1), Effect(10, 2)}) != baseSig);

    // Toggling one off changes it.
    std::array toggled{Effect(0, 1), Effect(10, 2)};
    toggled[1].Enabled = false;
    CHECK(signature(toggled) != baseSig);
}
