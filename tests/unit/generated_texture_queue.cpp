// The generated-texture service's scheduling policy, exercised against a mock tick recorder with
// no device: idempotent keys, priority-then-FIFO selection re-evaluated per tick, budget accounting
// across mixed jobs, and the completion that fires exactly once on the tick that exhausts a job.
//
// GeneratedTextureQueue is renderer-internal (engine/src is on this target's include path) and is
// pure integer/order arithmetic, so the whole policy surface is pinned here rather than through a
// GPU case that would only observe it indirectly.

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "Renderer/GeneratedTextureQueue.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Records every tick and completion as "key.tickIndex" / "key!" so an expected schedule reads
    // as a literal sequence rather than as a set of counters.
    struct Recorder
    {
        std::vector<std::string> Log;

        [[nodiscard]] function<void(u64, u32, u32)> Tick()
        {
            return [this](const u64 key, const u32 tickIndex, const u32 tickCount)
            {
                CHECK(tickIndex < tickCount);
                Log.push_back(std::to_string(key) + "." + std::to_string(tickIndex));
            };
        }

        [[nodiscard]] function<void(u64)> Complete()
        {
            return [this](const u64 key) { Log.push_back(std::to_string(key) + "!"); };
        }
    };
}

TEST_CASE("GeneratedTextureQueue: a request is idempotent on its key")
{
    GeneratedTextureQueue queue;

    CHECK(queue.Add(7, 4, 0));
    CHECK_FALSE(queue.Add(7, 99, 100));

    REQUIRE(queue.Find(7) != nullptr);
    // The second request changed nothing — not the tick count, not the priority.
    CHECK(queue.Find(7)->TickCount == 4u);
    CHECK(queue.Find(7)->Priority == 0);
    CHECK(queue.GetJobs().size() == 1u);
    CHECK(queue.GetPendingCount() == 1u);

    // A resident key is still live, so re-requesting it does not restart the job.
    Recorder recorder;
    CHECK(queue.Spend(GeneratedTextureQueue::UnlimitedTicks, recorder.Tick(),
                      recorder.Complete()) == 4u);
    CHECK(queue.Find(7)->State == GeneratedTextureState::Resident);
    CHECK_FALSE(queue.Add(7, 4, 0));

    CHECK(queue.Remove(7));
    CHECK_FALSE(queue.Contains(7));
    CHECK_FALSE(queue.Remove(7));
}

TEST_CASE("GeneratedTextureQueue: selection is priority first, request order second")
{
    GeneratedTextureQueue queue;
    queue.Add(1, 1, 0);
    queue.Add(2, 1, 5);
    queue.Add(3, 1, 0);
    queue.Add(4, 1, 5);

    Recorder recorder;
    const u32 spent =
        queue.Spend(GeneratedTextureQueue::UnlimitedTicks, recorder.Tick(), recorder.Complete());

    CHECK(spent == 4u);
    // The two priority-5 jobs run first, among themselves in request order; then the two zeros.
    CHECK(recorder.Log ==
          std::vector<std::string>{"2.0", "2!", "4.0", "4!", "1.0", "1!", "3.0", "3!"});
    CHECK(queue.GetPendingCount() == 0u);
    CHECK(queue.GetResidentCount() == 4u);
}

TEST_CASE("GeneratedTextureQueue: a budget is spent across jobs and resumes where it stopped")
{
    GeneratedTextureQueue queue;
    queue.Add(1, 3, 0);
    queue.Add(2, 2, 0);

    Recorder first;
    CHECK(queue.Spend(2, first.Tick(), first.Complete()) == 2u);
    CHECK(first.Log == std::vector<std::string>{"1.0", "1.1"});
    CHECK(queue.Find(1)->State == GeneratedTextureState::Running);
    CHECK(queue.Find(2)->State == GeneratedTextureState::Queued);

    // The next pump resumes job 1 at its third tick, completes it, and moves on to job 2.
    Recorder second;
    CHECK(queue.Spend(2, second.Tick(), second.Complete()) == 2u);
    CHECK(second.Log == std::vector<std::string>{"1.2", "1!", "2.0"});

    Recorder third;
    CHECK(queue.Spend(2, third.Tick(), third.Complete()) == 1u);
    CHECK(third.Log == std::vector<std::string>{"2.1", "2!"});

    // Nothing pending: a further pump spends nothing at all.
    Recorder fourth;
    CHECK(queue.Spend(2, fourth.Tick(), fourth.Complete()) == 0u);
    CHECK(fourth.Log.empty());
}

TEST_CASE("GeneratedTextureQueue: raising a priority preempts at the next tick, not the next job")
{
    GeneratedTextureQueue queue;
    queue.Add(1, 4, 0);
    queue.Add(2, 2, 0);

    Recorder first;
    CHECK(queue.Spend(1, first.Tick(), first.Complete()) == 1u);
    CHECK(first.Log == std::vector<std::string>{"1.0"});

    CHECK(queue.SetPriority(2, 10));
    CHECK_FALSE(queue.SetPriority(99, 10));

    // Job 1 is running and unfinished, but selection is re-evaluated per tick, so job 2 takes
    // every remaining tick before job 1 sees another.
    Recorder second;
    CHECK(queue.Spend(3, second.Tick(), second.Complete()) == 3u);
    CHECK(second.Log == std::vector<std::string>{"2.0", "2.1", "2!", "1.1"});
}

TEST_CASE("GeneratedTextureQueue: a removed job takes no further ticks and never completes")
{
    GeneratedTextureQueue queue;
    queue.Add(1, 4, 0);
    queue.Add(2, 1, 0);

    Recorder first;
    CHECK(queue.Spend(2, first.Tick(), first.Complete()) == 2u);
    CHECK(first.Log == std::vector<std::string>{"1.0", "1.1"});

    CHECK(queue.Remove(1));

    Recorder second;
    CHECK(queue.Spend(GeneratedTextureQueue::UnlimitedTicks, second.Tick(), second.Complete()) ==
          1u);
    CHECK(second.Log == std::vector<std::string>{"2.0", "2!"});
    CHECK_FALSE(queue.Contains(1));
}

TEST_CASE("GeneratedTextureQueue: a zero tick count is one tick, and a zero budget spends nothing")
{
    GeneratedTextureQueue queue;
    queue.Add(1, 0, 0);
    REQUIRE(queue.Find(1) != nullptr);
    CHECK(queue.Find(1)->TickCount == 1u);

    Recorder none;
    CHECK(queue.Spend(0, none.Tick(), none.Complete()) == 0u);
    CHECK(none.Log.empty());
    CHECK(queue.Find(1)->State == GeneratedTextureState::Queued);

    Recorder one;
    CHECK(queue.Spend(1, one.Tick(), one.Complete()) == 1u);
    CHECK(one.Log == std::vector<std::string>{"1.0", "1!"});
}
