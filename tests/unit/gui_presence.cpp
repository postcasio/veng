// Gui::Presence and Gui::KeyedPresence: the device-free open/close presence and its keyed swap
// machine, driven by deterministic stepping (no Context, no font). The properties pinned here are
// the eased alpha crossing the hidden threshold, the goal-signed slide that collapses to zero when
// fully open, and the four-state keyed swap — open from empty, close to empty, swap while open,
// swap while closing, and re-desiring the still-closing key.

#include <doctest/doctest.h>

#include <optional>

#include <Veng/Gui/Presence.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    constexpr f32 Dt = 1.0f / 60.0f;

    void Step(KeyedPresence<int>& keyed, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            keyed.Update(Dt);
        }
    }
}

TEST_CASE("gui_presence: alpha eases open and closed across the hidden threshold")
{
    Presence presence;
    CHECK(presence.IsHidden());
    CHECK(presence.GetAlpha() == doctest::Approx(0.0f));

    for (int i = 0; i < 300; ++i)
    {
        presence.Update(true, Dt);
    }
    CHECK_FALSE(presence.IsHidden());
    CHECK(presence.GetAlpha() == doctest::Approx(1.0f));

    for (int i = 0; i < 400; ++i)
    {
        presence.Update(false, Dt);
    }
    CHECK(presence.IsHidden());
}

TEST_CASE("gui_presence: GetSlide is signed by the goal and collapses to zero when fully open")
{
    Presence presence;

    // One opening step: goal true, alpha still below 1, so the slide is positive for a positive
    // travel (the element enters from the +travel offset toward zero).
    presence.Update(true, Dt);
    CHECK(presence.GetSlide(100.0f) > 0.0f);

    // Fully open: the slide is continuous through alpha = 1, where it is exactly zero.
    for (int i = 0; i < 300; ++i)
    {
        presence.Update(true, Dt);
    }
    CHECK(presence.GetSlide(100.0f) == doctest::Approx(0.0f));

    // One closing step from open: goal false, alpha below 1, so the slide flips negative (the
    // element exits toward the -travel offset).
    presence.Update(false, Dt);
    CHECK(presence.GetSlide(100.0f) < 0.0f);
}

TEST_CASE("gui_presence: KeyedPresence opens from empty and closes back to empty")
{
    KeyedPresence<int> keyed;
    CHECK_FALSE(keyed.GetShown().has_value());
    CHECK_FALSE(keyed.IsDisplayed());

    keyed.SetDesired(1);
    keyed.Update(Dt);
    REQUIRE(keyed.GetShown().has_value());
    CHECK(*keyed.GetShown() == 1);

    Step(keyed, 300);
    CHECK(keyed.IsDisplayed());
    CHECK(*keyed.GetShown() == 1);

    keyed.SetDesired(std::nullopt);
    Step(keyed, 400);
    CHECK_FALSE(keyed.IsDisplayed());
    // The shown key is forgotten once hidden, so a later re-desire reopens from empty.
    CHECK_FALSE(keyed.GetShown().has_value());
}

TEST_CASE(
    "gui_presence: KeyedPresence swap while open holds the stale key until hidden, then opens "
    "the new one")
{
    KeyedPresence<int> keyed;
    keyed.SetDesired(1);
    Step(keyed, 300);
    REQUIRE(keyed.IsDisplayed());
    CHECK(*keyed.GetShown() == 1);

    // Desire a new key while open: the machine closes over the still-shown stale key 1 rather than
    // cross-fading, so the shown key stays 1 for every frame the panel is still visible.
    keyed.SetDesired(2);
    int guard = 0;
    while (keyed.IsDisplayed() && guard++ < 10000)
    {
        REQUIRE(keyed.GetShown().has_value());
        CHECK(*keyed.GetShown() == 1);
        keyed.Update(Dt);
    }
    // Now fully hidden but the swap has not yet adopted the new key: the stale key is still reported.
    CHECK(*keyed.GetShown() == 1);

    // The next step adopts the desired key and reopens on fresh content.
    keyed.Update(Dt);
    REQUIRE(keyed.GetShown().has_value());
    CHECK(*keyed.GetShown() == 2);

    Step(keyed, 300);
    CHECK(keyed.IsDisplayed());
    CHECK(*keyed.GetShown() == 2);
}

TEST_CASE("gui_presence: KeyedPresence swap while closing retargets to the new key once hidden")
{
    KeyedPresence<int> keyed;
    keyed.SetDesired(1);
    Step(keyed, 300);

    // Begin closing (nothing desired), then, before it hides, desire a different key.
    keyed.SetDesired(std::nullopt);
    keyed.Update(Dt);
    REQUIRE(keyed.IsDisplayed());
    CHECK(*keyed.GetShown() == 1);

    keyed.SetDesired(2);
    int guard = 0;
    while (keyed.IsDisplayed() && guard++ < 10000)
    {
        CHECK(*keyed.GetShown() == 1);
        keyed.Update(Dt);
    }
    CHECK(*keyed.GetShown() == 1);

    keyed.Update(Dt);
    CHECK(*keyed.GetShown() == 2);

    Step(keyed, 300);
    CHECK(keyed.IsDisplayed());
    CHECK(*keyed.GetShown() == 2);
}

TEST_CASE("gui_presence: KeyedPresence re-desiring the still-closing key reopens it without a swap")
{
    KeyedPresence<int> keyed;
    keyed.SetDesired(1);
    Step(keyed, 300);
    REQUIRE(keyed.GetPresence().GetAlpha() == doctest::Approx(1.0f));

    // Start closing, let it decay part-way (still visible), then re-desire the same key.
    keyed.SetDesired(std::nullopt);
    Step(keyed, 4);
    REQUIRE(keyed.IsDisplayed());
    CHECK(keyed.GetPresence().GetAlpha() < 1.0f);
    CHECK(*keyed.GetShown() == 1);

    keyed.SetDesired(1);
    Step(keyed, 300);
    CHECK(keyed.IsDisplayed());
    CHECK(*keyed.GetShown() == 1);
    CHECK(keyed.GetPresence().GetAlpha() == doctest::Approx(1.0f));
}

TEST_CASE("gui_presence: KeyedPresence exposes its presence for configuration")
{
    KeyedPresence<int> keyed;
    keyed.GetPresence().Speed = 4.0f;
    CHECK(keyed.GetPresence().Speed == doctest::Approx(4.0f));
}
