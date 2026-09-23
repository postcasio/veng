// GPU timing on one-shot command buffers: scopes a caller brackets inside an ImmediateCommands
// callback are timed against the one-shot recording's own query pool and read back by the time it
// returns, with the nesting and ordering GetLastGpuPassTimings reports for the frame. A device
// without timestamp support reports nothing, which the cases accept rather than fail on.

#include <doctest/doctest.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "immediate gpu timing: a one-shot recording's scopes are timed and nested")
{
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            Context.BeginGpuScope(cmd, "Outer");
            Context.BeginGpuScope(cmd, "Inner");
            Context.EndGpuScope(cmd);
            Context.EndGpuScope(cmd);
        });

    const std::span<const Context::GpuPassTiming> timings =
        Context.GetLastImmediateGpuPassTimings();
    if (!Context.IsGpuTimingSupported())
    {
        CHECK(timings.empty());
        return;
    }
    REQUIRE(timings.size() == 2);
    CHECK(timings[0].Name == "Outer");
    CHECK(timings[1].Name == "Inner");
    CHECK(timings[0].Depth == 0);
    CHECK(timings[1].Depth == 1);
    CHECK(timings[0].BeginNanos == 0);
    CHECK(timings[1].BeginNanos >= timings[0].BeginNanos);
    CHECK(timings[1].EndNanos <= timings[0].EndNanos);
    CHECK(timings[0].Milliseconds >= timings[1].Milliseconds);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "immediate gpu timing: each recording replaces the last, and a nested one is "
                  "untimed")
{
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            Context.BeginGpuScope(cmd, "First");
            Context.EndGpuScope(cmd);
        });

    // A recording opening no scope leaves nothing to report.
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(Context.GetLastImmediateGpuPassTimings().empty());

    // The outer recording owns the pool until its submit is read back, so the scope the nested
    // one opens is not measured and the outer's own is what is reported.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            Context.BeginGpuScope(cmd, "Outer");
            Context.ImmediateCommands(
                [&](CommandBuffer& nested)
                {
                    Context.BeginGpuScope(nested, "Nested");
                    Context.EndGpuScope(nested);
                });
            Context.EndGpuScope(cmd);
        });
    const std::span<const Context::GpuPassTiming> timings =
        Context.GetLastImmediateGpuPassTimings();
    if (!Context.IsGpuTimingSupported())
    {
        CHECK(timings.empty());
        return;
    }
    REQUIRE(timings.size() == 1);
    CHECK(timings[0].Name == "Outer");
}
