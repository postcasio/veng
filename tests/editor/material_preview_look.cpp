// A material preview opens as the project's look says: its field of view, its environment, its
// render block's bloom — and, with no environment to light it, a sun.

#include <doctest/doctest.h>

#include "material/MaterialPreview.h"

using namespace Veng;
using namespace VengEditor;

TEST_CASE("MaterialPreview: with no environment the preview opens sunlit")
{
    const MaterialPreviewState state = MaterialPreview::DefaultState(PreviewLook{});
    CHECK(state.Sun);
    CHECK_FALSE(state.Environment.IsValid());
}

TEST_CASE("MaterialPreview: a project's look sets the opening field of view, environment and bloom")
{
    RenderLook render;
    render.Bloom = false;
    render.BloomIntensity = 0.7f;
    render.BloomRadius = 0.4f;
    const PreviewLook look{
        .Render = render, .FovY = 1.0f, .Environment = AssetId{0x0123456789ABCDEFULL}};

    const MaterialPreviewState state = MaterialPreview::DefaultState(look);
    CHECK(state.FovY == 1.0f);
    CHECK(state.Environment == look.Environment);
    CHECK_FALSE(state.Sun);
    CHECK_FALSE(state.Bloom);
    CHECK(state.BloomStrength == 0.7f);
    CHECK(state.BloomRadius == 0.4f);
}
