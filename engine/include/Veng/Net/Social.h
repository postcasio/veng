#pragma once

#include <Veng/Net/AccountId.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Veng.h>

#include <algorithm>
#include <span>

// Veng/Net/Social.h — the multiplayer-social vocabulary: invite tokens, an admission judge, and a
// roster diff.
//
// Every multiplayer system with invite-gated shared worlds re-derives the same three pieces, so
// they live here as pure value logic over engine types (WorldKey, AccountId, monotonic seconds).
// This is vocabulary, not a feature: the engine ships no roster component, no membership policy,
// no caps, and no invite flow — a consumer composes these against whatever membership model it
// defines, and no engine system calls them.
//
// The pieces are deliberately pure. InviteTable owns storage and nothing else; the judge is a free
// function over booleans and counts; the roster diff reads two snapshots and writes notices.
// Nothing here touches a socket, a scene, a clock, or a connection, so all of it is unit-testable
// end to end and compiles under include_hygiene.

namespace Veng::Net
{
    /// @brief One-shot capability tokens keyed on (world, invitee), with monotonic-seconds expiry.
    ///
    /// An invite is a secret the issuer mints and delivers out of band; presenting it is what grants
    /// admission. The (world, invitee) pair is only the addressing key — the token is the capability,
    /// and Consume succeeds exactly once per issued token. Time is monotonic seconds: an entry is
    /// live while now - issue < expirySeconds, so expiry never moves under an NTP step or a user
    /// clock change. Expired entries are dropped on demand — by SweepExpired, or by a Consume that
    /// lands on one; a const query reports an expired entry as absent without removing it.
    ///
    /// @warning AccountId is unauthenticated: whoever presents an account id is that account. The
    ///          token is therefore what stands between an invite and anyone able to name an invitee,
    ///          so admitting on the pair alone degrades the table to a spoofable allowlist.
    class VE_API InviteTable
    {
    public:
        /// @brief Issues a token for the pair, replacing any token the pair already holds.
        ///
        /// Re-issuing invalidates the prior token: exactly one token is outstanding per pair, and
        /// the expiry window restarts from this issue time.
        /// @param world     The world the invite admits to.
        /// @param invitee   The account the invite addresses.
        /// @param token     The minted secret the invitee must present back.
        /// @param now       Monotonic seconds at issue.
        void Issue(const WorldKey& world, AccountId invitee, u64 token, f64 now);

        /// @brief Consumes the pair's token when the presented value matches and has not expired.
        ///
        /// Succeeds at most once per issued token: a matching, live entry is removed. A mismatched
        /// token fails and leaves the entry intact, so a wrong guess never burns the real invite. An
        /// expired entry fails and is dropped.
        /// @param world           The world the invite admits to.
        /// @param invitee         The account presenting the token.
        /// @param token           The presented secret.
        /// @param now             Monotonic seconds at presentation.
        /// @param expirySeconds   Lifetime of an entry, in seconds since its issue.
        /// @return True when the token matched a live entry and was consumed.
        bool Consume(const WorldKey& world, AccountId invitee, u64 token, f64 now,
                     f64 expirySeconds);

        /// @brief Returns whether the pair holds a live (unexpired) invite.
        ///
        /// A query over the addressing key alone, so it answers "was this account invited" for a
        /// listing or a UI gate. It is not an admission check — admission presents the token
        /// through Consume.
        /// @param world           The world the invite admits to.
        /// @param invitee         The account being asked about.
        /// @param now             Monotonic seconds at the query.
        /// @param expirySeconds   Lifetime of an entry, in seconds since its issue.
        /// @return True when a matching entry exists and has not expired.
        [[nodiscard]] bool IsInvited(const WorldKey& world, AccountId invitee, f64 now,
                                     f64 expirySeconds) const;

        /// @brief Drops every entry whose lifetime has elapsed.
        /// @param now             Monotonic seconds at the sweep.
        /// @param expirySeconds   Lifetime of an entry, in seconds since its issue.
        void SweepExpired(f64 now, f64 expirySeconds);

        /// @brief Drops every entry addressed to one world, live or expired.
        /// @param world  The world whose invites are discarded.
        void ClearFor(const WorldKey& world);

        /// @brief Returns the number of entries held, counting entries no sweep has dropped yet.
        [[nodiscard]] usize GetCount() const;

    private:
        /// @brief One outstanding invite: its addressing pair, its secret, and its issue time.
        struct Entry
        {
            /// @brief The world the invite admits to.
            WorldKey World;
            /// @brief The account the invite addresses.
            AccountId Invitee;
            /// @brief The minted secret the invitee presents back.
            u64 Token = 0;
            /// @brief Monotonic seconds the entry was issued at.
            f64 IssuedAt = 0.0;
        };

        /// @brief The outstanding invites, at most one per (world, invitee) pair.
        vector<Entry> m_Entries;
    };

    /// @brief The outcome of judging one join against a roster and an invite.
    enum class JoinVerdict
    {
        /// @brief The joiner is already a roster member and readmits regardless of capacity.
        AdmitMember,
        /// @brief The joiner presented a valid invite and a seat is free.
        AdmitByInvite,
        /// @brief The joiner is invited but the roster is at capacity.
        RefuseFull,
        /// @brief The joiner is neither a roster member nor invited.
        RefuseUninvited
    };

    /// @brief Judges one invite-gated join: reattach, admit by invite, or refuse by reason.
    ///
    /// The admission rule for a shared world whose membership is invite-gated. A roster member
    /// always readmits — a member reattaching after a disconnect is not competing for a seat, so
    /// capacity is not consulted for one. A non-member is judged on its invite first and its seat
    /// second, so an uninvited joiner is refused as uninvited whether or not the roster is full and
    /// learns nothing about occupancy.
    ///
    /// Pure: it reads the invite's validity and never consumes it. The caller consumes the token
    /// ONLY on AdmitByInvite. Capacity is judged before any consume, so a RefuseFull invitee's
    /// one-shot token survives for a retry once a seat frees; consuming before judging silently
    /// burns it.
    /// @param inRoster      Whether the joiner is already a roster member.
    /// @param hasInvite     Whether the joiner holds a valid, unexpired invite.
    /// @param rosterCount   Current roster size.
    /// @param capacity      Maximum roster size.
    /// @return The verdict; the caller consumes the invite only on AdmitByInvite.
    [[nodiscard]] VE_API JoinVerdict JudgeInviteGatedJoin(bool inRoster, bool hasInvite,
                                                          usize rosterCount, usize capacity);

    /// @brief One row of a replicated roster snapshot: its match key, its online flag, its payload.
    ///
    /// The row shape DeriveRosterNotices diffs. Key is whatever identity the consumer's snapshot
    /// actually carries — a display identity, an account id, an entity handle — because a
    /// replicated roster commonly carries no account id by design. Payload is the consumer's row
    /// data, uninterpreted here beyond the change test.
    /// @tparam Key      The identity rows match on; requires equality comparison.
    /// @tparam Payload  The consumer's per-row data.
    template <typename Key, typename Payload>
    struct RosterEntry
    {
        /// @brief The identity this row matches on across snapshots.
        Key Id{};
        /// @brief Whether the member is online in this snapshot.
        bool Online = false;
        /// @brief The consumer's row data for this member.
        Payload Data{};
    };

    /// @brief The kind of change one roster notice reports.
    enum class RosterEvent
    {
        /// @brief The member appears in the later snapshot and not the earlier one.
        Joined,
        /// @brief The member appears in the earlier snapshot and not the later one.
        Left,
        /// @brief The member's online flag rose between the snapshots.
        CameOnline,
        /// @brief The member's online flag fell between the snapshots.
        WentOffline,
        /// @brief The member's payload differs between the snapshots.
        Changed
    };

    /// @brief One derived roster change: what happened, to whom, with the relevant row data.
    /// @tparam Key      The identity rows match on.
    /// @tparam Payload  The consumer's per-row data.
    template <typename Key, typename Payload>
    struct RosterNotice
    {
        /// @brief What changed for this member.
        RosterEvent Event;
        /// @brief The member the notice concerns.
        Key Id{};
        /// @brief The later snapshot's row data, or the earlier one's for a Left notice.
        Payload Data{};
    };

    /// @brief Diffs two roster snapshots into ordered notices, deriving events from state alone.
    ///
    /// The client-side pattern for a roster that replicates as ordinary state: rather than sending
    /// a message per membership change, a consumer keeps the previous snapshot and derives what
    /// changed. Rows match on Key. A member present in both snapshots may yield two notices in one
    /// diff — a presence notice and a Changed — so a payload edit is never lost behind an
    /// online-flag flip.
    ///
    /// Notices are emitted in `after` order for every row `after` holds, each row contributing its
    /// presence notice before its Changed notice, followed by the Left notices in `before` order. A
    /// Left notice carries the earlier snapshot's payload, every other notice the later one's.
    /// @tparam Key      The identity rows match on; requires equality comparison.
    /// @tparam Payload  The consumer's per-row data.
    /// @param before    The earlier snapshot.
    /// @param after     The later snapshot.
    /// @param changed   Decides whether a matched pair fires Changed. An empty function falls back
    ///                  to operator!= where Payload supports it, and never fires Changed where it
    ///                  does not.
    /// @return The notices, in the order described above.
    template <typename Key, typename Payload>
    [[nodiscard]] vector<RosterNotice<Key, Payload>>
    DeriveRosterNotices(std::span<const RosterEntry<Key, Payload>> before,
                        std::span<const RosterEntry<Key, Payload>> after,
                        const function<bool(const Payload&, const Payload&)>& changed = {})
    {
        using Entry = RosterEntry<Key, Payload>;

        const auto findBy = [](std::span<const Entry> rows, const Key& id) -> const Entry*
        {
            const auto match =
                std::ranges::find_if(rows, [&id](const Entry& row) { return row.Id == id; });
            return match == rows.end() ? nullptr : &*match;
        };

        const auto payloadChanged = [&changed](const Payload& lhs, const Payload& rhs)
        {
            if (changed)
            {
                return changed(lhs, rhs);
            }
            if constexpr (requires { static_cast<bool>(lhs != rhs); })
            {
                return static_cast<bool>(lhs != rhs);
            }
            else
            {
                return false;
            }
        };

        vector<RosterNotice<Key, Payload>> notices;
        for (const Entry& row : after)
        {
            const Entry* const prior = findBy(before, row.Id);
            if (prior == nullptr)
            {
                notices.emplace_back(RosterNotice<Key, Payload>{
                    .Event = RosterEvent::Joined, .Id = row.Id, .Data = row.Data});
                continue;
            }
            if (prior->Online != row.Online)
            {
                notices.emplace_back(RosterNotice<Key, Payload>{
                    .Event = row.Online ? RosterEvent::CameOnline : RosterEvent::WentOffline,
                    .Id = row.Id,
                    .Data = row.Data});
            }
            if (payloadChanged(prior->Data, row.Data))
            {
                notices.emplace_back(RosterNotice<Key, Payload>{
                    .Event = RosterEvent::Changed, .Id = row.Id, .Data = row.Data});
            }
        }
        for (const Entry& row : before)
        {
            if (findBy(after, row.Id) == nullptr)
            {
                notices.emplace_back(RosterNotice<Key, Payload>{
                    .Event = RosterEvent::Left, .Id = row.Id, .Data = row.Data});
            }
        }
        return notices;
    }
}
