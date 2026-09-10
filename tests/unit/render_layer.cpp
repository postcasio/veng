// RenderLayer table + mask arithmetic: pure CPU, no Context, no Vulkan. The closed layer table is
// reflected and prefab-persisted, so its integer values are stable and a new layer is appended,
// never inserted — SelfLit is value 2. These pin the value, the widened count, that the mask
// helpers handle every layer, and — the load-bearing policy of plan-00 — that SelfLit sits in both
// AllRenderLayers and the shared DefaultEnvironmentCaptureLayers (the engine offers the self-lit
// layer but curates its exclusion into no shared default; a capture that must drop it declares so).

#include <doctest/doctest.h>

#include <Veng/Scene/RenderLayer.h>

using namespace Veng;

TEST_CASE("RenderLayer keeps its persisted integer values and count")
{
    CHECK(static_cast<u32>(RenderLayer::Default) == 0);
    CHECK(static_cast<u32>(RenderLayer::ViewAnchored) == 1);
    // Appended, not inserted: SelfLit takes the next value, so persisted prefab integers for the
    // existing layers do not move.
    CHECK(static_cast<u32>(RenderLayer::SelfLit) == 2);
    CHECK(RenderLayerCount == 3);
}

TEST_CASE("RenderLayerBit / RenderLayerInMask handle the widened table")
{
    CHECK(RenderLayerBit(RenderLayer::Default) == 0b001u);
    CHECK(RenderLayerBit(RenderLayer::ViewAnchored) == 0b010u);
    CHECK(RenderLayerBit(RenderLayer::SelfLit) == 0b100u);

    // AllRenderLayers names every layer including the new one.
    CHECK(AllRenderLayers == 0b111u);
    CHECK(RenderLayerInMask(AllRenderLayers, RenderLayer::Default));
    CHECK(RenderLayerInMask(AllRenderLayers, RenderLayer::ViewAnchored));
    CHECK(RenderLayerInMask(AllRenderLayers, RenderLayer::SelfLit));

    // A mask that clears just the SelfLit bit still names the other two.
    const u32 noSelfLit = AllRenderLayers & ~RenderLayerBit(RenderLayer::SelfLit);
    CHECK(RenderLayerInMask(noSelfLit, RenderLayer::Default));
    CHECK(RenderLayerInMask(noSelfLit, RenderLayer::ViewAnchored));
    CHECK_FALSE(RenderLayerInMask(noSelfLit, RenderLayer::SelfLit));
}

TEST_CASE("DefaultEnvironmentCaptureLayers keeps SelfLit and drops only ViewAnchored")
{
    // The shared default carries only the universal exclusion (ViewAnchored has no world position,
    // so it is wrong in every environment capture).
    CHECK_FALSE(RenderLayerInMask(DefaultEnvironmentCaptureLayers, RenderLayer::ViewAnchored));
    // SelfLit stays in the shared default: excluding it is a per-capture decision the capture that
    // needs it declares, never one baked into the mask others inherit.
    CHECK(RenderLayerInMask(DefaultEnvironmentCaptureLayers, RenderLayer::SelfLit));
    CHECK(RenderLayerInMask(DefaultEnvironmentCaptureLayers, RenderLayer::Default));
}
