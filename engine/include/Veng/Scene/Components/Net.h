#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    /// @brief Who simulates and owns an entity: the ownership tier of an Authority.
    ///
    /// Integer values are stable — persisted in prefabs. Server and Local are the two authored tiers;
    /// Remote and Predicted are runtime client-side stances a replicated entity carries. Remote is the
    /// marking every replicated entity arrives from the wire with; Predicted is the promotion the
    /// client applies to the entities its own seat controls, so it re-runs the real Sim systems for
    /// them locally. Only Server and Local are ever authored or persisted; Remote and Predicted are
    /// each peer's stance toward an entity, never replicated.
    enum class Tier : u32
    {
        /// @brief Server-authoritative: the replicated, deterministic owner.
        Server = 0,
        /// @brief Client-local: never replicated, derived per client (view entities).
        Local = 1,
        /// @brief Client-side mirror of a server-owned entity: never simulated, interpolated for display.
        ///
        /// Stamped by the client's replication layer on every entity that arrives from the spawn
        /// stream. A Remote entity's state is server truth applied latest-wins; its Transform is
        /// presentation written by the View-phase remote-interpolation system, never authoritative.
        Remote = 2,
        /// @brief Client-side prediction stance over a server-owned entity the local seat controls.
        ///
        /// The client promotes the entities its own seat controls (its pawn and any attached subtree
        /// carrying replicated state) from Remote to Predicted, then re-runs the real Sim systems
        /// (control + movement) for them each client tick ahead of the server — so the local pawn
        /// responds on the tick its input is sampled. It is a client-side-only stance: a server never
        /// holds it, it is never replicated (tier is each peer's stance, not wire state), never
        /// authored (prefab/level data carries Server/Local only), and never persisted by the
        /// serializer. The authoritative snapshot corrects its accumulated prediction error.
        Predicted = 3,
    };

    /// @brief How many client ticks a prediction keeps for rollback.
    ///
    /// Twice the worst supported round trip in ticks, so a mispredict at the oldest still-unacked
    /// tick has its inputs and state to roll back to. The prediction history's default capacity and
    /// the depth of a predicted body's per-tick physics record share it, so a tick the history can
    /// replay from is a tick the solver can restore.
    inline constexpr usize PredictionWindowTicks = 128;

    /// @brief Ownership annotation marking who simulates an entity.
    ///
    /// Threaded onto entities with sensible defaults (authored entities are Server;
    /// client-local view entities like cameras are Local; an absent component defaults
    /// to Server). The net layer is its consumer: HasAuthority gates the authoritative
    /// Sim advancers by tier against the peer's NetRole, the replication server selects
    /// Server-tier entities for the snapshot stream, the hosts stamp each connection's
    /// seat with its Owner id, and the client marks replicated arrivals Remote.
    struct Authority
    {
        /// @brief The ownership tier.
        Tier Tier{Tier::Server};
        /// @brief Owning connection id (server-assigned; see NetEvents.h); 0 = server/none.
        u32 Owner = 0;
    };

    /// @brief Wire identity of a replicated entity: a server-assigned id the two ends agree on.
    ///
    /// The net layer's key for an entity across the wire. The authority owning an entity assigns it
    /// a fresh Id from a monotonic counter (never reused, never zero — zero is the null-reference
    /// sentinel on the wire), and the displaying end keeps its own NetId → Entity map, rebuilt from
    /// the NetIdentity components on the entities it spawns. It is runtime-only: server-assigned at
    /// spawn, never authored in a prefab and never persisted (like SkinnedPose/RootMotionDelta, it
    /// carries no on-disk lifetime), so it never appears in a prefab source. It is *not* itself
    /// replicated (VE_REPLICATED) — it *is* the wire key, carried in each snapshot's per-entity
    /// header rather than as a replicated component. It is reflected so the inspector can surface
    /// the assigned id (read-only — a consumer never edits it).
    struct NetIdentity
    {
        /// @brief The server-assigned wire id; 0 until assigned (and the wire's null-reference value).
        u32 Id = 0;
    };

    /// @brief An opaque 128-bit stable identity binding a replicated entity to its live local twin.
    ///
    /// The WorldKey discipline applied to entities: content derived on *both* peers that also carries
    /// server-authoritative dynamic state names itself with an anchor the consumer mints (from its own
    /// stable ids), and the engine never interprets the two halves. An authoritative entity carrying a
    /// NetAnchor replicates it in its spawn record; on the displaying peer the net layer resolves a
    /// live local entity carrying the equal anchor (its claimant) and binds the wire id to it — applying
    /// the replicated state onto the derived entity instead of spawning a duplicate. A claimant-less
    /// anchored spawn falls back to an ordinary wire-owned spawn. Reflected so a consumer may author or
    /// inspect it; not itself replicated (VE_REPLICATED) — it rides the spawn record, read before any
    /// entity is created, so the claimant is resolvable at spawn time rather than a snapshot later.
    struct NetAnchor
    {
        /// @brief Low 64 bits of the opaque anchor id.
        u64 Lo = 0;
        /// @brief High 64 bits of the opaque anchor id.
        u64 Hi = 0;

        /// @brief Equality over both halves — two entities share an anchor iff both halves match.
        [[nodiscard]] bool operator==(const NetAnchor&) const = default;
    };

    /// @brief How a replicated entity's relevance is scoped relative to its owning connection.
    ///
    /// The subject of a NetRelevance component. Owner here is the entity's Authority::Owner — the
    /// connection id an entity is owned by (0 = server/none) — so the two owner-relative scopes are
    /// meaningful only on an entity whose Owner names a connection.
    enum class RelevanceScope : u32
    {
        /// @brief Relevant to every connection — the default, byte-identical to no component.
        All = 0,
        /// @brief Relevant only to the connection that owns the entity (Authority::Owner).
        OwnerOnly = 1,
        /// @brief Relevant to every connection except the one that owns the entity.
        ExceptOwner = 2,
    };

    /// @brief Per-connection relevance scope: restricts which connections a replicated entity reaches.
    ///
    /// The general form of "this entity is not for everyone". The replication server already filters
    /// each connection's snapshot to its interest set; this component narrows that set by owner
    /// relationship, so an entity can be made relevant to only its owner (a player's private pickup),
    /// or to everyone but its owner (a proxy standing in for the owner's own view of an entity it
    /// sees differently). The owner is the entity's Authority::Owner. A scope of All (the default, and
    /// an entity with no component) leaves the interest set untouched, so every existing entity is
    /// unchanged and worlds carrying no scope pay nothing.
    ///
    /// It names *whether* an entity reaches a connection, never *how*: it gates spawn and delta
    /// uniformly (a filtered-out entity's spawn simply never sends, and it despawns with
    /// DespawnReason::Visibility if it later leaves a connection's set, re-spawning on re-entry). It is
    /// a purely server-side predicate — it changes no wire bytes, only whether they send — so it is
    /// reflected (a game authors it on a prefab) but not replicated.
    struct NetRelevance
    {
        /// @brief The relevance scope; All leaves the interest set unchanged.
        RelevanceScope Scope{RelevanceScope::All};
    };

    /// @brief The account a seat entity belongs to, stamped by the server when it spawns the seat.
    ///
    /// Server-local bookkeeping: the host stamps each spawned seat with the connection's admitted
    /// account so server-side game code can key player decisions on the person, not the transport
    /// link. Deliberately not replicated — the account id is never broadcast to world members; a
    /// game wanting a public identity replicates its own display component instead. Runtime-only,
    /// never authored (the account exists only once a connection is admitted).
    struct SeatAccount
    {
        /// @brief The seat's owning account (valid — admission precedes the seat spawn).
        Net::AccountId Account;
    };

    /// @brief Marks a remote seat whose client is presenting the seat's world.
    ///
    /// Server-local bookkeeping, like SeatAccount: the host adds it when the seat's client reports a
    /// viewport now shows the world (ClientHost::ReportPresenting) and removes it when the client
    /// reports it no longer does. A joining client loads and streams a world before it shows it, so
    /// game code that must not place a player in a world the player cannot yet see — a pawn that
    /// would fly or be seen by others meanwhile — waits for it. Never replicated, never authored.
    struct SeatPresented
    {
    };

    /// @brief The prefab an entity was spawned from, stamped on each spawned root by Prefab::SpawnInto.
    ///
    /// Spawn provenance: it ties a live entity back to the recipe that produced it, which nothing
    /// else records once a spawn has run. The net layer is its consumer — a host-authoritative
    /// entity carrying provenance *and* the NetSpawn marker has that prefab associated with its wire
    /// id, so a joiner instantiates it through the ordinary prefab path.
    ///
    /// Stamped only for a prefab loaded by AssetId; a runtime-built prefab is not addressable, so it
    /// records nothing. Provenance is derivable state, so it is registered without a reflected
    /// field: it never serializes into a prefab or a save and never rides the wire. It is inert on
    /// its own — carrying it changes no behavior anywhere.
    struct PrefabSource
    {
        /// @brief The prefab this entity was spawned from (always valid where the component exists).
        AssetId Prefab;
    };

    /// @brief Opt-in mark: replicate this locally-created host entity as a spawn of its source prefab.
    ///
    /// A server-spawned seat gets a prefab association for free, so a joiner instantiates it as a
    /// real prefab spawn. An entity a game spawns itself gets none, so its Spawn rides the
    /// runtime-constructed arm and a joiner receives only its replicated leaves — no prefab
    /// structure, no non-replicated components. Adding this marker beside the entity's PrefabSource
    /// closes that asymmetry: while the world is a live replication instance, the host associates
    /// the recorded prefab with the entity's wire id each pump, so an entity marked before
    /// replication begins is picked up when it does.
    ///
    /// **The mark is opt-in, and deliberately so.** Authority::Tier defaults to Server and every
    /// prefab spawn records provenance, so an authority-plus-provenance rule would enroll every
    /// host-local prop, effect, and editor-placed fixture a game meant to stay local, each at a
    /// per-connection bookkeeping cost and with no opt-out. Only marked entities are enrolled.
    ///
    /// It names *how* an entity replicates, never *whether*: Authority still decides that, so a
    /// Local-tier entity carrying the mark stays unreplicated. Carries no field and, being authored
    /// intent rather than derived state, is safe to author on a prefab.
    struct NetSpawn
    {
    };

    /// @brief Marks the seat this peer owns locally, so its input resolves from local devices.
    ///
    /// The net layer publishes it on a joining client's own seat as that seat binds, and removes it
    /// when the join is released. It is the fact `IsLocallyOwned` (Veng/Scene/InputMappingSystem.h)
    /// reads: because `Viewer` is always-relevant, a peer receives one replicated seat per peer, and
    /// this marker is what tells its own from the others'. A peer's seat that arrives through the
    /// stream carries none.
    ///
    /// The marker is **local-only**: it carries no reflected field, so it never serializes, never
    /// persists, and never rides the wire — it is published on the machine that owns the seat and
    /// means nothing off it. With no marker anywhere in a scene — single-player, headless, a host's
    /// own seat — every seat resolves locally, the pre-replication default.
    struct LocalSeat
    {
    };

    /// @brief Marks the pawn a presenting viewport's own seat controls on this machine.
    ///
    /// The engine's answer to "which pawn is mine?", derived in two steps that are both required:
    /// **a presenting viewport → the seat bound to it → that seat's Possesses → the pawn**. The
    /// first step is the one that cannot be dropped: on a client every mirrored pawn carries its
    /// own instantiated Tier::Local seat, so "possessed by a local seat" would mark every pawn on
    /// screen. Only the seat a viewport actually presents through yields a marker, so a client
    /// marks exactly its own pawn, a listen host marks the pawn its local seat controls, and a
    /// dedicated host — which presents nothing — marks nothing.
    ///
    /// The marker is singular per presenting viewport and carries the seat it was derived from, so
    /// under split-screen each viewport reads back its own pawn: a consumer resolves its viewport's
    /// seat and asks ResolveLocalControlledPawn (Veng/Scene/LocalControl.h) rather than treating a
    /// bare "some entity is marked" as its own. A flat every-locally-possessed-pawn marker is not
    /// what this is.
    ///
    /// Never replicated and never persisted — it is per-process presentation state, meaningless off
    /// the machine that derived it, so it carries no reflected field and rides no snapshot or save.
    /// @warning A consumer only ever **reads** this marker. The engine owns its whole lifecycle
    ///          (ReconcileLocalControl); adding, removing, or editing it in application code is
    ///          misuse and is overwritten at the next reconcile.
    /// @warning Viewport↔seat binding changes stamp it eagerly, but a possession change does not:
    ///          Possesses is a plain component a game writes directly and, on a client, one that
    ///          arrives through snapshot apply — neither raises an engine-side event. That case,
    ///          and only that case, is covered by a reconciling sweep the engine runs once per
    ///          frame over the presenting viewports (never a scan of the scene's entities), so the
    ///          marker reflects the possession state the frame renders.
    struct LocalControl
    {
        /// @brief The presenting viewport's seat whose Possesses named this pawn.
        Entity Seat = Entity::Null;
    };
}

VE_ENUM(::Veng::Tier, 0x45470D3410320AB9ULL)
VE_ENUMERATOR(Server)
VE_ENUMERATOR(Local)
VE_ENUMERATOR(Remote)
VE_ENUMERATOR(Predicted)
VE_ENUM_END();

VE_REFLECT(::Veng::Authority, 0xA934C4B9009D7735ULL)
VE_FIELD(Tier, .DisplayName = "Tier")
VE_FIELD(Owner, .DisplayName = "Owner")
VE_REFLECT_END();

// Reflected so the inspector surfaces the server-assigned id (read-only), but *not* replicated:
// NetIdentity is the wire key itself, carried in each snapshot's per-entity header.
VE_REFLECT(::Veng::NetIdentity, 0x9E7C4A1B6D3F0852ULL)
VE_FIELD(Id, .DisplayName = "Net Id", .ReadOnly = true)
VE_REFLECT_END();

// Reflected so a consumer authors/inspects the anchor, but *not* replicated: NetAnchor rides the
// spawn record (read before the entity is created) so the claimant resolves at spawn time.
VE_REFLECT(::Veng::NetAnchor, 0x6B5366CCAC328A6CULL)
VE_FIELD(Lo, .DisplayName = "Anchor Lo")
VE_FIELD(Hi, .DisplayName = "Anchor Hi")
VE_REFLECT_END();

VE_ENUM(::Veng::RelevanceScope, 0x6D6B56DD098E177AULL)
VE_ENUMERATOR(All)
VE_ENUMERATOR(OwnerOnly)
VE_ENUMERATOR(ExceptOwner)
VE_ENUM_END();

// Reflected so a game authors the scope on a prefab, but *not* replicated: it is a server-side
// interest predicate that changes no wire bytes, only which connections an entity's bytes reach.
VE_REFLECT(::Veng::NetRelevance, 0x2C7A94377311D11EULL)
VE_FIELD(Scope, .DisplayName = "Relevance")
VE_REFLECT_END();

// Registered without a reflected field, so it neither serializes nor rides the wire: provenance is
// derivable state recorded at spawn, never authored and never persisted.
VE_TYPE(::Veng::PrefabSource, 0xD0EA6653C1F9B14DULL);

// A fieldless mark: the component's presence is the whole signal, so there is nothing to reflect.
VE_TYPE(::Veng::NetSpawn, 0xF6B2DEC39DC3F319ULL);

// Registered without a reflected field, so it neither serializes nor rides the wire: the marker is
// local-only, published by the net layer on the seat this peer owns and meaningless off its machine.
VE_TYPE(::Veng::LocalSeat, 0x8689D3B182EB53CEULL);

// Registered without a reflected field, so it neither serializes nor rides the wire: the marker is
// per-process presentation state the engine derives per frame, meaningless off its machine.
VE_TYPE(::Veng::LocalControl, 0x7B0B171ABA0821EDULL);

// Reflected so the inspector surfaces the seat's account (read-only), but *not* replicated: the
// account id stays server-local, never broadcast to world members.
VE_REFLECT(::Veng::SeatAccount, 0xF3DBE3736F6A92EDULL)
VE_FIELD(Account, .DisplayName = "Account", .ReadOnly = true)
VE_REFLECT_END();

// Registered without a reflected field: server-local presentation state, never serialized or sent.
VE_TYPE(::Veng::SeatPresented, 0x8A22B7F43E5EE2B3ULL);
