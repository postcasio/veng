// Hash64/HashCombine/Rng/Noise/Fbm: pure CPU math, no Context, no Vulkan symbol
// touched (the Math/ pure-helper pattern — see frustum.cpp/bvh.cpp). The core
// properties pinned here are determinism (an Rng stream and a hash are exact
// replays of the same seed, byte-identical) and divergence (two distinct seeds
// produce different streams/fields), plus avalanche/distribution sanity on the
// hash and range/continuity on the noise.

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <tuple>

#include <Veng/Math/Noise.h>
#include <Veng/Math/Random.h>

using namespace Veng;

TEST_CASE("math_random: Hash64 is a pure function: identical input yields identical output")
{
    CHECK(Hash64(12345ULL) == Hash64(12345ULL));
    CHECK(Hash64(0ULL) == Hash64(0ULL));
}

TEST_CASE(
    "math_random: Hash64 avalanches: a one-bit input change flips roughly half the output bits")
{
    // Strict avalanche criterion sanity, not a formal SAC proof: flipping each
    // input bit in turn should flip somewhere in the neighborhood of 32 of the 64
    // output bits, not a handful (a weak/linear mix) or all-or-nothing.
    const u64 base = Hash64(0xA5A5A5A5A5A5A5A5ULL);
    for (u32 bit = 0; bit < 64; ++bit)
    {
        const u64 flipped = Hash64(0xA5A5A5A5A5A5A5A5ULL ^ (1ULL << bit));
        const u64 diff = base ^ flipped;
        const i32 setBits = std::popcount(diff);
        CHECK(setBits >= 20);
        CHECK(setBits <= 44);
    }
}

TEST_CASE(
    "math_random: Hash64 distribution: hashing a sequential range spreads across the byte range")
{
    // A weak mix would leave the low byte correlated with the sequential input;
    // bucket the low byte of 4096 sequential hashes into 16 buckets and check no
    // bucket is wildly over/under-represented (a coarse chi-square-ish sanity, not
    // a rigorous statistical test).
    constexpr u32 sampleCount = 4096;
    constexpr u32 bucketCount = 16;
    std::array<u32, bucketCount> buckets{};
    for (u32 i = 0; i < sampleCount; ++i)
    {
        const u64 hash = Hash64(static_cast<u64>(i));
        const u32 bucket = static_cast<u32>(hash & 0xFF) * bucketCount / 256;
        ++buckets[bucket];
    }

    const f64 expected = static_cast<f64>(sampleCount) / bucketCount;
    for (const u32 count : buckets)
    {
        CHECK(static_cast<f64>(count) > expected * 0.5);
        CHECK(static_cast<f64>(count) < expected * 1.5);
    }
}

TEST_CASE("math_random: HashCombine is order-dependent")
{
    const u64 ab = HashCombine(HashCombine(0, 1), 2);
    const u64 ba = HashCombine(HashCombine(0, 2), 1);
    CHECK(ab != ba);
}

TEST_CASE("math_random: Hash64(span) folds a tuple of integers deterministically, order-dependent")
{
    const std::array<u64, 3> a = {1, 2, 3};
    const std::array<u64, 3> aAgain = {1, 2, 3};
    const std::array<u64, 3> reordered = {3, 2, 1};

    CHECK(Hash64(std::span<const u64>(a)) == Hash64(std::span<const u64>(aAgain)));
    CHECK(Hash64(std::span<const u64>(a)) != Hash64(std::span<const u64>(reordered)));
}

TEST_CASE("math_random: Rng replays byte-identically from the same seed")
{
    Rng a(42);
    Rng b(42);
    for (u32 i = 0; i < 256; ++i)
    {
        CHECK(a.NextU32() == b.NextU32());
    }
}

TEST_CASE("math_random: Rng: two distinct seeds diverge")
{
    Rng a(1);
    Rng b(2);

    u32 differing = 0;
    for (u32 i = 0; i < 64; ++i)
    {
        if (a.NextU32() != b.NextU32())
        {
            ++differing;
        }
    }
    // Two independent streams should differ on nearly every draw; a handful of
    // incidental collisions is fine, but the streams must not be secretly aliased.
    CHECK(differing >= 60);
}

TEST_CASE("math_random: Rng: copying yields an independent stream from the same point")
{
    Rng original(7);
    // Advance the original before branching so the copy starts mid-stream.
    std::ignore = original.NextU32();
    std::ignore = original.NextU32();

    Rng copy = original;
    for (u32 i = 0; i < 32; ++i)
    {
        CHECK(original.NextU32() == copy.NextU32());
    }
}

TEST_CASE("math_random: Rng::NextFloat stays within the half-open unit interval")
{
    Rng rng(99);
    for (u32 i = 0; i < 4096; ++i)
    {
        const f32 value = rng.NextFloat();
        CHECK(value >= 0.0f);
        CHECK(value < 1.0f);
    }
}

TEST_CASE("math_random: Rng::NextFloat(lo, hi) stays within range")
{
    Rng rng(11);
    for (u32 i = 0; i < 1024; ++i)
    {
        const f32 value = rng.NextFloat(-5.0f, 5.0f);
        CHECK(value >= -5.0f);
        CHECK(value < 5.0f);
    }
}

TEST_CASE("math_random: Rng::NextInSphere stays within the unit ball")
{
    Rng rng(2024);
    for (u32 i = 0; i < 1024; ++i)
    {
        const vec3 p = rng.NextInSphere();
        CHECK(glm::dot(p, p) <= 1.0f);
    }
}

TEST_CASE("math_random: ValueNoise stays within approximately [-1, 1] and is deterministic")
{
    Rng rng(5);
    for (u32 i = 0; i < 256; ++i)
    {
        const vec3 p = rng.NextFloat(-100.0f, 100.0f) * vec3(1.0f, 0.0f, 0.0f) +
                       rng.NextFloat(-100.0f, 100.0f) * vec3(0.0f, 1.0f, 0.0f) +
                       rng.NextFloat(-100.0f, 100.0f) * vec3(0.0f, 0.0f, 1.0f);
        const f32 value = ValueNoise(p, 123);
        CHECK(value >= -1.0f);
        CHECK(value <= 1.0f);
        CHECK(ValueNoise(p, 123) == value);
    }
}

TEST_CASE("math_random: GradientNoise stays within approximately [-1, 1] and is deterministic")
{
    Rng rng(6);
    for (u32 i = 0; i < 256; ++i)
    {
        const vec3 p(rng.NextFloat(-100.0f, 100.0f), rng.NextFloat(-100.0f, 100.0f),
                     rng.NextFloat(-100.0f, 100.0f));
        const f32 value = GradientNoise(p, 456);
        CHECK(value >= -1.0f);
        CHECK(value <= 1.0f);
        CHECK(GradientNoise(p, 456) == value);
    }
}

TEST_CASE("math_random: GradientNoise: different seeds produce different fields at the same point")
{
    const vec3 p(3.3f, -1.7f, 8.1f);
    CHECK(GradientNoise(p, 1) != GradientNoise(p, 2));
}

TEST_CASE("math_random: GradientNoise is continuous: a small step changes the value by only a "
          "small amount")
{
    Rng rng(9);
    for (u32 i = 0; i < 64; ++i)
    {
        const vec3 p(rng.NextFloat(-50.0f, 50.0f), rng.NextFloat(-50.0f, 50.0f),
                     rng.NextFloat(-50.0f, 50.0f));
        const f32 base = GradientNoise(p, 77);
        const vec3 step(0.001f, 0.0f, 0.0f);
        const f32 stepped = GradientNoise(p + step, 77);
        CHECK(std::abs(stepped - base) < 0.05f);
    }
}

TEST_CASE("math_random: GradientNoise is zero at every integer lattice point")
{
    // Every lattice corner's own gradient is dotted against a zero offset there,
    // so the field passes through zero at every integer coordinate regardless of seed.
    for (i32 x = -3; x <= 3; ++x)
    {
        for (i32 y = -3; y <= 3; ++y)
        {
            for (i32 z = -3; z <= 3; ++z)
            {
                CHECK(GradientNoise(
                          vec3(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)),
                          314) == doctest::Approx(0.0f).epsilon(0.0001));
            }
        }
    }
}

TEST_CASE("math_random: Fbm with one octave equals a single GradientNoise evaluation")
{
    const vec3 p(1.5f, -2.5f, 3.5f);
    CHECK(Fbm(p, 55, 1) == doctest::Approx(GradientNoise(p, HashCombine(55, 0))));
}

TEST_CASE("math_random: Fbm stays within approximately [-1, 1] across octave counts")
{
    Rng rng(21);
    for (u32 i = 0; i < 64; ++i)
    {
        const vec3 p(rng.NextFloat(-20.0f, 20.0f), rng.NextFloat(-20.0f, 20.0f),
                     rng.NextFloat(-20.0f, 20.0f));
        for (const u32 octaves : {1u, 2u, 4u, 8u})
        {
            const f32 value = Fbm(p, 8080, octaves);
            CHECK(value >= -1.0f);
            CHECK(value <= 1.0f);
        }
    }
}

TEST_CASE("math_random: Fbm is deterministic across repeated calls")
{
    const vec3 p(-4.2f, 0.6f, 12.9f);
    CHECK(Fbm(p, 999, 4) == Fbm(p, 999, 4));
}
