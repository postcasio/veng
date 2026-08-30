// The seated-surface claim rule (Renderer::ClaimsSeatedSurface): which of a scene's presenting
// viewports drives a GuiSurface's GuiDriver. The property under guard is that every surface is
// drivable in every posture — in particular the default single-player one, where no consumer binds
// a viewport seat (Viewport::SetSeat's Null default) and a surface still has to name a seat to take
// routed pointer input: a seat-equality-only rule leaves such a surface's driver unrunnable
// forever, its document rendering while nothing steps it.

#include <doctest/doctest.h>

#include "Renderer/SurfaceClaim.h"

using namespace Veng;
using Renderer::ClaimsSeatedSurface;

namespace
{
    constexpr Entity SeatA{.Index = 1, .Generation = 0};
    constexpr Entity SeatB{.Index = 2, .Generation = 0};
}

TEST_CASE("surface claim: the unbound sole presenter drives a seated surface")
{
    // The single-player posture: the surface names its seat for input, no viewport binds one, and
    // the sole presenter must claim it or the driver never runs anywhere.
    CHECK(ClaimsSeatedSurface(SeatA, Entity::Null, true, false));
    // Among several unbound presenters of one scene, exactly the primary claims it.
    CHECK_FALSE(ClaimsSeatedSurface(SeatA, Entity::Null, false, false));
}

TEST_CASE("surface claim: a bound viewport claims exactly its own seat's surfaces")
{
    // Real split-screen: the viewport bound to the surface's seat claims it whether or not it is
    // the primary presenter, and a viewport bound to a different seat never does.
    CHECK(ClaimsSeatedSurface(SeatA, SeatA, false, false));
    CHECK(ClaimsSeatedSurface(SeatA, SeatA, true, false));
    CHECK_FALSE(ClaimsSeatedSurface(SeatA, SeatB, true, false));
    CHECK_FALSE(ClaimsSeatedSurface(SeatA, SeatB, false, false));
}

TEST_CASE("surface claim: an unbound presenter yields to the viewport bound to the seat")
{
    // The mixed posture: one viewport genuinely binds the surface's seat, so its claim is
    // exclusive — the unbound primary presenter must not produce a second driver instance.
    CHECK_FALSE(ClaimsSeatedSurface(SeatA, Entity::Null, true, true));
    CHECK_FALSE(ClaimsSeatedSurface(SeatA, Entity::Null, false, true));
}

TEST_CASE("surface claim: an unseated surface follows the primary presenter alone")
{
    // The cockpit posture: a surface naming no seat is display-only for input and is driven by the
    // sole/primary presenter, bound seat or not.
    CHECK(ClaimsSeatedSurface(Entity::Null, Entity::Null, true, false));
    CHECK_FALSE(ClaimsSeatedSurface(Entity::Null, Entity::Null, false, false));
    CHECK(ClaimsSeatedSurface(Entity::Null, SeatA, true, false));
    CHECK_FALSE(ClaimsSeatedSurface(Entity::Null, SeatA, false, false));
}
