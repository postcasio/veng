#pragma once

#include <Veng/Net/AccountId.h>
#include <Veng/Persistence/Store.h>
#include <Veng/Veng.h>

#include <cstddef>
#include <span>

// Veng/Persistence/SessionStore.h — the shipped store backing for the session durability hooks.
//
// A SessionRegistry keeps its records for the life of the process unless its LoadSession/SaveSession
// hooks are set: the engine owns when durability fires (first admit, disconnect, teardown, the
// debounced checkpoint) and what is encoded, and the hook pair owns only where the bytes land. This
// header is the engine's own answer to "where": one store family, one record per account, keyed by
// the full account id. A consumer with an open Store gets durable sessions by registering the family
// and assigning the returned hooks; a consumer with storage of its own writes the raw hooks and
// never includes this header, and a consumer that sets no hooks at all keeps the memory-only
// posture unchanged.

namespace Veng
{
    /// @brief The engine's session family: one encoded session record per account.
    ///
    /// Minted from the same flat id space consumers mint from — the engine reserves no numeric
    /// range, only this one value.
    inline constexpr StoreFamilyId SessionsFamily{0xE25D907002419AC1ULL};

    /// @brief The session family's file stem within a slot directory (`veng.sessions.<gen>.vst`).
    ///
    /// Namespaced with the engine's `veng.` prefix: a bare "sessions" is the stem a consumer is
    /// most likely to choose for its own family, and two families sharing a stem is a refused
    /// registration.
    inline constexpr string_view SessionsFileStem = "veng.sessions";

    /// @brief How the binding treats a session save.
    struct SessionStoreInfo
    {
        /// @brief Whether a session save flushes the store to disk before returning.
        ///
        /// A session save is a genuine durability point — a disconnect may precede process death —
        /// so flushing is the default. The cost is that Store::Flush is whole-slot rather than
        /// per-family: on a slot carrying large unrelated families, every one of them that is dirty
        /// is rewritten and synced on someone's disconnect. A consumer that checkpoints the slot on
        /// its own cadence sets this false and keeps the session writes in memory until it does.
        bool FlushOnSave = true;
    };

    /// @brief The session durability hook pair, ready to assign onto a session-hosting info struct.
    ///
    /// The fields match Net::SessionRegistryInfo::LoadSession/SaveSession and the mirrored fields on
    /// ApplicationInfo and Net::ServerHostInfo, so one MakeSessionHooks result serves whichever of
    /// them a consumer fills.
    struct SessionHooks
    {
        /// @brief Reads an account's persisted session blob; nullopt when it has none.
        function<optional<vector<std::byte>>(Net::AccountId)> LoadSession;
        /// @brief Writes an account's session blob.
        function<void(Net::AccountId, std::span<const std::byte>)> SaveSession;
    };

    /// @brief Registers the session family on a store, once per store.
    ///
    /// The family carries no scene hooks: session records are fed through the hook pair, not
    /// captured off a scene. Registering is separate from MakeSessionHooks and idempotent, because
    /// a consumer that binds more than one session-hosting struct against a single store would
    /// otherwise register the same family twice, which is fatal.
    /// @param store  The store to register the family on.
    VE_API void RegisterSessionFamily(Store& store);

    /// @brief Builds the session hook pair over a store the source resolves per call.
    ///
    /// The source is resolved on every load and save rather than captured, because a session
    /// registry is built once and outlives any one store: a store holds an exclusive slot lock, so a
    /// consumer that lets a slot be closed and another opened has no store to name when the hooks
    /// are installed. A source that returns nullptr — including an empty source — is the memory-only
    /// posture: loads report no record and saves are dropped, exactly as with no hooks at all.
    ///
    /// The record is keyed by the full 128-bit account id (both halves) and holds the encoded
    /// session record as a single blob tagged with Net::SessionRecord's type id, so the bytes stay
    /// identifiable to a dump or an inspector. The family must be registered on the resolved store
    /// (see RegisterSessionFamily) or its records reach no file.
    /// @param storeSource  Resolves the store to persist into, or nullptr for memory-only.
    /// @param info         Save behavior; the default flushes on every save.
    /// @return The hook pair.
    /// @note SaveSession returns void, so a failed flush can only be logged — a consumer needing to
    ///       observe write failures flushes the store itself with FlushOnSave false.
    [[nodiscard]] VE_API SessionHooks MakeSessionHooks(function<Store*()> storeSource,
                                                       const SessionStoreInfo& info = {});
}
