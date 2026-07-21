#include <Veng/Net/Social.h>

#include <algorithm>

namespace Veng::Net
{
    namespace
    {
        // An entry lives for exactly its expiry window: issued at t, it is live while now - t is
        // under expirySeconds and expired from the instant the window closes.
        bool IsExpired(const f64 issuedAt, const f64 now, const f64 expirySeconds)
        {
            return now - issuedAt >= expirySeconds;
        }
    }

    void InviteTable::Issue(const WorldKey& world, const AccountId invitee, const u64 token,
                            const f64 now)
    {
        const auto existing =
            std::ranges::find_if(m_Entries, [&world, invitee](const Entry& entry)
                                 { return entry.World == world && entry.Invitee == invitee; });
        if (existing != m_Entries.end())
        {
            existing->Token = token;
            existing->IssuedAt = now;
            return;
        }
        m_Entries.emplace_back(
            Entry{.World = world, .Invitee = invitee, .Token = token, .IssuedAt = now});
    }

    bool InviteTable::Consume(const WorldKey& world, const AccountId invitee, const u64 token,
                              const f64 now, const f64 expirySeconds)
    {
        const auto existing =
            std::ranges::find_if(m_Entries, [&world, invitee](const Entry& entry)
                                 { return entry.World == world && entry.Invitee == invitee; });
        if (existing == m_Entries.end())
        {
            return false;
        }
        if (IsExpired(existing->IssuedAt, now, expirySeconds))
        {
            m_Entries.erase(existing);
            return false;
        }
        // A mismatched presentation leaves the entry standing: a wrong guess must not burn the
        // invite the legitimate holder still needs.
        if (existing->Token != token)
        {
            return false;
        }
        m_Entries.erase(existing);
        return true;
    }

    bool InviteTable::IsInvited(const WorldKey& world, const AccountId invitee, const f64 now,
                                const f64 expirySeconds) const
    {
        const auto existing =
            std::ranges::find_if(m_Entries, [&world, invitee](const Entry& entry)
                                 { return entry.World == world && entry.Invitee == invitee; });
        return existing != m_Entries.end() && !IsExpired(existing->IssuedAt, now, expirySeconds);
    }

    void InviteTable::SweepExpired(const f64 now, const f64 expirySeconds)
    {
        const auto stale =
            std::ranges::remove_if(m_Entries, [now, expirySeconds](const Entry& entry)
                                   { return IsExpired(entry.IssuedAt, now, expirySeconds); });
        m_Entries.erase(stale.begin(), stale.end());
    }

    void InviteTable::ClearFor(const WorldKey& world)
    {
        const auto matching = std::ranges::remove_if(m_Entries, [&world](const Entry& entry)
                                                     { return entry.World == world; });
        m_Entries.erase(matching.begin(), matching.end());
    }

    usize InviteTable::GetCount() const
    {
        return m_Entries.size();
    }

    JoinVerdict JudgeInviteGatedJoin(const bool inRoster, const bool hasInvite,
                                     const usize rosterCount, const usize capacity)
    {
        if (inRoster)
        {
            return JoinVerdict::AdmitMember;
        }
        if (!hasInvite)
        {
            return JoinVerdict::RefuseUninvited;
        }
        if (rosterCount >= capacity)
        {
            return JoinVerdict::RefuseFull;
        }
        return JoinVerdict::AdmitByInvite;
    }

    PresenceTransition ClassifyMemberPresence(const bool inRoster, const bool online,
                                              const bool present, const bool connected)
    {
        if (present && connected)
        {
            if (!inRoster)
            {
                return PresenceTransition::Join;
            }
            if (!online)
            {
                return PresenceTransition::Rejoin;
            }
            return PresenceTransition::None;
        }
        if (inRoster && online && !connected)
        {
            return PresenceTransition::Offline;
        }
        return PresenceTransition::None;
    }
}
