// A GUI draw run's clip → attachment scissor. A projected draw list's clip is the bounding box of
// its four projected corners, so a world-anchored surface seen at an angle produces a clip that
// legitimately reaches past the target's top or left edge; the scissor it becomes must stay inside
// the attachment while still covering the part of the clip that is on it. The near miss these pin
// is not the negative offset — a validator catches that — but the silent one beside it: clamping
// the corner to zero without cropping the far edge slides the clipped region down and right, which
// mis-clips scrolled content and looks like a layout bug. Pure math; no device.

#include <doctest/doctest.h>

#include "Renderer/GuiScissor.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Target{800, 600};

    // Whether the scissor covers every pixel the scaled clip touches inside the target: the corner
    // no later than the clip's, the far edge no earlier, on both axes.
    bool CoversVisiblePart(const GuiScissor& scissor, const Gui::Rect& clip, const f32 uiScale)
    {
        const vec2 clipMin = glm::max(clip.Min * uiScale, vec2(0.0f));
        const vec2 clipMax = glm::min(clip.Max() * uiScale, vec2(Target));
        const vec2 scissorMin = vec2(scissor.Offset);
        const vec2 scissorMax = scissorMin + vec2(scissor.Extent);
        return scissorMin.x <= clipMin.x && scissorMin.y <= clipMin.y &&
               scissorMax.x >= clipMax.x && scissorMax.y >= clipMax.y;
    }
}

TEST_CASE("gui scissor: an on-target clip resolves to the box it covers")
{
    const Gui::Rect clip{.Min = {10.0f, 20.0f}, .Size = {100.0f, 50.0f}};
    const optional<GuiScissor> scissor = ResolveGuiScissor(clip, 2.0f, Target);
    REQUIRE(scissor.has_value());
    CHECK(scissor->Offset == ivec2(20, 40));
    CHECK(scissor->Extent == uvec2(200, 100));
    CHECK(CoversVisiblePart(*scissor, clip, 2.0f));
}

TEST_CASE("gui scissor: a clip past the top-left stays on the attachment and keeps its far edge")
{
    // The projected clip starts 162 pixels above the surface and 40 left of it — the case a raw
    // cast emits as a negative offset. The scissor crops to the visible part: it starts at the
    // corner and still ends where the clip does, rather than sliding the region down and right.
    const Gui::Rect clip{.Min = {-40.0f, -162.0f}, .Size = {300.0f, 400.0f}};
    const optional<GuiScissor> scissor = ResolveGuiScissor(clip, 1.0f, Target);
    REQUIRE(scissor.has_value());
    CHECK(scissor->Offset == ivec2(0, 0));
    CHECK(scissor->Extent == uvec2(260, 238));
    CHECK(CoversVisiblePart(*scissor, clip, 1.0f));
}

TEST_CASE("gui scissor: a clip past the far edges is cropped to the attachment")
{
    const Gui::Rect clip{.Min = {700.0f, 500.0f}, .Size = {400.0f, 400.0f}};
    const optional<GuiScissor> scissor = ResolveGuiScissor(clip, 1.0f, Target);
    REQUIRE(scissor.has_value());
    CHECK(scissor->Offset == ivec2(700, 500));
    CHECK(scissor->Offset.x + static_cast<i32>(scissor->Extent.x) == static_cast<i32>(Target.x));
    CHECK(scissor->Offset.y + static_cast<i32>(scissor->Extent.y) == static_cast<i32>(Target.y));
}

TEST_CASE("gui scissor: a clip wholly off the attachment covers nothing")
{
    CHECK_FALSE(
        ResolveGuiScissor(Gui::Rect{.Min = {-500.0f, 10.0f}, .Size = {200.0f, 50.0f}}, 1.0f, Target)
            .has_value());
    CHECK_FALSE(
        ResolveGuiScissor(Gui::Rect{.Min = {10.0f, 900.0f}, .Size = {200.0f, 50.0f}}, 1.0f, Target)
            .has_value());
}

TEST_CASE("gui scissor: a fractional clip is snapped out, never cut")
{
    // Both edges snap outward independently. Rounding the corner down while rounding the *size* up
    // is not the same thing and does not always cover: a clip starting deep inside a pixel and
    // spanning a whole number of them ends up a pixel short at the far edge, which is a cut column
    // of whatever hugs that edge.
    for (const Gui::Rect clip : {Gui::Rect{.Min = {10.4f, 20.6f}, .Size = {100.2f, 50.1f}},
                                 Gui::Rect{.Min = {10.9f, 20.9f}, .Size = {100.0f, 50.0f}}})
    {
        const optional<GuiScissor> scissor = ResolveGuiScissor(clip, 1.0f, Target);
        REQUIRE(scissor.has_value());
        CHECK(CoversVisiblePart(*scissor, clip, 1.0f));
    }
}
