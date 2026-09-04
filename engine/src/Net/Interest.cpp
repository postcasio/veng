#include <Veng/Net/Interest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <iterator>

namespace Veng::Net
{
    vector<InterestCandidate> GatherSpatialCandidates(const Scene& scene, const vec3& viewerPos,
                                                      const f32 leaveRadius)
    {
        vector<InterestCandidate> candidates;
        const f32 leaveSquared = leaveRadius * leaveRadius;
        for (auto [entity, identity, transform] : scene.View<NetIdentity, Transform>())
        {
            if (identity.Id == InvalidNetId)
            {
                continue;
            }
            const f32 distanceSquared =
                glm::dot(transform.Position - viewerPos, transform.Position - viewerPos);
            if (distanceSquared <= leaveSquared)
            {
                candidates.push_back(
                    InterestCandidate{.Id = identity.Id, .Distance = std::sqrt(distanceSquared)});
            }
        }
        return candidates;
    }

    vector<NetId> GatherAlwaysRelevant(const Scene& scene)
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();

        // The always-relevant component types, resolved once from the registry.
        vector<TypeId> marked;
        for (const auto& [id, info] : registry.All())
        {
            if (info.AlwaysRelevant)
            {
                marked.push_back(id);
            }
        }

        vector<NetId> ids;
        for (auto [entity, identity] : scene.View<NetIdentity>())
        {
            if (identity.Id == InvalidNetId)
            {
                continue;
            }
            for (const TypeId type : marked)
            {
                if (scene.TryGetComponent(entity, type) != nullptr)
                {
                    ids.push_back(identity.Id);
                    break;
                }
            }
        }
        return ids;
    }

    void ApplyRelevanceScope(const Scene& scene, const ConnectionId conn, set<NetId>& interest)
    {
        for (auto [entity, relevance] : scene.View<NetRelevance>())
        {
            if (relevance.Scope == RelevanceScope::All)
            {
                continue;
            }
            const auto* identity = scene.TryGet<NetIdentity>(entity);
            if (identity == nullptr || identity->Id == InvalidNetId)
            {
                continue;
            }
            const auto* authority = scene.TryGet<Authority>(entity);
            const ConnectionId owner = authority != nullptr ? authority->Owner : ServerConnectionId;
            const bool ownedByConn = owner == conn;
            const bool admits =
                relevance.Scope == RelevanceScope::OwnerOnly ? ownedByConn : !ownedByConn;
            if (!admits)
            {
                interest.erase(identity->Id);
            }
        }
    }

    set<NetId> UpdateInterest(std::span<const InterestCandidate> spatial,
                              std::span<const NetId> alwaysRelevant, std::span<const NetId> extra,
                              const InterestSettings& settings, InterestState& state)
    {
        set<NetId> next;
        const u32 dwell = settings.MinDwellSnapshots;

        // The unconditional arms: always-relevant global state and the policy hook's ids never cull.
        const auto keep = [&](NetId id)
        {
            next.insert(id);
            state.Dwell[id] = dwell;
        };
        for (const NetId id : alwaysRelevant)
        {
            keep(id);
        }
        for (const NetId id : extra)
        {
            keep(id);
        }

        // The spatial arm (disabled at radius 0): enter within Radius, stay within the leave band or
        // while dwell holds, leave once beyond it and the dwell floor has elapsed.
        set<NetId> seenSpatial;
        if (settings.Radius > 0.0f)
        {
            const f32 leave = settings.Radius * settings.LeaveMultiplier;
            for (const InterestCandidate& candidate : spatial)
            {
                seenSpatial.insert(candidate.Id);
                if (next.contains(candidate.Id))
                {
                    continue; // already always-relevant / policy
                }
                const bool wasIn = state.Current.contains(candidate.Id);
                if (candidate.Distance <= settings.Radius)
                {
                    next.insert(candidate.Id);
                    state.Dwell[candidate.Id] =
                        wasIn ? (state.Dwell[candidate.Id] > 0 ? state.Dwell[candidate.Id] - 1 : 0)
                              : dwell;
                }
                else if (wasIn && candidate.Distance <= leave)
                {
                    next.insert(candidate.Id); // hysteresis band: a member stays
                    if (state.Dwell[candidate.Id] > 0)
                    {
                        --state.Dwell[candidate.Id];
                    }
                }
                else if (wasIn && state.Dwell[candidate.Id] > 0)
                {
                    next.insert(candidate.Id); // beyond the band, but the dwell floor holds it
                    --state.Dwell[candidate.Id];
                }
            }

            // Previous members beyond even the leave radius (absent from the candidate scan) leave
            // once their dwell floor elapses.
            for (const NetId id : state.Current)
            {
                if (next.contains(id) || seenSpatial.contains(id))
                {
                    continue;
                }
                if (state.Dwell[id] > 0)
                {
                    next.insert(id);
                    --state.Dwell[id];
                }
            }
        }

        // Drop dwell entries for ids no longer in the set.
        for (auto it = state.Dwell.begin(); it != state.Dwell.end();)
        {
            it = next.contains(it->first) ? std::next(it) : state.Dwell.erase(it);
        }

        state.Current = next;
        return next;
    }
}
