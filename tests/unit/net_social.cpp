// Social-toolkit cases. The four pieces are pure value logic with no socket, scene, or clock:
// InviteTable holds one-shot capability tokens keyed on (world, invitee) with monotonic-seconds
// expiry, JudgeInviteGatedJoin decides one admission without consuming anything,
// ClassifyMemberPresence separates a disconnect from a leave, and DeriveRosterNotices diffs two
// roster snapshots into ordered notices. These pin the token discipline (including that a wrong
// guess never burns a live invite and that a capacity refusal leaves the token retryable), the full
// four-input presence matrix, and every roster-diff event under both the default change test and a
// consumer-supplied one.

#include <doctest/doctest.h>

#include <Veng/Net/Social.h>

#include <span>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    constexpr WorldKey WorldA = WorldKey{.Lo = 11};
    constexpr WorldKey WorldB = WorldKey{.Lo = 22};
    constexpr AccountId Alice = AccountId{.Lo = 1};
    constexpr AccountId Bob = AccountId{.Lo = 2};

    constexpr f64 Expiry = 10.0;

    // A comparable row payload — what the default change test folds over.
    struct Row
    {
        int Score = 0;
        bool operator==(const Row&) const = default;
    };

    // A payload with no equality at all: the diff must still compile and simply never fire Changed
    // when the caller supplies no predicate.
    struct OpaqueRow
    {
        int Score = 0;
    };

    using Entry = RosterEntry<int, Row>;
    using Notice = RosterNotice<int, Row>;

    vector<Notice> Diff(const vector<Entry>& before, const vector<Entry>& after,
                        const function<bool(const Row&, const Row&)>& changed = {})
    {
        return DeriveRosterNotices(std::span<const Entry>(before), std::span<const Entry>(after),
                                   changed);
    }
}

TEST_CASE("invite table: a token consumes exactly once")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);
    CHECK(table.GetCount() == 1);
    CHECK(table.IsInvited(WorldA, Alice, 1.0, Expiry));

    CHECK(table.Consume(WorldA, Alice, 0xABCD, 1.0, Expiry));
    CHECK(table.GetCount() == 0);

    // The capability is spent: the same token never admits again.
    CHECK_FALSE(table.Consume(WorldA, Alice, 0xABCD, 1.0, Expiry));
    CHECK_FALSE(table.IsInvited(WorldA, Alice, 1.0, Expiry));
}

TEST_CASE("invite table: the addressing pair scopes the invite")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);

    // Neither the other world nor the other account holds this invite.
    CHECK_FALSE(table.IsInvited(WorldB, Alice, 1.0, Expiry));
    CHECK_FALSE(table.IsInvited(WorldA, Bob, 1.0, Expiry));
    CHECK_FALSE(table.Consume(WorldB, Alice, 0xABCD, 1.0, Expiry));
    CHECK_FALSE(table.Consume(WorldA, Bob, 0xABCD, 1.0, Expiry));
    CHECK(table.GetCount() == 1);
}

TEST_CASE("invite table: a wrong token fails and the entry survives")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);

    CHECK_FALSE(table.Consume(WorldA, Alice, 0x1234, 1.0, Expiry));
    // The real invite is untouched — a guess must not burn what the holder still needs.
    CHECK(table.GetCount() == 1);
    CHECK(table.IsInvited(WorldA, Alice, 1.0, Expiry));
    CHECK(table.Consume(WorldA, Alice, 0xABCD, 1.0, Expiry));
}

TEST_CASE("invite table: re-issuing replaces the pair's token")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);
    table.Issue(WorldA, Alice, 0x5678, 1.0);

    // One entry per pair, and only the latest token is outstanding.
    CHECK(table.GetCount() == 1);
    CHECK_FALSE(table.Consume(WorldA, Alice, 0xABCD, 2.0, Expiry));
    CHECK(table.Consume(WorldA, Alice, 0x5678, 2.0, Expiry));
}

TEST_CASE("invite table: re-issuing restarts the expiry window")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);
    table.Issue(WorldA, Alice, 0x5678, 9.0);

    CHECK(table.IsInvited(WorldA, Alice, 18.0, Expiry));
    CHECK(table.Consume(WorldA, Alice, 0x5678, 18.0, Expiry));
}

TEST_CASE("invite table: an entry is live until its window closes")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);

    CHECK(table.IsInvited(WorldA, Alice, 9.999, Expiry));
    CHECK_FALSE(table.IsInvited(WorldA, Alice, 10.0, Expiry));
    // A const query reports it absent without removing it.
    CHECK(table.GetCount() == 1);

    // A consume landing on the expired entry fails and drops it.
    CHECK_FALSE(table.Consume(WorldA, Alice, 0xABCD, 10.0, Expiry));
    CHECK(table.GetCount() == 0);
}

TEST_CASE("invite table: sweeping drops only the expired entries")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);
    table.Issue(WorldA, Bob, 0x5678, 8.0);

    table.SweepExpired(12.0, Expiry);
    CHECK(table.GetCount() == 1);
    CHECK_FALSE(table.IsInvited(WorldA, Alice, 12.0, Expiry));
    CHECK(table.IsInvited(WorldA, Bob, 12.0, Expiry));

    table.SweepExpired(20.0, Expiry);
    CHECK(table.GetCount() == 0);
}

TEST_CASE("invite table: clearing a world spares the others")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);
    table.Issue(WorldA, Bob, 0x5678, 0.0);
    table.Issue(WorldB, Alice, 0x9ABC, 0.0);

    table.ClearFor(WorldA);
    CHECK(table.GetCount() == 1);
    CHECK(table.IsInvited(WorldB, Alice, 1.0, Expiry));
    CHECK_FALSE(table.IsInvited(WorldA, Alice, 1.0, Expiry));
}

TEST_CASE("join judge: a roster member readmits, capacity notwithstanding")
{
    CHECK(JudgeInviteGatedJoin(true, false, 4, 4) == JoinVerdict::AdmitMember);
    CHECK(JudgeInviteGatedJoin(true, true, 4, 4) == JoinVerdict::AdmitMember);
    // The reattach case: a member returning to an over-full roster is not competing for a seat.
    CHECK(JudgeInviteGatedJoin(true, false, 9, 4) == JoinVerdict::AdmitMember);
    CHECK(JudgeInviteGatedJoin(true, false, 0, 0) == JoinVerdict::AdmitMember);
}

TEST_CASE("join judge: an invite admits while a seat remains")
{
    CHECK(JudgeInviteGatedJoin(false, true, 0, 4) == JoinVerdict::AdmitByInvite);
    CHECK(JudgeInviteGatedJoin(false, true, 3, 4) == JoinVerdict::AdmitByInvite);
    CHECK(JudgeInviteGatedJoin(false, true, 4, 4) == JoinVerdict::RefuseFull);
    CHECK(JudgeInviteGatedJoin(false, true, 5, 4) == JoinVerdict::RefuseFull);
    CHECK(JudgeInviteGatedJoin(false, true, 0, 0) == JoinVerdict::RefuseFull);
}

TEST_CASE("join judge: an uninvited joiner refuses as uninvited, full or not")
{
    CHECK(JudgeInviteGatedJoin(false, false, 0, 4) == JoinVerdict::RefuseUninvited);
    // Occupancy is never disclosed to someone with no invite at all.
    CHECK(JudgeInviteGatedJoin(false, false, 4, 4) == JoinVerdict::RefuseUninvited);
}

TEST_CASE("join judge: a capacity refusal leaves the token live for a retry")
{
    InviteTable table;
    table.Issue(WorldA, Alice, 0xABCD, 0.0);

    // The judge is pure: judging a full roster consumes nothing, so the one-shot token survives.
    const bool invited = table.IsInvited(WorldA, Alice, 1.0, Expiry);
    CHECK(JudgeInviteGatedJoin(false, invited, 4, 4) == JoinVerdict::RefuseFull);
    CHECK(table.GetCount() == 1);

    // A seat frees and the same token now admits.
    CHECK(JudgeInviteGatedJoin(false, table.IsInvited(WorldA, Alice, 2.0, Expiry), 3, 4) ==
          JoinVerdict::AdmitByInvite);
    CHECK(table.Consume(WorldA, Alice, 0xABCD, 2.0, Expiry));
    CHECK(table.GetCount() == 0);
}

TEST_CASE("presence classifier: the full four-input transition matrix")
{
    // Index bits: inRoster, online, present, connected.
    constexpr PresenceTransition Expected[16] = {/* F F F F */ PresenceTransition::None,
                                                 /* F F F T */ PresenceTransition::None,
                                                 /* F F T F */ PresenceTransition::None,
                                                 /* F F T T */ PresenceTransition::Join,
                                                 /* F T F F */ PresenceTransition::None,
                                                 /* F T F T */ PresenceTransition::None,
                                                 /* F T T F */ PresenceTransition::None,
                                                 /* F T T T */ PresenceTransition::Join,
                                                 /* T F F F */ PresenceTransition::None,
                                                 /* T F F T */ PresenceTransition::None,
                                                 /* T F T F */ PresenceTransition::None,
                                                 /* T F T T */ PresenceTransition::Rejoin,
                                                 /* T T F F */ PresenceTransition::Offline,
                                                 /* T T F T */ PresenceTransition::None,
                                                 /* T T T F */ PresenceTransition::Offline,
                                                 /* T T T T */ PresenceTransition::None};

    for (int bits = 0; bits < 16; ++bits)
    {
        const bool inRoster = (bits & 0b1000) != 0;
        const bool online = (bits & 0b0100) != 0;
        const bool present = (bits & 0b0010) != 0;
        const bool connected = (bits & 0b0001) != 0;
        CAPTURE(bits);
        CHECK(ClassifyMemberPresence(inRoster, online, present, connected) == Expected[bits]);
    }
}

TEST_CASE("presence classifier: a disconnect is not a leave")
{
    // Losing the connection marks the standing member offline exactly once...
    CHECK(ClassifyMemberPresence(true, true, true, false) == PresenceTransition::Offline);
    // ...and every subsequent sweep over the already-offline member is quiet.
    CHECK(ClassifyMemberPresence(true, false, true, false) == PresenceTransition::None);
    // The member comes back as a rejoin, not a fresh join.
    CHECK(ClassifyMemberPresence(true, false, true, true) == PresenceTransition::Rejoin);
}

TEST_CASE("roster diff: an empty snapshot to a populated one is all joins")
{
    const vector<Entry> after{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 5}},
                              Entry{.Id = 2, .Online = false, .Data = Row{.Score = 7}}};

    const vector<Notice> notices = Diff({}, after);
    REQUIRE(notices.size() == 2);
    CHECK(notices[0].Event == RosterEvent::Joined);
    CHECK(notices[0].Id == 1);
    CHECK(notices[0].Data.Score == 5);
    CHECK(notices[1].Event == RosterEvent::Joined);
    CHECK(notices[1].Id == 2);
}

TEST_CASE("roster diff: a populated snapshot to an empty one is all leaves")
{
    const vector<Entry> before{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 5}},
                               Entry{.Id = 2, .Online = true, .Data = Row{.Score = 7}}};

    const vector<Notice> notices = Diff(before, {});
    REQUIRE(notices.size() == 2);
    CHECK(notices[0].Event == RosterEvent::Left);
    CHECK(notices[0].Id == 1);
    // A leave carries the last data known for the member.
    CHECK(notices[0].Data.Score == 5);
    CHECK(notices[1].Event == RosterEvent::Left);
    CHECK(notices[1].Id == 2);
}

TEST_CASE("roster diff: an unchanged snapshot pair yields nothing")
{
    const vector<Entry> rows{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 5}},
                             Entry{.Id = 2, .Online = false, .Data = Row{.Score = 7}}};

    CHECK(Diff(rows, rows).empty());
}

TEST_CASE("roster diff: the online flag drives the presence notices")
{
    const vector<Entry> before{Entry{.Id = 1, .Online = false, .Data = Row{.Score = 5}},
                               Entry{.Id = 2, .Online = true, .Data = Row{.Score = 7}}};
    const vector<Entry> after{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 5}},
                              Entry{.Id = 2, .Online = false, .Data = Row{.Score = 7}}};

    const vector<Notice> notices = Diff(before, after);
    REQUIRE(notices.size() == 2);
    CHECK(notices[0].Event == RosterEvent::CameOnline);
    CHECK(notices[0].Id == 1);
    CHECK(notices[1].Event == RosterEvent::WentOffline);
    CHECK(notices[1].Id == 2);
}

TEST_CASE("roster diff: the default change test is payload inequality")
{
    const vector<Entry> before{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 5}}};
    const vector<Entry> after{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 6}}};

    const vector<Notice> notices = Diff(before, after);
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].Event == RosterEvent::Changed);
    CHECK(notices[0].Data.Score == 6);
}

TEST_CASE("roster diff: a supplied predicate replaces the default change test")
{
    const vector<Entry> before{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 5}},
                               Entry{.Id = 2, .Online = true, .Data = Row{.Score = 5}}};
    const vector<Entry> after{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 0}},
                              Entry{.Id = 2, .Online = true, .Data = Row{.Score = 9}}};

    // A consumer that treats a zeroed score as "no longer known" rather than a change.
    const auto changed = [](const Row& lhs, const Row& rhs)
    { return rhs.Score != 0 && lhs.Score != rhs.Score; };

    const vector<Notice> notices = Diff(before, after, changed);
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].Event == RosterEvent::Changed);
    CHECK(notices[0].Id == 2);
}

TEST_CASE("roster diff: a member yields both a presence notice and a change in one diff")
{
    const vector<Entry> before{Entry{.Id = 1, .Online = false, .Data = Row{.Score = 5}}};
    const vector<Entry> after{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 6}}};

    const vector<Notice> notices = Diff(before, after);
    REQUIRE(notices.size() == 2);
    CHECK(notices[0].Event == RosterEvent::CameOnline);
    CHECK(notices[1].Event == RosterEvent::Changed);
    CHECK(notices[1].Id == 1);
}

TEST_CASE("roster diff: joins, leaves, and updates order after-first then the departures")
{
    const vector<Entry> before{Entry{.Id = 1, .Online = true, .Data = Row{.Score = 5}},
                               Entry{.Id = 2, .Online = true, .Data = Row{.Score = 7}}};
    const vector<Entry> after{Entry{.Id = 2, .Online = false, .Data = Row{.Score = 7}},
                              Entry{.Id = 3, .Online = true, .Data = Row{.Score = 9}}};

    const vector<Notice> notices = Diff(before, after);
    REQUIRE(notices.size() == 3);
    CHECK(notices[0].Event == RosterEvent::WentOffline);
    CHECK(notices[0].Id == 2);
    CHECK(notices[1].Event == RosterEvent::Joined);
    CHECK(notices[1].Id == 3);
    CHECK(notices[2].Event == RosterEvent::Left);
    CHECK(notices[2].Id == 1);
}

TEST_CASE("roster diff: the match key is the consumer's, not an account id")
{
    using NameEntry = RosterEntry<string, Row>;
    const vector<NameEntry> before{
        NameEntry{.Id = "north", .Online = true, .Data = Row{.Score = 1}}};
    const vector<NameEntry> after{
        NameEntry{.Id = "south", .Online = true, .Data = Row{.Score = 1}}};

    const vector<RosterNotice<string, Row>> notices =
        DeriveRosterNotices(std::span<const NameEntry>(before), std::span<const NameEntry>(after));
    REQUIRE(notices.size() == 2);
    CHECK(notices[0].Event == RosterEvent::Joined);
    CHECK(notices[0].Id == "south");
    CHECK(notices[1].Event == RosterEvent::Left);
    CHECK(notices[1].Id == "north");
}

TEST_CASE("roster diff: a payload with no equality never fires a change on its own")
{
    using OpaqueEntry = RosterEntry<int, OpaqueRow>;
    const vector<OpaqueEntry> before{
        OpaqueEntry{.Id = 1, .Online = true, .Data = OpaqueRow{.Score = 5}}};
    const vector<OpaqueEntry> after{
        OpaqueEntry{.Id = 1, .Online = true, .Data = OpaqueRow{.Score = 6}}};

    CHECK(DeriveRosterNotices(std::span<const OpaqueEntry>(before),
                              std::span<const OpaqueEntry>(after))
              .empty());

    // The same pair fires once the consumer says what "changed" means for its payload.
    const function<bool(const OpaqueRow&, const OpaqueRow&)> changed =
        [](const OpaqueRow& lhs, const OpaqueRow& rhs) { return lhs.Score != rhs.Score; };
    const vector<RosterNotice<int, OpaqueRow>> notices = DeriveRosterNotices(
        std::span<const OpaqueEntry>(before), std::span<const OpaqueEntry>(after), changed);
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].Event == RosterEvent::Changed);
}
