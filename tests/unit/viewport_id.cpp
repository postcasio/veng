// ViewportId value-type + standalone ViewportRegistry cases (device-free).
//
// The value type's invalid spelling, IsValid predicate, and equality are pure
// logic, and the registry is standalone-constructible (no Context, device, or
// ICD), so both are exercised here with no driver. Minting requires a live
// Viewport (a GPU-band concern); these cases cover the value semantics and that
// a fresh registry resolves any unminted or invalid id to nullptr.

#include <doctest/doctest.h>

#include <Veng/Renderer/ViewportId.h>
#include <Veng/Renderer/ViewportRegistry.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("viewport_id: a default-constructed id is the invalid, names-no-viewport spelling")
{
    constexpr ViewportId invalid{};
    CHECK(invalid.Value == 0);
    CHECK_FALSE(invalid.IsValid());
}

TEST_CASE("viewport_id: IsValid tracks the non-zero value and equality is member-wise")
{
    constexpr ViewportId a{.Value = 7};
    constexpr ViewportId b{.Value = 7};
    constexpr ViewportId c{.Value = 8};

    CHECK(a.IsValid());
    CHECK(a == b);
    CHECK_FALSE(a == c);
}

TEST_CASE("viewport_id: a default-constructed id never equals a minted (non-zero) one")
{
    constexpr ViewportId invalid{};
    constexpr ViewportId minted{.Value = 1};

    CHECK_FALSE(invalid == minted);
    CHECK(minted.IsValid());
}

TEST_CASE("viewport_id: a standalone registry resolves an invalid or unminted id to nullptr")
{
    const ViewportRegistry registry;

    CHECK(registry.Resolve(ViewportId{}) == nullptr);
    CHECK(registry.Resolve(ViewportId{.Value = 1}) == nullptr);
    CHECK(registry.Resolve(ViewportId{.Value = 0xFFFFFFFFFFFFFFFFULL}) == nullptr);
}
