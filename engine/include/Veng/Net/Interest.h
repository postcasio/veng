#pragma once

#include <Veng/Net/Replication.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/Interest.h — the per-connection relevancy filter over the send loop.
//
// A connection hears about what matters to it, not the world: its interest set is a spatial query
// around its pawn/viewer, unioned with the always-relevant entities (the seats, a game's global
// game state every client needs) and a game policy hook. Set enter/leave rides the spawn/despawn
// machinery as connection-scoped visibility. This is the scale lever — bandwidth stops growing with
// world size and grows with what each player can see. Radius 0 disables interest entirely (every
// entity relevant, the planset-54 behavior). The set math is pure and device-free; the scene scans
// are headless-safe (no renderer/broadphase dependency — the server's own relevancy query over
// scene state), so a dedicated server filters interest with no graphics stack.

namespace Veng
{
    class Scene;

    namespace Net
    {
        /// @brief A game hook adding entities to a connection's interest set beyond the spatial arm.
        ///
        /// Given the connection's seat and possessed pawn, returns the extra entities that must be
        /// relevant regardless of distance — team members, quest objectives, a coupled vehicle. The
        /// returned entities are unioned into the interest set (never culled). Unset adds nothing.
        /// @param scene  The server scene.
        /// @param seat   The connection's seat entity (may be null before it spawns).
        /// @param pawn   The seat's possessed pawn (may be null before possession).
        /// @return The extra entities to keep relevant to the connection.
        using InterestPolicy =
            function<vector<Entity>(const Scene& scene, Entity seat, Entity pawn)>;

        /// @brief The interest radius and boundary-stability knobs.
        struct InterestSettings
        {
            /// @brief The enter radius in the world's linear unit; 0 disables interest (every
            /// entity relevant).
            f32 Radius = 0.0f;
            /// @brief The leave radius is Radius times this — the hysteresis band that prevents flapping.
            f32 LeaveMultiplier = 1.15f;
            /// @brief The minimum snapshots a member stays after entering, a dwell floor on membership.
            u32 MinDwellSnapshots = 4;
        };

        /// @brief One spatial candidate: a replicated entity's wire id and its distance to the viewer.
        struct InterestCandidate
        {
            /// @brief The candidate entity's wire id.
            NetId Id = InvalidNetId;
            /// @brief The candidate's distance to the viewer, in the world's linear unit.
            f32 Distance = 0.0f;
        };

        /// @brief A connection's interest bookkeeping: the current set and per-member dwell counters.
        struct InterestState
        {
            /// @brief The NetIds currently in the connection's interest set.
            set<NetId> Current;
            /// @brief Snapshots remaining before each member may leave (the min-dwell floor).
            unordered_map<NetId, u32> Dwell;
        };

        /// @brief Gathers the replicated entities within @p leaveRadius of @p viewerPos, with distances.
        ///
        /// A const scene scan over entities carrying a NetIdentity and a Transform — headless-safe,
        /// so the dedicated server queries relevancy with no renderer. An entity with no Transform is
        /// not a spatial candidate (it is interest-visible only via AlwaysRelevant/policy).
        /// @param scene       The server scene to scan.
        /// @param viewerPos   The interest query center (the connection's pawn/viewer position).
        /// @param leaveRadius The outer (leave) radius to gather within, so the hysteresis band is covered.
        /// @return The candidates within @p leaveRadius, each with its distance.
        [[nodiscard]] VE_API vector<InterestCandidate>
        GatherSpatialCandidates(const Scene& scene, const vec3& viewerPos, f32 leaveRadius);

        /// @brief Gathers the wire ids of entities carrying any AlwaysRelevant-marked component.
        ///
        /// The global-state arm of the interest union (the seats, a game's global state): relevant to every
        /// connection regardless of distance. A const scene scan.
        /// @param scene  The server scene to scan.
        /// @return The always-relevant entities' NetIds.
        [[nodiscard]] VE_API vector<NetId> GatherAlwaysRelevant(const Scene& scene);

        /// @brief Narrows an interest set by each entity's NetRelevance scope for one connection.
        ///
        /// The owner-relative relevance filter: an entity carrying a NetRelevance whose scope excludes
        /// @p conn is removed from the set, so it neither spawns to nor updates that connection. The
        /// owner an entity is scoped against is its Authority::Owner (ServerConnectionId when it
        /// carries no Authority) — OwnerOnly keeps the entity only for its owning connection,
        /// ExceptOwner keeps it for every connection but the owner. A scope of All (and an entity with
        /// no NetRelevance) is left untouched, and the scan is over the NetRelevance pool alone, so a
        /// world authoring no scope pays a pool lookup and nothing more. A const, headless-safe scene
        /// scan, so a dedicated server applies it with no renderer.
        /// @param scene     The server scene to scan.
        /// @param conn      The connection whose interest set is being narrowed.
        /// @param interest  The interest set to narrow in place.
        VE_API void ApplyRelevanceScope(const Scene& scene, ConnectionId conn,
                                        set<NetId>& interest);

        /// @brief Computes a connection's new interest set, applying hysteresis and the dwell floor.
        ///
        /// The union of: spatial candidates within the enter radius (or the leave radius while already
        /// a member — the hysteresis band), the always-relevant ids, and @p extra (the policy hook's
        /// ids), all unconditional. A member beyond the leave radius stays until its dwell expires.
        /// Mutates @p state (its Current set and Dwell counters). With Radius 0 the spatial arm is
        /// disabled and the set is always-relevant ∪ extra only.
        /// @param spatial          The spatial candidates (from GatherSpatialCandidates).
        /// @param alwaysRelevant   The always-relevant ids (from GatherAlwaysRelevant).
        /// @param extra            The policy hook's extra relevant ids (team, objectives, owned).
        /// @param settings         The radius and hysteresis/dwell knobs.
        /// @param state            The connection's interest bookkeeping, updated in place.
        /// @return The new interest set (also stored in state.Current).
        VE_API set<NetId> UpdateInterest(std::span<const InterestCandidate> spatial,
                                         std::span<const NetId> alwaysRelevant,
                                         std::span<const NetId> extra,
                                         const InterestSettings& settings, InterestState& state);
    }
}
