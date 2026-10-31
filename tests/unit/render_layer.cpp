// RenderLayer table + mask arithmetic: pure CPU, no Context, no Vulkan. The closed layer table is
// reflected and prefab-persisted, so its integer values are stable and a new layer is appended,
// never inserted — Environment is value 2, Display 3, CameraOnly 4, CaptureOnly 5. These pin the
// values, the count, that the mask helpers handle every layer, that Environment sits in both
// AllRenderLayers and the shared DefaultEnvironmentCaptureLayers (a general reflection keeps the
// distant backdrop; an IBL probe that wants only it names it alone) while ViewAnchored and Display
// sit in neither capture, and that CameraOnly and CaptureOnly split the camera views' default from
// the captures'.

#include <doctest/doctest.h>

#include <Veng/Scene/RenderLayer.h>

using namespace Veng;

TEST_CASE("RenderLayer keeps its persisted integer values and count")
{
    CHECK(static_cast<u32>(RenderLayer::Default) == 0);
    CHECK(static_cast<u32>(RenderLayer::ViewAnchored) == 1);
    CHECK(static_cast<u32>(RenderLayer::Environment) == 2);
    CHECK(static_cast<u32>(RenderLayer::Display) == 3);
    CHECK(static_cast<u32>(RenderLayer::CameraOnly) == 4);
    CHECK(static_cast<u32>(RenderLayer::CaptureOnly) == 5);
    CHECK(RenderLayerCount == 6);
}

TEST_CASE("RenderLayerBit / RenderLayerInMask handle every layer")
{
    CHECK(RenderLayerBit(RenderLayer::Default) == 0b001u);
    CHECK(RenderLayerBit(RenderLayer::ViewAnchored) == 0b010u);
    CHECK(RenderLayerBit(RenderLayer::Environment) == 0b100u);
    CHECK(RenderLayerBit(RenderLayer::Display) == 0b1000u);
    CHECK(RenderLayerBit(RenderLayer::CameraOnly) == 0b10000u);
    CHECK(RenderLayerBit(RenderLayer::CaptureOnly) == 0b100000u);

    CHECK(AllRenderLayers == 0b111111u);
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

TEST_CASE("CameraOnly and CaptureOnly are the only layers the camera and capture defaults split")
{
    // Each kind of view draws its own layer and leaves out the other's.
    CHECK(RenderLayerInMask(DefaultCameraViewLayers, RenderLayer::CameraOnly));
    CHECK_FALSE(RenderLayerInMask(DefaultCameraViewLayers, RenderLayer::CaptureOnly));
    for (const u32 capture : {DefaultSceneCaptureLayers, DefaultEnvironmentCaptureLayers})
    {
        CHECK(RenderLayerInMask(capture, RenderLayer::CaptureOnly));
        CHECK_FALSE(RenderLayerInMask(capture, RenderLayer::CameraOnly));
    }
    // Every other layer is drawn by both defaults, so a scene using neither layer renders and
    // captures as it would without them.
    const u32 split =
        RenderLayerBit(RenderLayer::CameraOnly) | RenderLayerBit(RenderLayer::CaptureOnly);
    CHECK((DefaultCameraViewLayers & ~split) == (AllRenderLayers & ~split));
    CHECK((DefaultSceneCaptureLayers & ~split) == (AllRenderLayers & ~split));
    CHECK((DefaultEnvironmentCaptureLayers & ~split) ==
          (AllRenderLayers & ~split & ~RenderLayerBit(RenderLayer::ViewAnchored) &
           ~RenderLayerBit(RenderLayer::Display)));
}
