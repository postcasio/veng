// Sky-bake gate unit cases. ShouldRebakeMaterialSky decides whether a baked material sky must
// re-bake its radiance cube this resolve, from the content key, the cube's validity, whether a
// bake is outstanding, and the material identity/revision — no device involved. Both of its
// failure modes shipped as live bugs (an in-flight fill superseded every frame so the cube never
// filled; a transient world-swap gap clearing the key so an equal-content sky re-baked on every
// switch), so the decision is pinned here rather than only through a rendered frame.

#include <doctest/doctest.h>

#include "Renderer/SkyBakeGate.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Two distinct material identities and a keyed helper so cases read as intent, not argument
    // soup. The pointers are compared, never dereferenced.
    int MatA = 0;
    int MatB = 0;
    const void* const A = &MatA;
    const void* const B = &MatB;

    // The keyed regime with the material arm held fixed (same instance, same revision), so a keyed
    // case turns only on the key/validity/outstanding trio.
    bool Keyed(u64 key, u64 lastKey, bool valid, bool outstanding)
    {
        return ShouldRebakeMaterialSky(key, lastKey, valid, outstanding, A, A, 7, 7);
    }
}

TEST_CASE("sky bake gate: keyed regime shares one bake across equal-content worlds")
{
    constexpr u64 K = 0x1234'5678'9abc'def0ULL;

    // The first keyed resolve: no cube, none outstanding, last key still 0 → bake.
    CHECK(Keyed(K, 0, /*valid*/ false, /*outstanding*/ false));

    // During the amortized fill the cube is not yet valid but a bake IS outstanding (Pending, or
    // the one-frame Landed gap before the copy). Re-requesting here would supersede the in-flight
    // fill every frame and it would never land — the first live bug. The gate must NOT re-bake.
    CHECK_FALSE(Keyed(K, K, /*valid*/ false, /*outstanding*/ true));

    // The bake landed: the cube is valid and the key is unchanged → no re-bake.
    CHECK_FALSE(Keyed(K, K, /*valid*/ true, /*outstanding*/ false));

    // A transient no-sky gap (a world swap before the destination authored its Sky) leaves the key
    // and the cube standing; the equal-key sky returns to a valid cube → no re-bake. This is the
    // property that stops a re-bake on every regime switch — the second live bug.
    CHECK_FALSE(Keyed(K, K, /*valid*/ true, /*outstanding*/ false));

    // A genuine content change (a jump, a live tuning retune, a dust volume settling in) moves the
    // key → re-bake.
    CHECK(Keyed(0xdead'beef'0000'0001ULL, K, /*valid*/ true, /*outstanding*/ false));

    // A bake abandoned before it landed leaves no valid cube and nothing outstanding → re-bake,
    // even though the key is unchanged.
    CHECK(Keyed(K, K, /*valid*/ false, /*outstanding*/ false));
}

TEST_CASE("sky bake gate: unkeyed regime is the material-identity gate")
{
    // Key 0 selects the historical gate: the validity/outstanding trio is ignored entirely.
    // Same instance, same revision → no re-bake, whatever the cube state.
    CHECK_FALSE(ShouldRebakeMaterialSky(0, 0, /*valid*/ true, /*outstanding*/ false, A, A, 3, 3));
    CHECK_FALSE(ShouldRebakeMaterialSky(0, 0, /*valid*/ false, /*outstanding*/ false, A, A, 3, 3));

    // A material swap re-bakes.
    CHECK(ShouldRebakeMaterialSky(0, 0, true, false, B, A, 3, 3));

    // An in-place revision bump (same instance, params rewritten) re-bakes.
    CHECK(ShouldRebakeMaterialSky(0, 0, true, false, A, A, 4, 3));
}
