// The pure anisotropy-resolve helper: a settable global anisotropy value is bounded to the device
// maximum here, and a disabled request ignores the value entirely. Device-free — the clamp is a
// function of its arguments alone.

#include <doctest/doctest.h>

#include <Veng/Renderer/Sampler.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("sampler anisotropy: an enabled request clamps into [1, deviceMax]")
{
    // Above the device max clamps down to it — the correctness fix a settable value forces.
    CHECK(ResolveAnisotropy(true, 32.0f, 16.0f).MaxAnisotropy == doctest::Approx(16.0f));
    // Within range passes through unchanged, still enabled.
    const ResolvedAnisotropy within = ResolveAnisotropy(true, 4.0f, 16.0f);
    CHECK(within.Enabled);
    CHECK(within.MaxAnisotropy == doctest::Approx(4.0f));
    // Below 1 clamps up to 1 (1x is filtering on at the minimum sample count).
    CHECK(ResolveAnisotropy(true, 0.25f, 16.0f).MaxAnisotropy == doctest::Approx(1.0f));
}

TEST_CASE("sampler anisotropy: a disabled request ignores the value")
{
    const ResolvedAnisotropy disabled = ResolveAnisotropy(false, 32.0f, 16.0f);
    CHECK_FALSE(disabled.Enabled);
    CHECK(disabled.MaxAnisotropy == doctest::Approx(1.0f));
}

TEST_CASE("sampler anisotropy: the default reproduces today's 8x")
{
    // The spec guarantees a device max of at least 16 with the feature enabled, so the shared
    // default is always representable and resolves to itself — the byte-identical-default property.
    const ResolvedAnisotropy defaulted = ResolveAnisotropy(true, DefaultMaxAnisotropy, 16.0f);
    CHECK(defaulted.Enabled);
    CHECK(defaulted.MaxAnisotropy == doctest::Approx(8.0f));
}
