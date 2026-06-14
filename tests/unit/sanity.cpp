// Trivial sanity case proving the framework + CTest discovery are wired
// correctly.
#include <doctest/doctest.h>

TEST_CASE("framework sanity")
{
    CHECK(1 + 1 == 2);
}
