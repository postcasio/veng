#pragma once

#include <Veng/Reflection/ReflectionTypes.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <utility>

namespace Veng
{
    class Scene;

    /// @brief A minted 64-bit id naming one store family (`vengc generate-family-id`).
    ///
    /// A family is one keyspace of durable records, persisted as its own file within a slot
    /// directory. The id is the family's stable identity in the file headers; the file stem is
    /// presentation only. Ids are minted from a single flat space — the engine's own families are
    /// drawn from the same space consumers mint from, and a collision is fatal at registration.
    struct StoreFamilyId
    {
        /// @brief The minted id value; zero names no family.
        u64 Value = 0;

        /// @brief Returns whether the id names a family.
        [[nodiscard]] bool IsValid() const { return Value != 0; }

        /// @brief Equality on the id value.
        [[nodiscard]] bool operator==(const StoreFamilyId&) const = default;
    };

    /// @brief A record's key within a family: an opaque 128-bit id, verbatim.
    ///
    /// The store never interprets the bits, so any id space a consumer already owns keys records
    /// directly. Both halves are load-bearing — a consumer keying on a 128-bit id passes both.
    struct StoreKey
    {
        /// @brief Low 64 bits of the key.
        u64 Lo = 0;
        /// @brief High 64 bits of the key.
        u64 Hi = 0;

        /// @brief Equality over both halves.
        [[nodiscard]] bool operator==(const StoreKey&) const = default;
    };

    /// @brief One captured reflected component: the type it encodes and its reflection-binary bytes.
    ///
    /// The bytes are WriteFields output, or an already-encoded opaque blob under InvalidTypeId. The
    /// reflection walker's tolerant read is the schema-evolution story — an unknown field in the
    /// bytes is skipped, a missing field keeps its default.
    struct ComponentBlob
    {
        /// @brief The reflected type the bytes encode, or InvalidTypeId for an opaque blob.
        TypeId Type = InvalidTypeId;
        /// @brief The component's reflection-binary bytes.
        vector<u8> Bytes;

        /// @brief Value equality over the type id and the bytes.
        [[nodiscard]] bool operator==(const ComponentBlob&) const = default;
    };

    /// @brief One durable record: captured reflected components plus when they were captured.
    struct StoreRecord
    {
        /// @brief Wall-clock seconds (Unix epoch) at capture — the input a rehydrate derives its
        ///        elapsed time from.
        i64 CapturedAtWall = 0;
        /// @brief The captured component blobs.
        vector<ComponentBlob> Components;
    };

    /// @brief One registered store family: its id, file identity, version, and optional hooks.
    ///
    /// Registered by whichever system owns the family's data. Both scene hooks are optional: a
    /// family fed through another path — records written directly rather than captured off a scene
    /// — registers neither.
    struct StoreFamily
    {
        /// @brief The family's minted id.
        StoreFamilyId Id;
        /// @brief The family file's name stem within the slot directory ("items" → items.<gen>.vst).
        ///
        /// A stem is a single path component of `[A-Za-z0-9._-]`, non-empty, at most
        /// Store::MaxFileStemLength bytes, and neither "." nor ".."; it must be unique across
        /// registered families, since two families sharing a stem would write the same file.
        string FileStem;
        /// @brief The family's format version, stored in the family file's header only (never per
        ///        record).
        ///
        /// Schema evolution within a version is the reflection walker's tolerant read; a version
        /// bump is an explicit Migrate function run at Read.
        u32 Version = 1;
        /// @brief Captures a scene's durable state into records; unset for a family fed elsewhere.
        /// @see Store::CaptureScene
        function<void(Scene&, vector<std::pair<StoreKey, StoreRecord>>&)> Capture;
        /// @brief Collects the keys in a fresh scene this family may hold records for; unset
        ///        rehydrates none.
        /// @see Store::RehydrateScene
        function<vector<StoreKey>(Scene&)> RehydrateKeys;
        /// @brief Applies a stored record onto a fresh scene, with elapsed wall seconds since
        ///        capture.
        ///
        /// The elapsed time is clamped >= 0 at the call site (wall clocks regress — NTP, suspend,
        /// manual change). An identity implementation restores state verbatim and ignores it; the
        /// argument is the seam a consumer that must account for time spent away hangs its own
        /// policy off.
        function<void(Scene&, StoreKey, const StoreRecord&, f64 elapsedSeconds)> Rehydrate;
        /// @brief Migrates one record read from an older-version family file up to Version.
        ///
        /// Run at Read when the record's family file was written under an older version; the
        /// returned record replaces the stored one (persisted at Version on the next flush). An
        /// error, or an older file with no Migrate hook, reads as no record (logged once).
        function<Result<StoreRecord>(u32 storedVersion, StoreRecord)> Migrate;
    };

    /// @brief The durable-state substrate: one instance per slot directory, per process.
    ///
    /// The place state outlives worlds, sessions, and the process. Records are captured reflected
    /// components keyed by opaque 128-bit ids, grouped into registered families; reads and writes
    /// are memory-only against per-family tables loaded at open, and Flush persists the dirty set
    /// atomically for the whole slot: dirty families write under generation-suffixed names, then a
    /// small commit record renames into place last — that rename is the commit point, so a crash
    /// anywhere in the flush leaves the prior generation fully readable. Open takes an exclusive
    /// slot lock (an advisory lock file) and fails loudly on contention.
    ///
    /// The store owns the `slot.` file-name prefix within a slot directory and its own
    /// `<stem>.<generation>.vst` family files; every other file in the directory is left alone.
    class VE_API Store
    {
    public:
        /// @brief The longest family file stem the store accepts, in bytes.
        static constexpr usize MaxFileStemLength = 64;

        /// @brief Opens a store on a slot directory, creating the directory when absent.
        ///
        /// Takes the exclusive slot lock, reads the commit record (an absent one is an empty store
        /// — a fresh slot), and loads every referenced family file into memory. Records of
        /// families never registered in this process are preserved verbatim across Flush. A
        /// directory carrying an unrecognized file under the store's reserved `slot.` prefix, or a
        /// commit record the store cannot read, fails the open rather than reading as fresh — an
        /// unreadable slot is reported, never silently replaced.
        /// @param slotDirectory  The slot directory; created when absent.
        /// @return The opened store, or a recoverable error (lock contention, an unreadable or
        /// unrecognized slot, a rejected file stem, an implausible record count).
        [[nodiscard]] static Result<Unique<Store>> Open(const path& slotDirectory);

        /// @brief Releases the slot lock and drops the in-memory tables; unflushed writes are lost.
        ~Store();

        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;

        /// @brief Registers a family: its file identity, version, and optional capture/rehydrate
        ///        hooks.
        ///
        /// Attaches the hooks and version to the family's table (loading nothing — the tables were
        /// loaded at Open). Registering the same id twice, or a stem another family already
        /// claims, is a fatal assert, as is a stem failing the FileStem rules.
        /// @param family  The family to register; Id must be valid and FileStem must be a legal,
        /// unclaimed stem.
        void RegisterFamily(StoreFamily family);

        /// @brief Reads a record, running the family's version migration when the stored version is
        ///        older.
        /// @param family  The family to read from.
        /// @param key     The record's key.
        /// @return A copy of the record, or nullopt when none exists (or a needed migration
        /// failed).
        [[nodiscard]] optional<StoreRecord> Read(StoreFamilyId family, StoreKey key);

        /// @brief Writes (inserts or replaces) a record and marks the family dirty.
        /// @param family  The family to write into.
        /// @param key     The record's key.
        /// @param record  The record to store.
        void Write(StoreFamilyId family, StoreKey key, StoreRecord record);

        /// @brief Registers an observer of record changes, fired per changed key.
        ///
        /// The pub seam an event-driven projection hangs off: every Write, every effective Erase,
        /// and each record EraseAll drops notifies with the family and key, after the table
        /// reflects the change (an observer's Read sees the new state; nullopt means erased).
        /// Observers are never removed — they must outlive the store. A notified observer may write
        /// or erase further records; the nested change notifies again.
        /// @param onChanged  Invoked with the changed record's family and key.
        void Subscribe(function<void(StoreFamilyId, StoreKey)> onChanged);

        /// @brief Erases a record and marks the family dirty; a missing record is a no-op.
        /// @param family  The family to erase from.
        /// @param key     The record's key.
        void Erase(StoreFamilyId family, StoreKey key);

        /// @brief Erases every record of every family and marks them dirty (the whole-slot reset).
        void EraseAll();

        /// @brief Visits every record of a family, in no particular order.
        ///
        /// The whole-family read a projection seeds from (the store keys on opaque ids, so a
        /// consumer cannot enumerate keys itself). Records lagging an older stored version are
        /// migrated as they are visited, exactly as Read migrates. The visitor must not write or
        /// erase records of the visited family.
        /// @param family  The family to visit.
        /// @param visit   Invoked with each record's key and (migrated) value.
        void ForEachRecord(StoreFamilyId family,
                           const function<void(StoreKey, const StoreRecord&)>& visit);

        /// @brief Persists the dirty families to disk, atomically for the whole slot.
        ///
        /// Dirty families write under the next generation's file names, each synced; then the
        /// commit record replaces the old one by rename — the commit point. Superseded
        /// old-generation files are deleted after the commit (best effort). A no-op when nothing is
        /// dirty.
        /// @return Empty on success; a recoverable error describing the failed write.
        VoidResult Flush();

        /// @brief Runs every capture-registered family over a scene, writing its records.
        ///
        /// Each family's Capture collects records off the scene, each stamped with the current wall
        /// clock and written (marking the family dirty). Memory-only — the file I/O is Flush's.
        /// @param scene  The scene to capture.
        void CaptureScene(Scene& scene);

        /// @brief Rehydrates a freshly-built scene from every rehydrate-registered family.
        ///
        /// For each family with both RehydrateKeys and Rehydrate: collects the scene's keys, reads
        /// each record (through any migration), and applies it with the elapsed wall seconds since
        /// capture, clamped >= 0.
        /// @param scene  The freshly-built scene to rehydrate.
        void RehydrateScene(Scene& scene);

        /// @brief Returns whether a family has been registered on this store.
        ///
        /// Registering a family twice is fatal, so a helper that registers a well-known family on a
        /// consumer's behalf asks first rather than assuming it is the only caller.
        /// @param family  The family to test.
        [[nodiscard]] bool IsFamilyRegistered(StoreFamilyId family) const;

        /// @brief Returns whether any family holds unflushed writes.
        [[nodiscard]] bool IsDirty() const;

        /// @brief Returns the slot directory this store is open on.
        [[nodiscard]] const path& GetSlotDirectory() const;

        /// @brief Returns the committed generation number (0 before the first flush of a fresh
        ///        slot).
        [[nodiscard]] u64 GetGeneration() const;

        /// @brief Returns the total record count across every family (a debug-surface statistic).
        [[nodiscard]] usize GetRecordCount() const;

        /// @brief The current wall clock in whole seconds since the Unix epoch (a record's
        ///        CapturedAtWall).
        [[nodiscard]] static i64 WallClockSeconds();

        /// @brief Returns whether a string is a legal family file stem.
        ///
        /// A stem is a single path component of `[A-Za-z0-9._-]`, non-empty, at most
        /// MaxFileStemLength bytes, and neither "." nor "..". Stems read from a slot's commit
        /// record are checked against this before being interpolated into a path, so a crafted
        /// commit record can neither read nor write outside the slot directory.
        /// @param stem  The candidate stem.
        /// @return True when the stem is legal.
        [[nodiscard]] static bool IsValidFileStem(string_view stem);

    private:
        struct State;

        explicit Store(Unique<State> state);

        Unique<State> m_State;
    };
}
