// RenderLayer table + mask arithmetic: pure CPU, no Context, no Vulkan. The closed layer table is
// reflected and prefab-persisted, so its integer values are stable and a new layer is appended,
// never inserted — Environment is value 2, Display 3. These pin the values, the count, that the mask
// helpers handle every layer, and that Environment sits in both AllRenderLayers and the shared
// DefaultEnvironmentCaptureLayers (a general reflection keeps the distant backdrop; an IBL probe
// that wants only it names it alone) while ViewAnchored and Display sit in neither capture.

#include <doctest/doctest.h>

#include <Veng/Scene/RenderLayer.h>

using namespace Veng;

TEST_CASE("RenderLayer keeps its persisted integer values and count")
{
    CHECK(static_cast<u32>(RenderLayer::Default) == 0);
    CHECK(static_cast<u32>(RenderLayer::ViewAnchored) == 1);
    CHECK(static_cast<u32>(RenderLayer::Environment) == 2);
    CHECK(static_cast<u32>(RenderLayer::Display) == 3);
    CHECK(RenderLayerCount == 4);
}

TEST_CASE("RenderLayerBit / RenderLayerInMask handle every layer")
{
    CHECK(RenderLayerBit(RenderLayer::Default) == 0b001u);
    CHECK(RenderLayerBit(RenderLayer::ViewAnchored) == 0b010u);
    CHECK(RenderLayerBit(RenderLayer::Environment) == 0b100u);
    CHECK(RenderLayerBit(RenderLayer::Display) == 0b1000u);

    CHECK(AllRenderLayers == 0b1111u);
    CHECK(RenderLayerInMask(AllRenderLayers, RenderLayer::Display));
    CHECK(RenderLayerInMask(AllRenderLayers, RenderLayer::Default));
    CHECK(RenderLayerInMask(AllRenderLayers, RenderLayer::ViewAnchored));
    CHECK(RenderLayerInMask(AllRenderLayers, RenderLayer::Environment));

    // A mask that clears just the Environment bit still names the other two.
    const u32 noEnvironment = AllRenderLayers & ~RenderLayerBit(RenderLayer::Environment);
    CHECK(RenderLayerInMask(noEnvironment, RenderLayer::Default));
    CHECK(RenderLayerInMask(noEnvironment, RenderLayer::ViewAnchored));
    CHECK_FALSE(RenderLayerInMask(noEnvironment, RenderLayer::Environment));
}

TEST_CASE("DefaultEnvironmentCaptureLayers keeps Environment and Default, drops ViewAnchored and "
          "Display")
{
    // The shared default carries only the universal exclusions: ViewAnchored has no world position,
    // and Display is drawn for whoever looks at it rather than being scenery, so both are wrong in
    // every capture.
    CHECK_FALSE(RenderLayerInMask(DefaultEnvironmentCaptureLayers, RenderLayer::ViewAnchored));
    CHECK_FALSE(RenderLayerInMask(DefaultEnvironmentCaptureLayers, RenderLayer::Display));
    // A general specular reflection wants both the nearby geometry and the distant backdrop; a
    // probe that wants only the surroundings names Environment alone instead.
    CHECK(RenderLayerInMask(DefaultEnvironmentCaptureLayers, RenderLayer::Default));
    CHECK(RenderLayerInMask(DefaultEnvironmentCaptureLayers, RenderLayer::Environment));
}
