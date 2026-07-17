// WorldDirectory: the role-neutral get-or-place directory. These cases exercise the two
// consumer-facing surfaces this suite adds — the requester-aware WorldFactory (a factory reads the
// resolving request's account) and the accountless warm pin (the mechanism Application::HoldWorldWarm
// delegates to: an infrastructure pin that composes with the presence refcount). Pure policy —
// device-free: the directory never dereferences a bucket's Scene, so a bare Scene stands in for one.

#include <doctest/doctest.h>

#include <Veng/Net/AccountId.h>
#include <Veng/Net/Blob.h>
#include <Veng/Net/JoinRequest.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/WorldDirectory.h>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A directory over a factory that opens one fresh bucket per miss, recording the account of the
    // request that drove each open — so a case can assert who the factory saw. The dwell is short and
    // reaps route through the CloseWorld hook (no runner), which records the reaped ids.
    struct DirectoryFixture
    {
        TypeRegistry Types;
        Unique<Scene> WorldScene = Scene::Create(Types);

        // The account each factory-open saw (its resolving request's account), in open order.
        vector<AccountId> OpenAccounts;
        // The ids CloseWorld reaped, in reap order.
        vector<u64> Closed;
        u64 NextWorld = 1;

        Unique<WorldDirectory> Directory;

        explicit DirectoryFixture(f64 dwell = 5.0)
        {
            Directory = WorldDirectory::Create(WorldDirectoryInfo{
                .IdleKeepWarmDwell = dwell,
                .WorldFactory = [this](const JoinRequestInfo& request, const WorldKey&,
                                       const Blob&) -> optional<ServerWorldResolution>
                {
                    OpenAccounts.push_back(request.Account);
                    return ServerWorldResolution{.WorldId = WorldInstanceId{.Value = NextWorld++},
                                                 .World = WorldScene.get()};
                },
                .CloseWorld = [this](WorldInstanceId id) { Closed.push_back(id.Value); },
            });
        }

        // Resolves a first-time key, driving the factory. The payload outlives the synchronous call.
        WorldResolveResult Open(const WorldKey& key, const AccountId& account)
        {
            const Blob payload;
            return Directory->Resolve(JoinRequestInfo{.Connection = ConnectionId{},
                                                      .Account = account,
                                                      .Key = key,
                                                      .Payload = payload},
                                      /*heldWorlds=*/0);
        }
    };
}

TEST_CASE("The WorldFactory receives the resolving request's account")
{
    DirectoryFixture fx;
    const WorldKey key = WorldKey::FromU64(0xA11CE);
    const AccountId joiner{.Lo = 0x1234, .Hi = 0x5678};

    const WorldResolveResult resolve = fx.Open(key, joiner);

    REQUIRE(resolve.Outcome == WorldResolveOutcome::Opened);
    REQUIRE(fx.OpenAccounts.size() == 1);
    // The factory can project per-account state at world open: it sees exactly who asked.
    CHECK(fx.OpenAccounts[0] == joiner);
    CHECK(fx.OpenAccounts[0].IsValid());
}

TEST_CASE("A requester-less resolve hands the factory the invalid account")
{
    DirectoryFixture fx;
    const WorldKey key = WorldKey::FromU64(0xDA7A);

    // The warm-pre-open contract: a resolve driven by no particular join carries the invalid account.
    const WorldResolveResult resolve = fx.Open(key, AccountId{});

    REQUIRE(resolve.Outcome == WorldResolveOutcome::Opened);
    REQUIRE(fx.OpenAccounts.size() == 1);
    CHECK_FALSE(fx.OpenAccounts[0].IsValid());
}

TEST_CASE("An accountless warm pin holds a factory-opened world past its dwell")
{
    DirectoryFixture fx(/*dwell=*/5.0);
    const WorldKey key = WorldKey::FromU64(0x1);
    const WorldResolveResult resolve = fx.Open(key, AccountId{});
    REQUIRE(resolve.Outcome == WorldResolveOutcome::Opened);

    // The accountless pin: presence with no recorded member (MembersOf reports nobody).
    fx.Directory->Pin(resolve.World);
    CHECK(fx.Directory->PresenceOf(resolve.World) == 1);
    CHECK(fx.Directory->MembersOf(key).empty());

    // Far past the dwell, a pinned world never reaps — presence keeps it warm.
    const vector<WorldInstanceId> reaped = fx.Directory->ReapIdle(/*now=*/1000.0);
    CHECK(reaped.empty());
    CHECK(fx.Directory->Contains(resolve.World));
    CHECK(fx.Closed.empty());
}

TEST_CASE("Releasing a warm pin lets the world reap after the dwell")
{
    DirectoryFixture fx(/*dwell=*/5.0);
    const WorldKey key = WorldKey::FromU64(0x2);
    const WorldResolveResult resolve = fx.Open(key, AccountId{});
    REQUIRE(resolve.Outcome == WorldResolveOutcome::Opened);

    fx.Directory->Pin(resolve.World);
    // Release the pin: presence reaches zero and the dwell starts at now=10.
    fx.Directory->Unpin(resolve.World, /*now=*/10.0);
    CHECK(fx.Directory->PresenceOf(resolve.World) == 0);

    // Within the dwell it is still held warm.
    CHECK(fx.Directory->ReapIdle(/*now=*/12.0).empty());
    CHECK(fx.Directory->Contains(resolve.World));

    // Past the dwell it reaps through the CloseWorld hook.
    const vector<WorldInstanceId> reaped = fx.Directory->ReapIdle(/*now=*/16.0);
    REQUIRE(reaped.size() == 1);
    CHECK(reaped[0] == resolve.World);
    CHECK_FALSE(fx.Directory->Contains(resolve.World));
    REQUIRE(fx.Closed.size() == 1);
    CHECK(fx.Closed[0] == resolve.World.Value);
}

TEST_CASE("A world with both a warm pin and a join survives until both are gone")
{
    DirectoryFixture fx(/*dwell=*/5.0);
    const WorldKey key = WorldKey::FromU64(0x3);
    const AccountId joiner{.Lo = 0xABC, .Hi = 0};
    const WorldResolveResult resolve = fx.Open(key, joiner);
    REQUIRE(resolve.Outcome == WorldResolveOutcome::Opened);

    // An accountless warm pin plus a live account join: presence is two, the account is a member.
    fx.Directory->Pin(resolve.World);
    fx.Directory->AddJoin(resolve.World, joiner);
    CHECK(fx.Directory->PresenceOf(resolve.World) == 2);
    CHECK(fx.Directory->MembersOf(key).size() == 1);

    // Drop the join: the warm pin still holds it, so it never reaps however far past the dwell.
    fx.Directory->RemoveJoin(resolve.World, /*now=*/10.0, joiner);
    CHECK(fx.Directory->PresenceOf(resolve.World) == 1);
    CHECK(fx.Directory->ReapIdle(/*now=*/1000.0).empty());
    CHECK(fx.Directory->Contains(resolve.World));

    // Drop the warm pin too: only now, with every presence gone, does the dwell own its fate.
    fx.Directory->Unpin(resolve.World, /*now=*/1000.0);
    CHECK(fx.Directory->PresenceOf(resolve.World) == 0);
    CHECK(fx.Directory->ReapIdle(/*now=*/1002.0).empty());
    const vector<WorldInstanceId> reaped = fx.Directory->ReapIdle(/*now=*/1006.0);
    REQUIRE(reaped.size() == 1);
    CHECK(reaped[0] == resolve.World);
}
