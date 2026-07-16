#pragma once

#include <Veng/Net/AccountId.h>
#include <Veng/Net/TravelPayload.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>
#include <Veng/World.h>

#include <cstddef>
#include <span>

// Veng/Net/Session.h — the per-account session record and the host-tier registry that keeps it.
//
// A session is an account's sitting with this host: the standing joins it holds (data worlds
// re-joined verbatim on reconnect) and the last gameplay world it presented (key, factory params,
// arrival pose). The record outlives the connection — it exists while the account is offline, spans
// worlds, and keys by account, so it lives beside the WorldDirectory at the host tier, never in a
// scene. Reconnecting is reattaching: an admitted account with a record has its standing joins
// re-issued and its gameplay world resolved back through the directory, so reconnect placement is
// engine policy, not a consumer special case. The registry is maintained as a side effect of the
// existing join/travel operations — no consumer bookkeeping.
//
// Durability is a hook pair (LoadSession/SaveSession): the engine owns when and what (load on first
// admit; save on disconnect, on teardown, and debounced at the checkpoint), the consumer owns where.
// With no hooks, records live for the process lifetime. The blob is the record's reflection-binary
// encoding, so schema drift reads tolerantly like any reflected value.

namespace Veng
{
    class TypeRegistry;
}

namespace Veng::Net
{
    /// @brief An account's last gameplay world: the key, the params that (re)materialize it, the pose.
    ///
    /// Params, not just the key, because a reaped dynamic world's minted key resolves to nothing
    /// while its params re-materialize an equivalent world through the ordinary get-or-place (or a
    /// placement policy re-matches a live neighbor). Pose is the consumer-encoded arrival pose —
    /// the last travel's payload until a CaptureTravelPose hook refreshes it (see
    /// SessionRegistryInfo::CaptureTravelPose).
    struct SessionGameplayEntry
    {
        /// @brief The gameplay world's key; the invalid (zero) key means none — the front door.
        WorldKey Key;
        /// @brief The opaque factory params that (re)materialize the world on a key miss.
        TravelPayload Params;
        /// @brief The consumer-encoded arrival pose, delivered back on reattach.
        TravelPayload Pose;
    };

    /// @brief One account's session with this host: its standing joins and last gameplay world.
    ///
    /// A plain reflected value type, so the durability blob is the ordinary reflection-binary
    /// encoding. Owned by a SessionRegistry keyed by account; a consumer sees it through the
    /// registry accessors and the TransformOnReattach hook.
    struct SessionRecord
    {
        /// @brief The account the record belongs to.
        AccountId Account;
        /// @brief The data worlds re-joined verbatim (non-presenting) on reattach.
        vector<WorldKey> StandingJoins;
        /// @brief The last gameplay world, restored through the directory on reattach.
        SessionGameplayEntry Gameplay;
    };

    /// @brief How a join participates in the account's session record.
    ///
    /// The resolved form of the explicit standing choice (see ResolveSessionDurability): the
    /// record stores the resolution, so a later reclassification is a call-site change, never a
    /// data migration.
    enum class SessionDurability : u8
    {
        /// @brief The join enters no record (an explicit standing opt-out: a prefetch, a spectate).
        None = 0,
        /// @brief The join is the account's gameplay world (key + params + pose recorded).
        Gameplay = 1,
        /// @brief The join is a standing join, re-joined verbatim on reattach; its leave removes it.
        Standing = 2,
    };

    /// @brief Resolves the explicit standing choice against presentation into a durability class.
    ///
    /// Unset standing resolves to "not presenting is standing" — a join without presentation is a
    /// data world held across reconnects, a presenting one is the gameplay world. Set, it
    /// overrides: true records a standing join regardless of presentation, false opts the join out
    /// of durability entirely.
    /// @param present   Whether the join presents (the gameplay-world signal).
    /// @param standing  The explicit standing choice, or unset for the presentation default.
    /// @return The durability class the record stores.
    [[nodiscard]] constexpr SessionDurability
    ResolveSessionDurability(const bool present, const optional<bool> standing)
    {
        if (standing.has_value())
        {
            return *standing ? SessionDurability::Standing : SessionDurability::None;
        }
        return present ? SessionDurability::Gameplay : SessionDurability::Standing;
    }

    /// @brief Encodes a session record into its durability blob (the reflection-binary encoding).
    /// @param record    The record to encode.
    /// @param registry  The type registry holding the SessionRecord schema.
    /// @pre SessionRecord is registered in @p registry (RegisterBuiltinTypes does so).
    /// @return The encoded blob.
    [[nodiscard]] VE_API vector<std::byte> EncodeSessionRecord(const SessionRecord& record,
                                                               const TypeRegistry& registry);

    /// @brief Decodes a durability blob back into a session record, tolerating schema drift.
    /// @param blob      The encoded blob (from EncodeSessionRecord, possibly of an older schema).
    /// @param registry  The type registry holding the SessionRecord schema.
    /// @pre SessionRecord is registered in @p registry.
    /// @return The decoded record, or an error string for a truncated/corrupt blob.
    [[nodiscard]] VE_API Result<SessionRecord> DecodeSessionRecord(std::span<const std::byte> blob,
                                                                   const TypeRegistry& registry);

    /// @brief Configuration for a SessionRegistry: the schema registry and the consumer hooks.
    struct SessionRegistryInfo
    {
        /// @brief The type registry the record codec and payload validation resolve against; required.
        const TypeRegistry* Types = nullptr;
        /// @brief Rewrites an account's record as a reattach begins; unset restores it as recorded.
        ///
        /// The consumer's one word on reattach placement: resurface in a different regime, veto a
        /// stale location. Runs before the gameplay resolve, so the rewritten record is what the
        /// reattach restores (and what the registry keeps).
        function<SessionRecord(SessionRecord)> TransformOnReattach;
        /// @brief Encodes an account's current gameplay pose from its seat; unset keeps the last pose.
        ///
        /// The engine cannot serialize game pose, so the consumer encodes it: invoked at disconnect
        /// and at the save checkpoint with the account's gameplay world and its seat entity there;
        /// the result overwrites the record's Gameplay.Pose. Unset, the pose stays the last
        /// travel's (arrival) pose.
        function<TravelPayload(WorldInstanceId, Entity)> CaptureTravelPose;
        /// @brief Loads an account's persisted durability blob; unset keeps records process-lifetime.
        ///
        /// Invoked once per account on its first admission; nullopt means no persisted record. The
        /// blob is decoded tolerantly (an undecodable blob is logged and treated as none).
        function<optional<vector<std::byte>>(AccountId)> LoadSession;
        /// @brief Persists an account's durability blob; unset keeps records process-lifetime.
        ///
        /// Invoked with the record's encoding on disconnect, on teardown, and for a dirty record at
        /// the debounced checkpoint. The consumer owns where the blob lands.
        function<void(AccountId, std::span<const std::byte>)> SaveSession;
        /// @brief Seconds between checkpoint saves of a dirty record (the save debounce).
        f64 SaveDebounceSeconds = 5.0;
    };

    /// @brief The host-tier per-account session registry: records, reattach, and durability.
    ///
    /// Lives beside the WorldDirectory — an Application constructs one and a ServerHost borrows it
    /// (or builds a private one), so a listen host, a dedicated server, and a standalone process
    /// keep session records through one code path. The registry never touches a socket or a scene:
    /// hosts feed it as a side effect of joins, travels, leaves, and disconnects.
    class VE_API SessionRegistry
    {
    public:
        /// @brief Creates a registry over the given schema registry and consumer hooks.
        /// @param info  The registry configuration; Types is required.
        /// @return The registry.
        static Unique<SessionRegistry> Create(const SessionRegistryInfo& info);

        ~SessionRegistry();

        SessionRegistry(const SessionRegistry&) = delete;
        SessionRegistry& operator=(const SessionRegistry&) = delete;

        /// @brief Loads an account's persisted record on its first admission (the LoadSession hook).
        ///
        /// A no-op when the account already has an in-memory record, when the hook is unset, or
        /// when the hook returns nullopt. An undecodable blob is logged and treated as none.
        /// @param account  The admitted account.
        void EnsureLoaded(const AccountId& account);

        /// @brief Returns an account's record, or nullptr when it has none.
        /// @param account  The account to look up.
        [[nodiscard]] const SessionRecord* Find(const AccountId& account) const;

        /// @brief Records a standing join of a key for an account (idempotent per key).
        /// @param account  The joining account.
        /// @param key      The standing world's key.
        void RecordStandingJoin(const AccountId& account, const WorldKey& key);

        /// @brief Removes a standing join from an account's record (a standing join's leave).
        /// @param account  The leaving account.
        /// @param key      The standing world's key.
        void RemoveStandingJoin(const AccountId& account, const WorldKey& key);

        /// @brief Records an account's gameplay world: the key, its params, and the arrival pose.
        /// @param account  The account whose gameplay entry is set.
        /// @param key      The gameplay world's key.
        /// @param params   The opaque factory params that (re)materialize the world.
        /// @param pose     The consumer-encoded arrival pose.
        void RecordGameplay(const AccountId& account, const WorldKey& key,
                            const TravelPayload& params, const TravelPayload& pose);

        /// @brief Refreshes an account's gameplay pose through the CaptureTravelPose hook.
        ///
        /// A no-op when the hook is unset, the account has no record, or the record holds no
        /// gameplay entry — the pose then stays the last travel's.
        /// @param account  The account whose pose is captured.
        /// @param world    The account's gameplay world instance.
        /// @param seat     The account's seat entity in that world.
        void CaptureGameplayPose(const AccountId& account, WorldInstanceId world, Entity seat);

        /// @brief Clears an account's gameplay entry (a failed reattach resolve degrades to the front door).
        /// @param account  The account whose gameplay entry is cleared.
        void ClearGameplay(const AccountId& account);

        /// @brief Begins a reattach: applies TransformOnReattach and validates the record's payloads.
        ///
        /// The transform hook rewrites the record (kept as the new record); a gameplay entry whose
        /// Params or Pose carry a type id unknown to the registry is untrusted — cleared with a
        /// logged reason, so the account lands at its front door. The caller resolves the returned
        /// gameplay entry through its WorldDirectory and reports a resolve failure via
        /// ClearGameplay.
        /// @param account  The reattaching account.
        /// @return A copy of the validated record, or nullopt when the account has none.
        [[nodiscard]] optional<SessionRecord> BeginReattach(const AccountId& account);

        /// @brief Saves one account's record now through the SaveSession hook (a no-op unset).
        /// @param account  The account to save.
        void Save(const AccountId& account);

        /// @brief Saves every dirty record now (teardown, StopNet).
        void SaveAll();

        /// @brief Runs the debounced durability checkpoint.
        ///
        /// When a dirty record's debounce has elapsed, invokes @p refresh first — the caller's
        /// window to capture live gameplay poses into the registry — then saves every dirty
        /// record. A no-op when nothing is dirty or the debounce has not elapsed.
        /// @param now      Monotonic time in seconds.
        /// @param refresh  Optional pre-save pose refresh; may be empty.
        void Checkpoint(f64 now, const function<void()>& refresh = {});

        /// @brief Returns the number of accounts holding an in-memory record.
        [[nodiscard]] usize Count() const;

    private:
        struct State;

        explicit SessionRegistry(Unique<State> state);

        Unique<State> m_State;
    };
}

// Reflected so the durability blob is the ordinary reflection-binary encoding: WriteFields over the
// record is the save format, ReadFields the tolerant load.
VE_REFLECT(::Veng::Net::SessionGameplayEntry, 0x45B24CF26D99D0BAULL)
VE_FIELD(Key, .DisplayName = "Key")
VE_FIELD(Params, .DisplayName = "Params")
VE_FIELD(Pose, .DisplayName = "Pose")
VE_REFLECT_END();

VE_REFLECT(::Veng::Net::SessionRecord, 0xAD8008044F869763ULL)
VE_FIELD(Account, .DisplayName = "Account")
VE_ARRAY_FIELD(StandingJoins, .DisplayName = "Standing Joins")
VE_FIELD(Gameplay, .DisplayName = "Gameplay")
VE_REFLECT_END();
