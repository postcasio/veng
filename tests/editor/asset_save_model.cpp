#include <doctest/doctest.h>

#include <VengEditor/AssetSaveModel.h>

#include <Veng/Result.h>
#include <Veng/Veng.h>

using namespace Veng;
using namespace VengEditor;

TEST_CASE("SaveAssetSource writes, clears dirty, then cooks — in that order")
{
    vector<string> order;
    bool dirty = true;

    const VoidResult saved = SaveAssetSource(
        [&]() -> VoidResult
        {
            order.emplace_back("write");
            // The cook reads the file the write produced, so nothing may cook ahead of it.
            CHECK(dirty);
            return {};
        },
        dirty,
        [&]
        {
            order.emplace_back("cook");
            CHECK_FALSE(dirty);
        });

    CHECK(saved.has_value());
    CHECK_FALSE(dirty);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "write");
    CHECK(order[1] == "cook");
}

TEST_CASE("a failed write keeps the edits and cooks nothing")
{
    bool dirty = true;
    u32 cooks = 0;

    const VoidResult saved = SaveAssetSource(
        [] { return VoidResult{std::unexpected(string{"disk full"})}; }, dirty, [&] { ++cooks; });

    REQUIRE_FALSE(saved.has_value());
    CHECK(saved.error() == "disk full");
    // The editor still holds what it could not persist, and the mounted asset still matches the
    // file — a cook here would have replaced it with the unedited source.
    CHECK(dirty);
    CHECK(cooks == 0);
}

TEST_CASE("CookGate serialises recooks rather than dropping them")
{
    CookGate gate;

    SUBCASE("a request with nothing in flight submits immediately")
    {
        u32 submits = 0;
        gate.Request([&] { ++submits; });
        CHECK(submits == 1);
        CHECK(gate.IsCooking());

        gate.Complete();
        CHECK_FALSE(gate.IsCooking());
    }

    SUBCASE("a request behind one in flight is held, then run on Complete")
    {
        u32 first = 0;
        u32 second = 0;
        gate.Request([&] { ++first; });
        gate.Request([&] { ++second; });

        // The in-flight cook read an older source; the second must still run once it lands.
        CHECK(first == 1);
        CHECK(second == 0);

        gate.Complete();
        CHECK(second == 1);
        CHECK(gate.IsCooking());

        gate.Complete();
        CHECK_FALSE(gate.IsCooking());
    }

    SUBCASE("only the most recent queued request survives")
    {
        u32 superseded = 0;
        u32 latest = 0;
        gate.Request([] {});
        gate.Request([&] { ++superseded; });
        gate.Request([&] { ++latest; });

        gate.Complete();
        CHECK(superseded == 0);
        CHECK(latest == 1);
    }

    SUBCASE("a submit that queues behind the cook it starts is not re-run by its predecessor")
    {
        u32 runs = 0;
        u32 followUps = 0;
        gate.Request(
            [&]
            {
                ++runs;
                gate.Request([&] { ++followUps; });
            });
        CHECK(runs == 1);

        gate.Complete();
        CHECK(runs == 1);
        CHECK(followUps == 1);
    }
}
