// The swap-chain composite's source decisions (Renderer/CompositeSource.h), both device-free.
//
// The scene source: the gather is skipped exactly when its output would be a copy of one placement
// (the last one covers the window) or of its clear colour (there are none). Any other placement set
// — split-screen, picture-in-picture, a region short of the window — must still be assembled, which
// is the property a too-eager bypass would break by showing one viewport where several belong. The
// windowed tail itself has no gpu case (tests/gpu/splitscreen.cpp says why), so the decision is
// pinned here.
//
// The overlay source: a layer that drew nothing is replaced by a transparent stand-in.

#include <doctest/doctest.h>

#include "Renderer/CompositeSource.h"

#include <array>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Window{1920, 1080};

    // Only the region is consulted; the decision never touches a texture.
    CompositePlacement At(const ivec2 offset, const uvec2 extent)
    {
        return CompositePlacement{.Texture = nullptr,
                                  .Region = {.Offset = offset, .Extent = extent}};
    }

    const CompositePlacement Covering = At({0, 0}, Window);
    const CompositePlacement LeftHalf = At({0, 0}, {Window.x / 2, Window.y});
    const CompositePlacement RightHalf =
        At({static_cast<i32>(Window.x / 2), 0}, {Window.x / 2, Window.y});
}

TEST_CASE("No placements composite a black stand-in")
{
    CHECK(ResolveCompositeSceneSource({}, Window) == CompositeSceneSource::Black);
}

TEST_CASE("A last placement covering the window is sampled directly")
{
    const std::array alone{Covering};
    CHECK(ResolveCompositeSceneSource(alone, Window) == CompositeSceneSource::Direct);

    // The gather's opaque blend lets the last placement overwrite every earlier one.
    const std::array overPartials{LeftHalf, RightHalf, Covering};
    CHECK(ResolveCompositeSceneSource(overPartials, Window) == CompositeSceneSource::Direct);
}

TEST_CASE("Placements that do not end in a covering one are gathered")
{
    // A covering placement drawn first is overwritten in part by the ones after it.
    const std::array coveredFirst{Covering, LeftHalf};
    CHECK(ResolveCompositeSceneSource(coveredFirst, Window) == CompositeSceneSource::Gather);

    const std::array quadrants{LeftHalf, RightHalf};
    CHECK(ResolveCompositeSceneSource(quadrants, Window) == CompositeSceneSource::Gather);

    // One pixel short on either axis, or shifted by one, leaves an uncovered strip to clear.
    const std::array shortX{At({0, 0}, {Window.x - 1, Window.y})};
    CHECK(ResolveCompositeSceneSource(shortX, Window) == CompositeSceneSource::Gather);
    const std::array shortY{At({0, 0}, {Window.x, Window.y - 1})};
    CHECK(ResolveCompositeSceneSource(shortY, Window) == CompositeSceneSource::Gather);
    const std::array shifted{At({1, 0}, Window)};
    CHECK(ResolveCompositeSceneSource(shifted, Window) == CompositeSceneSource::Gather);

    // A region sized for a previous window extent (an absolute region after a resize).
    const std::array stale{At({0, 0}, {1280, 720})};
    CHECK(ResolveCompositeSceneSource(stale, Window) == CompositeSceneSource::Gather);
}

TEST_CASE("An overlay layer that drew nothing composites a transparent stand-in")
{
    CHECK(ResolveCompositeOverlaySource(true) == CompositeOverlaySource::Layer);
    CHECK(ResolveCompositeOverlaySource(false) == CompositeOverlaySource::Transparent);
}
