#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Net/Interest.h>
#include <Veng/Net/JoinRequest.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Blob.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Veng.h>
#include <Veng/World.h>

#include <span>

// Veng/WorldDirectory.h — the role-neutral world get-or-place directory.
//
// World lifetime policy — the WorldKey → live-instance map, the placement order, the presence
// refcount, the keep-warm dwell, the idle reap, and the capacity caps — is engine policy in *every*
// role, not just under a transport. A ServerHost borrows one to resolve joins; a standalone
// Application constructs one to resolve travels; a dedicated server does both. The directory never
// touches a socket or a scene's contents: it maps keys to instances, calls the consumer's factory /
// placement / authorize / close hooks (each threaded the opaque travel payload), and counts
// presence. "Presence" sums two sources — live joins the host reports and local pins the presentation
// drives — so a world a viewport shows and a world a connection joined are kept warm by the same
// counter, and a departed world's fate is the dwell's in all three roles.
//
// Socket-free and presentation-free — it holds only value types and consumer callbacks, so it
// compiles under include_hygiene and never depends on net or render layers.

namespace Veng
{
    class Scene;
    class WorldRunner;

    /// @brief A world the get-or-place factory materialized for a WorldKey, handed back to the caller.
    ///
    /// The factory drives the consumer's world runner directly (opening a scene on a miss) and returns
    /// the borrowed scene plus the per-world replication configuration; a ServerHost wraps it with a
    /// ReplicationServer, and a standalone caller presents it. The scene must outlive the world.
    struct ServerWorldResolution
    {
        /// @brief The process-local id the consumer minted for the opened world.
        WorldInstanceId WorldId;
        /// @brief The opened world's scene; borrowed, must outlive the world.
        Scene* World = nullptr;
        /// @brief The AssetId of the level a joining client loads; the invalid id names a level-less world.
        ///
        /// A data world has no authored level: resolving with the invalid id makes the join reply
        /// carry no level, and the client installs an empty scene its stream populates (LoadLevel is
        /// never invoked). The digest below is validated either way.
        AssetId LevelId;
        /// @brief Fixed simulation ticks per second the opened world steps at (echoed in the join reply).
        ///
        /// Each joining client constructs that join's tick-offset estimator at this rate, so a slow
        /// data world's RTT converts into leads in its own ticks, not a shared default's.
        u32 SimTickRate = 60;
        /// @brief The content digest echoed to a joining client to validate its reconstructed world.
        Net::ContentDigest Digest;
        /// @brief The seat template spawned per join; null spawns a bare Viewer+Possesses seat.
        Ref<Prefab> SeatPrefab;
        /// @brief If valid, each seat's Spawn rides this prefab id so the client instantiates it too.
        AssetId SeatPrefabId;
        /// @brief The replication cadence for this world's ReplicationServer.
        ReplicationServer::Settings Replication;
        /// @brief The per-connection interest filter; Radius 0 replicates the whole world.
        Net::InterestSettings Interest;
        /// @brief The game hook adding entities to each connection's interest set; unset adds none.
        Net::InterestPolicy InterestPolicy;
        /// @brief Seconds this world outlives zero presence before it reaps; unset inherits the directory default.
        ///
        /// The per-world override of WorldDirectoryInfo::IdleKeepWarmDwell: a gameplay bubble wants
        /// seconds, a long-lived data world whose members may all blip offline together wants
        /// minutes. Applies only to this factory-opened bucket.
        optional<f64> IdleDwell;
    };

    /// @brief One live instance ("bucket") of a WorldKey, offered to the placement policy on a resolve.
    ///
    /// The get-or-place policy resolves a request to a bucket from the live set a key currently has.
    /// Each carries its world id, its current presence count (live joins plus pins — the capacity
    /// metric), and the opaque travel payload recorded when the bucket was opened, so a placement
    /// policy can compare the requester's payload against every live bucket's params (the proximity
    /// match) without reaching into directory internals. A key with no live buckets offers an empty
    /// span, and any policy returning nullopt asks the directory to open a fresh bucket.
    struct WorldPlacement
    {
        /// @brief The bucket's world instance id (a live instance under the key).
        WorldInstanceId World;
        /// @brief The bucket's current presence — its live joins plus pins, the capacity metric.
        u32 LiveSeats = 0;
        /// @brief The opaque travel payload recorded when this bucket was opened (its factory params).
        Net::Blob Payload;
    };

    /// @brief How a directory resolve turned out: an existing bucket, a freshly opened one, or a denial.
    enum class WorldResolveOutcome : u8
    {
        /// @brief The request converged on an existing live bucket (World names it).
        Placed,
        /// @brief The request opened a fresh bucket through the factory (Opened carries its resolution).
        Opened,
        /// @brief The request was refused (Reason carries why); no bucket exists for it.
        Denied,
    };

    /// @brief The result of a WorldDirectory::Resolve: the outcome plus what it produced.
    struct WorldResolveResult
    {
        /// @brief Whether the resolve placed, opened, or denied.
        WorldResolveOutcome Outcome = WorldResolveOutcome::Denied;
        /// @brief The bucket the request resolved to (valid when Outcome is Placed or Opened).
        WorldInstanceId World;
        /// @brief Why the request was refused (meaningful only when Outcome is Denied).
        Net::JoinDenyReason Reason = Net::JoinDenyReason::NoSuchWorld;
        /// @brief The factory's resolution, populated only when Outcome is Opened (the caller wraps it).
        optional<ServerWorldResolution> Opened;
    };

    /// @brief Configuration for a WorldDirectory: the caps, the dwell, and the consumer policy hooks.
    struct WorldDirectoryInfo
    {
        /// @brief The server-wide bound on total live instances; a fresh-bucket open past it is denied.
        u32 MaxHostedWorlds = 64;
        /// @brief The most worlds one connection may hold; a resolve past it is denied PerConnectionCapReached.
        ///
        /// The default budgets the standing-join architecture (see GameNetInfo's arithmetic): a
        /// gameplay world plus several standing data worlds plus a make-before-break travel overlap,
        /// with headroom — while still capping fan-out abuse.
        u32 MaxJoinedWorldsPerConnection = 8;
        /// @brief Per-instance presence cap the built-in placement policy fills to; 0 (default) converges.
        ///
        /// Drives the built-in get-or-place policy when Placement is unset: 0 converges every requester of
        /// a key on one bucket, a value > 0 places into the first bucket under that many present, opening a
        /// fresh bucket through the factory when every existing one is full. Ignored when Placement is set.
        u32 MaxPlayersPerInstance = 0;
        /// @brief Seconds a bucket with no presence is held warm before it is reaped.
        ///
        /// The directory-wide default; a factory-opened bucket whose resolution set
        /// ServerWorldResolution::IdleDwell dwells by that value instead.
        f64 IdleKeepWarmDwell = 5.0;
        /// @brief The optional runner the reap tears the closed world down through, after the close hook.
        ///
        /// When set, a reaped bucket is closed through CloseWorld (the consumer capture hook) first, then
        /// WorldRunner::CloseWorld here — the hook-before-teardown ordering the persistence capture point
        /// relies on. Unset leaves the runner teardown to the consumer's CloseWorld hook (the ServerHost
        /// test path, where the hook owns the scene's destruction).
        WorldRunner* Runner = nullptr;
        /// @brief The authorization hook: may this requester resolve/open this key? Unset allows all.
        ///
        /// Sees the whole request identity (connection, account, key, payload); admission precedes
        /// authorization, so a connection-borne request's account is always valid.
        function<bool(const Net::JoinRequestInfo&)> Authorize;
        /// @brief The get-or-create factory: materialize a world for a key that has no placeable bucket.
        ///
        /// Called only on a placement miss, after the caps clear; returning nullopt denies with
        /// NoSuchWorld. Unset means only pre-registered worlds (Register) can be resolved. The key and
        /// the travel payload ride in so a world may be parameterized by data no key encodes, and the
        /// requesting JoinRequestInfo rides in so a world may project the requester's account (its Key
        /// and Payload match the trailing two arguments). A resolve not driven by a particular join —
        /// a warm pre-open, a placement the consumer primes — passes a requester-less request:
        /// ConnectionId{} and the invalid account (Net::AccountId::IsValid() is false), so a factory
        /// keying off the requester treats the invalid account as "no specific requester" rather than a
        /// real player.
        function<optional<ServerWorldResolution>(const Net::JoinRequestInfo&, const Net::WorldKey&,
                                                 const Net::Blob&)>
            WorldFactory;
        /// @brief The get-or-place policy: which live bucket of a key a requester lands in, or a fresh one.
        ///
        /// Sees the request identity (connection, account, key, payload) and the key's live buckets
        /// (each with its presence and recorded payload). Returning an offered bucket's id places the
        /// requester there; nullopt asks for a fresh bucket. Unset uses the built-in capacity policy
        /// driven by MaxPlayersPerInstance.
        function<optional<WorldInstanceId>(const Net::JoinRequestInfo&,
                                           std::span<const WorldPlacement>)>
            Placement;
        /// @brief Closes a factory-opened world when it idles out; unset leaves the world's teardown to Runner.
        ///
        /// Invoked with a reaped bucket's id once it has been presence-less past IdleKeepWarmDwell —
        /// before WorldRunner::CloseWorld, so a consumer captures persistent state here. Pre-registered
        /// worlds (Register) are never reaped.
        function<void(WorldInstanceId)> CloseWorld;
    };

    /// @brief The role-neutral world directory: get-or-place, presence refcount, keep-warm, idle reap.
    ///
    /// Owns the WorldKey → live-buckets map and the lifetime policy around it. A ServerHost borrows one
    /// and reports joins as presence; a standalone Application constructs one and drives presence from
    /// its viewport pins. Resolve runs the fixed order (authorize → per-connection cap → placement → on
    /// a miss the hosted cap and the factory), Pin/Unpin and AddJoin/RemoveJoin move the presence
    /// counter, and ReapIdle closes a bucket that has idled past the dwell (the consumer hook first,
    /// then the runner). Pure policy — no socket, no scene contents, no presentation dependency.
    class VE_API WorldDirectory
    {
    public:
        /// @brief Creates a directory over the given caps, dwell, and policy hooks.
        /// @param info  The directory configuration.
        /// @return The directory.
        static Unique<WorldDirectory> Create(const WorldDirectoryInfo& info);

        ~WorldDirectory();

        WorldDirectory(const WorldDirectory&) = delete;
        WorldDirectory& operator=(const WorldDirectory&) = delete;

        /// @brief Pre-registers a never-reaped bucket under a key (the primary + AddWorld path).
        ///
        /// The world becomes resolvable by its key and is never idle-reaped — the consumer owns its
        /// lifetime. Registering a world already present updates its recorded payload and leaves it
        /// never-reaped. A key may hold several registered buckets.
        /// @param key      The key the bucket resolves under.
        /// @param world    The world instance id to register.
        /// @param payload  The travel payload recorded for the bucket (its factory params); empty by default.
        void Register(const Net::WorldKey& key, WorldInstanceId world,
                      const Net::Blob& payload = {});

        /// @brief Resolves a request to a bucket in the fixed order, opening a fresh one on a miss.
        /// @param request     The request identity (connection, account, key, payload) threaded into
        ///                    every hook; the payload is also recorded on a freshly opened bucket.
        /// @param heldWorlds  How many worlds the requester already holds (for the per-connection cap).
        /// @return The resolve outcome: a placed bucket, a freshly opened one, or a denial with its reason.
        [[nodiscard]] WorldResolveResult Resolve(const Net::JoinRequestInfo& request,
                                                 u32 heldWorlds);

        /// @brief Records a live join's presence on a bucket (the ServerHost report on a grant).
        ///
        /// A valid account is recorded as a member of the bucket (see MembersOf); the invalid id
        /// counts presence only.
        /// @param world    The bucket a join landed in.
        /// @param account  The joining connection's account, or the invalid id for account-less presence.
        void AddJoin(WorldInstanceId world, const Net::AccountId& account = {});

        /// @brief Drops a live join's presence from a bucket, starting the dwell if it reaches zero.
        /// @param world    The bucket a join left.
        /// @param now      Monotonic time in seconds; the idle-since stamp when presence reaches zero.
        /// @param account  The account recorded when the join was added, so its membership drops too.
        void RemoveJoin(WorldInstanceId world, f64 now, const Net::AccountId& account = {});

        /// @brief Pins a bucket present (the presentation drive shows this world), holding it warm.
        ///
        /// A pin is local presence indistinguishable from a live join: a pinned world is never reaped.
        /// Application pins the world a managed viewport presents (a pending rebind's destination
        /// counts), so a presented world is kept warm by the same counter the host's joins feed. A
        /// valid account (the local player, on a standalone or listen host) is recorded as a member of
        /// the bucket exactly like a connection's join, so MembersOf reports the local account beside
        /// connected ones.
        /// @param world    The bucket to pin.
        /// @param account  The local account present through this pin, or the invalid id for none.
        void Pin(WorldInstanceId world, const Net::AccountId& account = {});

        /// @brief Removes one pin from a bucket, starting the dwell if presence reaches zero.
        /// @param world    The bucket to unpin.
        /// @param now      Monotonic time in seconds; the idle-since stamp when presence reaches zero.
        /// @param account  The account recorded when the pin was taken, so its membership drops too.
        void Unpin(WorldInstanceId world, f64 now, const Net::AccountId& account = {});

        /// @brief Returns the accounts present across a key's live buckets — the membership primitive.
        ///
        /// The union of every valid account recorded by AddJoin/Pin on the key's buckets, deduplicated
        /// (an account present in two buckets of one key, or by join and pin at once, appears once).
        /// Empty for a key with no live buckets or only account-less presence.
        /// @param key  The key whose members are gathered.
        /// @return The member accounts, in no particular order.
        [[nodiscard]] vector<Net::AccountId> MembersOf(const Net::WorldKey& key) const;

        /// @brief Reaps every reapable bucket presence-less past the dwell; returns the reaped ids.
        ///
        /// For each reaped bucket: invokes the consumer CloseWorld hook first, then WorldRunner::CloseWorld
        /// when a runner is configured (the stated hook-before-teardown ordering), and drops the bucket
        /// and its key mapping so a later resolve cold-opens. Pre-registered buckets are never reaped.
        /// @param now  Monotonic time in seconds.
        /// @return The reaped world ids, so a borrowing host drops its per-world state for each.
        [[nodiscard]] vector<WorldInstanceId> ReapIdle(f64 now);

        /// @brief Returns the number of live buckets (pre-registered plus factory-opened).
        [[nodiscard]] usize WorldCount() const;

        /// @brief Returns whether a world id names a live bucket.
        /// @param world  The world to test.
        [[nodiscard]] bool Contains(WorldInstanceId world) const;

        /// @brief Returns a bucket's current presence (live joins plus pins), or 0 for an unknown world.
        /// @param world  The world to query.
        [[nodiscard]] u32 PresenceOf(WorldInstanceId world) const;

        /// @brief Returns a bucket's recorded travel payload, or an empty payload for an unknown world.
        /// @param world  The world to query.
        [[nodiscard]] const Net::Blob& PayloadOf(WorldInstanceId world) const;

        /// @brief Returns the factory resolution recorded when a bucket was opened, or nullptr.
        ///
        /// Every factory-opened bucket records its ServerWorldResolution, so a caller that did not
        /// run the factory itself can still wrap the bucket — a ServerHost borrowing a shared
        /// directory hosts a bucket a local (standalone) travel opened when a remote join converges
        /// on it. Null for a pre-registered (Register) or unknown world.
        /// @param world  The world to query.
        /// @return The recorded resolution, or nullptr.
        [[nodiscard]] const ServerWorldResolution* ResolutionOf(WorldInstanceId world) const;

    private:
        struct State;

        explicit WorldDirectory(Unique<State> state);

        Unique<State> m_State;
    };
}
