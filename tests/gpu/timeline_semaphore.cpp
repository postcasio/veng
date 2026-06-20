// TimelineSemaphore host-side round-trip cases:
// host signal then wait on the same value returns immediately; GetValue
// reflects the latest signal; and signalling increasing values advances the
// counter monotonically. Queue-side wait/signal go through the submit info and
// are exercised by the async upload path, not here.

#include <doctest/doctest.h>

#include <Veng/Renderer/TimelineSemaphore.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "timeline semaphore: host signal then wait returns immediately")
{
    auto timeline = TimelineSemaphore::Create(Context);

    timeline->Signal(7);

    // The counter already reached 7, so the wait must return without blocking.
    timeline->Wait(7);

    CHECK(timeline->GetValue() == 7);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "timeline semaphore: GetValue reflects latest host signal")
{
    auto timeline = TimelineSemaphore::Create(Context, 3);

    CHECK(timeline->GetValue() == 3);

    timeline->Signal(42);

    CHECK(timeline->GetValue() == 42);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "timeline semaphore: signalling increasing values advances the counter")
{
    auto timeline = TimelineSemaphore::Create(Context);

    CHECK(timeline->GetValue() == 0);

    for (u64 value = 1; value <= 5; value++)
    {
        timeline->Signal(value);
        CAPTURE(value);
        CHECK(timeline->GetValue() == value);
    }
}
