// Device-free cases for StatusTracker: the in-flight-task bookkeeping the editor's
// status bar reads. Checks the active set, the wave-scoped completed/total counters,
// and the wave reset when the tracker returns to idle.

#include <doctest/doctest.h>

#include "StatusTracker.h"

using namespace VengEditor;

TEST_CASE("StatusTracker is idle until a task begins")
{
    const StatusTracker tracker;
    const StatusTracker::Snapshot idle = tracker.GetSnapshot();
    CHECK(idle.Tasks.empty());
    CHECK(idle.TotalInWave == 0);
    CHECK(idle.CompletedInWave == 0);
}

TEST_CASE("A single task reports its description and one-of-one wave")
{
    StatusTracker tracker;
    const StatusTracker::TaskId id = tracker.Begin("Cooking brick.tex.json");

    const StatusTracker::Snapshot running = tracker.GetSnapshot();
    REQUIRE(running.Tasks.size() == 1);
    CHECK(running.TotalInWave == 1);
    CHECK(running.CompletedInWave == 0);
    CHECK(running.Tasks.front() == "Cooking brick.tex.json");

    tracker.End(id);
    CHECK(tracker.GetSnapshot().Tasks.empty());
}

TEST_CASE("Overlapping tasks accumulate the wave's completed/total counts")
{
    StatusTracker tracker;
    const StatusTracker::TaskId a = tracker.Begin("a");
    const StatusTracker::TaskId b = tracker.Begin("b");
    const StatusTracker::TaskId c = tracker.Begin("c");

    StatusTracker::Snapshot snapshot = tracker.GetSnapshot();
    // Every running task is listed, oldest first — the set the expanded view draws.
    CHECK(snapshot.Tasks == Veng::vector<Veng::string>{"a", "b", "c"});
    CHECK(snapshot.TotalInWave == 3);
    CHECK(snapshot.CompletedInWave == 0);

    tracker.End(a);
    snapshot = tracker.GetSnapshot();
    CHECK(snapshot.Tasks == Veng::vector<Veng::string>{"b", "c"});
    CHECK(snapshot.TotalInWave == 3);
    CHECK(snapshot.CompletedInWave == 1);

    tracker.End(b);
    tracker.End(c);
    CHECK(tracker.GetSnapshot().Tasks.empty());
}

TEST_CASE("A new wave resets the counters once the tracker has gone idle")
{
    StatusTracker tracker;
    tracker.End(tracker.Begin("first"));

    // The wave's counters persist while idle (1 of 1 completed), then reset on the next Begin.
    const StatusTracker::TaskId second = tracker.Begin("second");
    const StatusTracker::Snapshot snapshot = tracker.GetSnapshot();
    REQUIRE(snapshot.Tasks.size() == 1);
    CHECK(snapshot.TotalInWave == 1);
    CHECK(snapshot.CompletedInWave == 0);
    tracker.End(second);
}

TEST_CASE("Ending an unknown or already-ended id is ignored")
{
    StatusTracker tracker;
    const StatusTracker::TaskId id = tracker.Begin("task");
    tracker.End(id);
    tracker.End(id);                          // already ended
    tracker.End(StatusTracker::TaskId{9999}); // never begun

    const StatusTracker::Snapshot snapshot = tracker.GetSnapshot();
    CHECK(snapshot.Tasks.empty());
    CHECK(snapshot.CompletedInWave == 1);
}
