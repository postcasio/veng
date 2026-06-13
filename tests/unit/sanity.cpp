// Trivial sanity case proving the framework + CTest discovery are wired before
// the real bands (plans 02–04) land. Harmless to keep; safe to delete once
// those add coverage.
#include <doctest/doctest.h>

TEST_CASE("framework sanity")
{
    CHECK(1 + 1 == 2);
}
