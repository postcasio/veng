#include <Veng/WorldDirectory.h>

#include <Veng/Log.h>
#include <Veng/WorldRunner.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace Veng
{
    struct WorldDirectory::State
    {
        // One live instance of a key: its world id, the presence counter (live joins plus pins), the
        // recorded travel payload (its factory params, offered to placement), whether it may be reaped
        // (factory-opened) or is pinned open forever (pre-registered), and the idle-since stamp set when
        // presence reaches zero.
        struct Bucket
        {
            WorldInstanceId World;
            Net::WorldKey Key;
            Net::TravelPayload Payload;
            u32 Presence = 0;
            // The accounts present on this bucket, refcounted per account (one account may hold
            // several presences — a join and a pin, say). Only valid accounts are recorded.
            std::unordered_map<Net::AccountId, u32> Members;
            bool Reapable = true;
            optional<f64> IdleSince;
            // The per-world idle-dwell override the opening resolution named; unset inherits the
            // directory's IdleKeepWarmDwell.
            optional<f64> Dwell;
            // The factory resolution recorded at open, so a borrowing host can wrap a bucket it did
            // not open itself (ResolutionOf). Unset for a pre-registered bucket.
            optional<ServerWorldResolution> Resolution;
        };

        u32 MaxHostedWorlds = 64;
        u32 MaxJoinedWorldsPerConnection = 8;
        u32 MaxPlayersPerInstance = 0;
        f64 IdleKeepWarmDwell = 5.0;
        WorldRunner* Runner = nullptr;
        function<bool(const Net::JoinRequestInfo&)> Authorize;
        function<optional<ServerWorldResolution>(const Net::WorldKey&, const Net::TravelPayload&)>
            WorldFactory;
        function<optional<WorldInstanceId>(const Net::JoinRequestInfo&,
                                           std::span<const WorldPlacement>)>
            Placement;
        function<void(WorldInstanceId)> CloseWorld;

        // The shared key → live buckets map and the buckets keyed by WorldInstanceId value.
        std::unordered_map<Net::WorldKey, vector<WorldInstanceId>> KeyMap;
        std::unordered_map<u64, Bucket> Buckets;

        Net::TravelPayload EmptyPayload;

        [[nodiscard]] Bucket* Find(WorldInstanceId world)
        {
            const auto it = Buckets.find(world.Value);
            return it != Buckets.end() ? &it->second : nullptr;
        }

        // The get-or-place selection: the custom policy if set, else the built-in capacity policy (the
        // first bucket under MaxPlayersPerInstance; 0 means the first — pure convergence). Only buckets
        // currently mapped to the key are offered, each carrying its presence and recorded payload.
        optional<WorldInstanceId> Place(const Net::JoinRequestInfo& request)
        {
            vector<WorldPlacement> buckets;
            if (const auto it = KeyMap.find(request.Key); it != KeyMap.end())
            {
                for (const WorldInstanceId world : it->second)
                {
                    const Bucket& bucket = Buckets.at(world.Value);
                    buckets.push_back(
                        {.World = world, .LiveSeats = bucket.Presence, .Payload = bucket.Payload});
                }
            }

            if (Placement)
            {
                return Placement(request, buckets);
            }

            for (const WorldPlacement& bucket : buckets)
            {
                if (MaxPlayersPerInstance == 0 || bucket.LiveSeats < MaxPlayersPerInstance)
                {
                    return bucket.World;
                }
            }
            return std::nullopt;
        }

        // Adds one unit of presence, recording a valid account as a bucket member.
        void Acquire(WorldInstanceId world, const Net::AccountId& account)
        {
            Bucket* bucket = Find(world);
            if (bucket == nullptr)
            {
                return;
            }
            bucket->Presence += 1;
            bucket->IdleSince.reset();
            if (account.IsValid())
            {
                bucket->Members[account] += 1;
            }
        }

        // Drops one unit of presence (and the account's membership when its refcount empties),
        // stamping the idle-since when the last presence leaves a reapable bucket so the dwell owns
        // its fate.
        void Release(WorldInstanceId world, f64 now, const Net::AccountId& account)
        {
            Bucket* bucket = Find(world);
            if (bucket == nullptr)
            {
                return;
            }
            if (bucket->Presence > 0)
            {
                bucket->Presence -= 1;
            }
            if (account.IsValid())
            {
                if (const auto it = bucket->Members.find(account); it != bucket->Members.end())
                {
                    it->second -= 1;
                    if (it->second == 0)
                    {
                        bucket->Members.erase(it);
                    }
                }
            }
            if (bucket->Presence == 0 && bucket->Reapable)
            {
                bucket->IdleSince = now;
            }
        }
    };

    WorldDirectory::WorldDirectory(Unique<State> state) : m_State(std::move(state)) {}

    WorldDirectory::~WorldDirectory() = default;

    Unique<WorldDirectory> WorldDirectory::Create(const WorldDirectoryInfo& info)
    {
        auto state = CreateUnique<State>();
        state->MaxHostedWorlds = info.MaxHostedWorlds;
        state->MaxJoinedWorldsPerConnection = info.MaxJoinedWorldsPerConnection;
        state->MaxPlayersPerInstance = info.MaxPlayersPerInstance;
        state->IdleKeepWarmDwell = info.IdleKeepWarmDwell;
        state->Runner = info.Runner;
        state->Authorize = info.Authorize;
        state->WorldFactory = info.WorldFactory;
        state->Placement = info.Placement;
        state->CloseWorld = info.CloseWorld;
        return Unique<WorldDirectory>(new WorldDirectory(std::move(state)));
    }

    void WorldDirectory::Register(const Net::WorldKey& key, const WorldInstanceId world,
                                  const Net::TravelPayload& payload)
    {
        State& s = *m_State;
        if (State::Bucket* existing = s.Find(world))
        {
            existing->Payload = payload;
            existing->Reapable = false;
            existing->IdleSince.reset();
            return;
        }
        s.Buckets.emplace(
            world.Value,
            State::Bucket{.World = world, .Key = key, .Payload = payload, .Reapable = false});
        vector<WorldInstanceId>& buckets = s.KeyMap[key];
        if (std::ranges::find(buckets, world) == buckets.end())
        {
            buckets.push_back(world);
        }
    }

    WorldResolveResult WorldDirectory::Resolve(const Net::JoinRequestInfo& request,
                                               const u32 heldWorlds)
    {
        State& s = *m_State;

        if (s.Authorize && !s.Authorize(request))
        {
            return {.Outcome = WorldResolveOutcome::Denied,
                    .Reason = Net::JoinDenyReason::NotAuthorized};
        }
        if (heldWorlds >= s.MaxJoinedWorldsPerConnection)
        {
            return {.Outcome = WorldResolveOutcome::Denied,
                    .Reason = Net::JoinDenyReason::PerConnectionCapReached};
        }

        if (const optional<WorldInstanceId> placed = s.Place(request))
        {
            return {.Outcome = WorldResolveOutcome::Placed, .World = *placed};
        }

        // A fresh-bucket open, bounded by the server-wide cap and gated on a factory.
        if (s.Buckets.size() >= s.MaxHostedWorlds)
        {
            return {.Outcome = WorldResolveOutcome::Denied,
                    .Reason = Net::JoinDenyReason::HostedWorldsCapReached};
        }
        if (!s.WorldFactory)
        {
            return {.Outcome = WorldResolveOutcome::Denied,
                    .Reason = Net::JoinDenyReason::NoSuchWorld};
        }
        optional<ServerWorldResolution> resolved = s.WorldFactory(request.Key, request.Payload);
        if (!resolved || resolved->World == nullptr)
        {
            return {.Outcome = WorldResolveOutcome::Denied,
                    .Reason = Net::JoinDenyReason::NoSuchWorld};
        }

        const WorldInstanceId world = resolved->WorldId;
        s.Buckets.emplace(world.Value, State::Bucket{.World = world,
                                                     .Key = request.Key,
                                                     .Payload = request.Payload,
                                                     .Reapable = true,
                                                     .Dwell = resolved->IdleDwell,
                                                     .Resolution = *resolved});
        s.KeyMap[request.Key].push_back(world);
        return {
            .Outcome = WorldResolveOutcome::Opened, .World = world, .Opened = std::move(resolved)};
    }

    void WorldDirectory::AddJoin(const WorldInstanceId world, const Net::AccountId& account)
    {
        m_State->Acquire(world, account);
    }

    void WorldDirectory::RemoveJoin(const WorldInstanceId world, const f64 now,
                                    const Net::AccountId& account)
    {
        m_State->Release(world, now, account);
    }

    void WorldDirectory::Pin(const WorldInstanceId world, const Net::AccountId& account)
    {
        m_State->Acquire(world, account);
    }

    void WorldDirectory::Unpin(const WorldInstanceId world, const f64 now,
                               const Net::AccountId& account)
    {
        m_State->Release(world, now, account);
    }

    vector<Net::AccountId> WorldDirectory::MembersOf(const Net::WorldKey& key) const
    {
        vector<Net::AccountId> members;
        const auto it = m_State->KeyMap.find(key);
        if (it == m_State->KeyMap.end())
        {
            return members;
        }
        for (const WorldInstanceId world : it->second)
        {
            for (const auto& [account, count] : m_State->Buckets.at(world.Value).Members)
            {
                if (std::ranges::find(members, account) == members.end())
                {
                    members.push_back(account);
                }
            }
        }
        return members;
    }

    vector<WorldInstanceId> WorldDirectory::ReapIdle(const f64 now)
    {
        State& s = *m_State;
        vector<WorldInstanceId> reaped;
        for (const auto& [value, bucket] : s.Buckets)
        {
            if (bucket.Reapable && bucket.Presence == 0 && bucket.IdleSince &&
                now - *bucket.IdleSince >= bucket.Dwell.value_or(s.IdleKeepWarmDwell))
            {
                reaped.push_back(bucket.World);
            }
        }
        for (const WorldInstanceId world : reaped)
        {
            const Net::WorldKey key = s.Buckets.at(world.Value).Key;
            // Hook-before-teardown: the consumer captures persistent state through CloseWorld first,
            // then the runner tears the world down (when a runner is configured).
            if (s.CloseWorld)
            {
                s.CloseWorld(world);
            }
            if (s.Runner != nullptr)
            {
                s.Runner->CloseWorld(world);
            }
            if (const auto it = s.KeyMap.find(key); it != s.KeyMap.end())
            {
                std::erase(it->second, world);
                if (it->second.empty())
                {
                    s.KeyMap.erase(it);
                }
            }
            s.Buckets.erase(world.Value);
        }
        return reaped;
    }

    usize WorldDirectory::WorldCount() const
    {
        return m_State->Buckets.size();
    }

    bool WorldDirectory::Contains(const WorldInstanceId world) const
    {
        return m_State->Buckets.contains(world.Value);
    }

    u32 WorldDirectory::PresenceOf(const WorldInstanceId world) const
    {
        const State::Bucket* bucket = m_State->Find(world);
        return bucket != nullptr ? bucket->Presence : 0;
    }

    const Net::TravelPayload& WorldDirectory::PayloadOf(const WorldInstanceId world) const
    {
        const State::Bucket* bucket = m_State->Find(world);
        return bucket != nullptr ? bucket->Payload : m_State->EmptyPayload;
    }

    const ServerWorldResolution* WorldDirectory::ResolutionOf(const WorldInstanceId world) const
    {
        const State::Bucket* bucket = m_State->Find(world);
        return bucket != nullptr && bucket->Resolution.has_value() ? &*bucket->Resolution : nullptr;
    }
}
