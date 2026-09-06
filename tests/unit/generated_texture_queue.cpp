// The generated-texture service's scheduling policy, exercised against a mock tick recorder with
// no device: idempotent keys, priority-then-FIFO selection re-evaluated per tick, cost-budget
// accounting across mixed jobs, and the completion that fires exactly once on the tick that
// exhausts a job. Every job at the default cost of one makes the cost budget a tick count, so the
// cases below that pass a plain budget are also the evidence the default reproduces the prior
// schedule; the cost-specific cases add the mixed-cost, amortization and one-tick-minimum properties.
//
// GeneratedTextureQueue is renderer-internal (engine/src is on this target's include path) and is
// pure integer/order arithmetic, so the whole policy surface is pinned here rather than through a
// GPU case that would only observe it indirectly.

#include <map>
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
    CHECK(queue.Spend(GeneratedTextureQueue::UnlimitedCost, recorder.Tick(), recorder.Complete()) ==
          4u);
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
        queue.Spend(GeneratedTextureQueue::UnlimitedCost, recorder.Tick(), recorder.Complete());

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
    CHECK(queue.Spend(GeneratedTextureQueue::UnlimitedCost, second.Tick(), second.Complete()) ==
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

TEST_CASE("GeneratedTextureQueue: a held job stays live and selectable neighbours run past it")
{
    GeneratedTextureQueue queue;
    queue.Add(1, 2, 10); // The higher priority, so it would win every selection.
    queue.Add(2, 1, 0);

    CHECK(queue.SetHeld(1, true));
    CHECK_FALSE(queue.SetHeld(3, true));

    // Held is not removed: the key is still live, so a re-request is still dropped, and the job
    // still counts as pending.
    CHECK(queue.Contains(1));
    CHECK_FALSE(queue.Add(1, 2, 10));
    CHECK(queue.GetPendingCount() == 2u);
    CHECK(queue.GetSelectableCount() == 1u);
    CHECK(queue.NextKey() == optional<u64>{2});

    Recorder held;
    CHECK(queue.Spend(GeneratedTextureQueue::UnlimitedCost, held.Tick(), held.Complete()) == 1u);
    CHECK(held.Log == std::vector<std::string>{"2.0", "2!"});
    CHECK(queue.Find(1)->TicksDone == 0u);

    // Releasing the hold resumes it from where it was — at the beginning, since no tick ran.
    CHECK(queue.SetHeld(1, false));
    Recorder released;
    CHECK(queue.Spend(GeneratedTextureQueue::UnlimitedCost, released.Tick(), released.Complete()) ==
          2u);
    CHECK(released.Log == std::vector<std::string>{"1.0", "1.1", "1!"});
}

TEST_CASE("GeneratedTextureQueue: a job marked resident abandons its remaining ticks")
{
    GeneratedTextureQueue queue;
    queue.Add(1, 8, 0);
    queue.SetHeld(1, true);

    CHECK(queue.MarkResident(1));
    CHECK(queue.Find(1)->State == GeneratedTextureState::Resident);
    CHECK(queue.GetResidentCount() == 1u);
    CHECK(queue.GetPendingCount() == 0u);
    CHECK_FALSE(queue.MarkResident(1));
    CHECK_FALSE(queue.MarkResident(2));

    // No tick is spent on it, and no completion fires — its targets were filled elsewhere.
    Recorder recorder;
    CHECK(queue.Spend(GeneratedTextureQueue::UnlimitedCost, recorder.Tick(), recorder.Complete()) ==
          0u);
    CHECK(recorder.Log.empty());
}

TEST_CASE("GeneratedTextureQueue: at the default cost the budget is a tick count")
{
    // The behaviour-preserving property: a job left at the default cost of one spends the budget as
    // a plain tick count, the exact schedule the cost budget generalizes.
    GeneratedTextureQueue queue;
    queue.Add(1, 10, 0);

    Recorder first;
    CHECK(queue.Spend(4, first.Tick(), first.Complete()) == 4u);
    CHECK(first.Log == std::vector<std::string>{"1.0", "1.1", "1.2", "1.3"});

    Recorder second;
    CHECK(queue.Spend(4, second.Tick(), second.Complete()) == 4u);
    CHECK(second.Log == std::vector<std::string>{"1.4", "1.5", "1.6", "1.7"});
}

TEST_CASE("GeneratedTextureQueue: cost bounds a pump's summed tick cost")
{
    // Every individual tick fits the budget, so a pump's summed cost never exceeds it, however the
    // cheap and dear jobs are ordered — asserted over several budgets and drained across pumps.
    const std::map<u64, u32> cost{{1, 1}, {2, 3}, {3, 5}};
    for (const u32 budget : {5u, 6u, 8u, 11u, 20u})
    {
        GeneratedTextureQueue queue;
        queue.Add(1, 6, 0, cost.at(1));
        queue.Add(2, 6, 0, cost.at(2));
        queue.Add(3, 6, 0, cost.at(3));

        u32 pumps = 0;
        while (queue.GetPendingCount() > 0)
        {
            u32 pumpCost = 0;
            const u32 ticks = queue.Spend(
                budget, [&](const u64 key, u32, u32) { pumpCost += cost.at(key); }, [](u64) {});
            CHECK(pumpCost <= budget);
            CHECK(ticks >= 1u); // a positive budget covering every cost always makes progress
            REQUIRE(++pumps < 100u);
        }
    }
}

TEST_CASE("GeneratedTextureQueue: a dear job takes proportionally more frames than a cheap one")
{
    constexpr u32 Budget = 12;
    constexpr u32 Ticks = 24;
    constexpr u32 K = 4;

    const auto pumpsToFinish = [](const u32 cost)
    {
        GeneratedTextureQueue queue;
        queue.Add(1, Ticks, 0, cost);
        u32 pumps = 0;
        while (queue.GetPendingCount() > 0)
        {
            queue.Spend(Budget, [](u64, u32, u32) {}, [](u64) {});
            REQUIRE(++pumps < 1000u);
        }
        return pumps;
    };

    // A cost-K tick runs a Kth as many per frame as a cost-1 tick, so a job of the same length takes
    // K× the frames — the amortization the expensive sky backdrop needs.
    CHECK(pumpsToFinish(K) == pumpsToFinish(1) * K);
}

TEST_CASE("GeneratedTextureQueue: a tick dearer than the whole budget still runs one a pump")
{
    // The one-tick minimum: cost 10 exceeds the budget of 4, but the job still advances one tick per
    // pump rather than stalling forever.
    GeneratedTextureQueue queue;
    queue.Add(1, 3, 0, 10);

    Recorder first;
    CHECK(queue.Spend(4, first.Tick(), first.Complete()) == 1u);
    CHECK(first.Log == std::vector<std::string>{"1.0"});

    Recorder second;
    CHECK(queue.Spend(4, second.Tick(), second.Complete()) == 1u);
    CHECK(second.Log == std::vector<std::string>{"1.1"});

    Recorder third;
    CHECK(queue.Spend(4, third.Tick(), third.Complete()) == 1u);
    CHECK(third.Log == std::vector<std::string>{"1.2", "1!"});
}

TEST_CASE("GeneratedTextureQueue: cost does not change selection order")
{
    // Selection is priority then request order; a job's cost feeds the budget alone and never the
    // order. Under an unlimited budget the dearest and cheapest run in the same order as any pump.
    GeneratedTextureQueue queue;
    queue.Add(1, 1, 0, 5);
    queue.Add(2, 1, 5, 1);
    queue.Add(3, 1, 5, 9);
    queue.Add(4, 1, 0, 1);

    Recorder recorder;
    queue.Spend(GeneratedTextureQueue::UnlimitedCost, recorder.Tick(), recorder.Complete());
    CHECK(recorder.Log ==
          std::vector<std::string>{"2.0", "2!", "3.0", "3!", "1.0", "1!", "4.0", "4!"});
}
