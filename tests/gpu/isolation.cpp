// Per-case isolation proof (planset-4, plan 05): the thing 5b adds over the
// one-exe band. Two TEST_CASE_FIXTUREs each create a Buffer with the *same*
// name and size on their *own* GpuFixture (and therefore their own
// Context::Initialize/Dispose lifecycle), upload a distinct deterministic byte
// pattern, and verify it round-trips correctly.
//
// Because doctest default-constructs the fixture per test case and destroys
// it afterwards, the second case's Context is a brand-new device: this proves
// there's no carried state, no leaked handles, and no crash from creating a
// same-named resource on a fresh device after the first Context was fully
// disposed. It's also the first in-process exercise of de-global's
// multi-context capability (still single-threaded, one Context alive at a
// time).

#include <array>
#include <numeric>

#include <doctest/doctest.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u64 Size = 32;
    constexpr string_view Name = "Isolation Buffer";

    void RoundTrip(Context& context, u8 fillStart)
    {
        auto buffer = Buffer::Create(context, {
            .Name = string(Name),
            .Size = Size,
            .Usage = BufferUsage::TransferSrc | BufferUsage::TransferDst,
        });

        std::array<u8, Size> source{};
        std::iota(source.begin(), source.end(), fillStart);

        buffer->Upload(source);

        const vector<u8> downloaded = buffer->Download();

        REQUIRE(downloaded.size() == Size);

        for (u64 i = 0; i < Size; i++)
        {
            CAPTURE(i);
            CHECK(downloaded[i] == source[i]);
        }
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "isolation: first case creates and verifies a fresh-context buffer")
{
    RoundTrip(Context, /* fillStart = */ 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "isolation: second case, same name/size, on its own fresh context")
{
    RoundTrip(Context, /* fillStart = */ 128);
}
