#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>

namespace Veng
{
    class TaskSystem;

    namespace Detail
    {
        /// @brief Reads the cache-entry Ref out of a type-erased AssetHandle field's bytes.
        ///
        /// Every AssetHandle<T> shares the layout { AssetId; Ref<AssetCacheEntry>; } (pinned by
        /// AssetHandleLayoutGuard), so the entry sits at AssetHandleEntryOffset off the field
        /// bytes regardless of T — letting a reflection walk capture a handle field generically.
        /// Returns null for an empty handle field.
        /// @param fieldPtr  The first byte of an AssetHandle field (its AssetId, at offset 0).
        /// @return The shared cache entry, or null when the handle is empty.
        inline Ref<AssetCacheEntry> EntryOfHandleField(const void* fieldPtr)
        {
            const auto* entrySlot = reinterpret_cast<const Ref<AssetCacheEntry>*>(
                static_cast<const u8*>(fieldPtr) + AssetHandleEntryOffset);
            return *entrySlot;
        }

        /// @brief Reads the cache-entry Ref out of a typed AssetHandle without going through the manager.
        /// @tparam T  The asset type the handle refers to.
        /// @param handle  The handle whose cache entry is read.
        /// @return The shared cache entry, or null when the handle is empty.
        template <typename T>
        Ref<AssetCacheEntry> EntryOfHandle(const AssetHandle<T>& handle)
        {
            return EntryOfHandleField(&handle);
        }
    }

    /// @brief The set of assets a single spawn introduced that are not yet resident.
    ///
    /// A spawn yields exactly the handles it built or referenced that have not finished
    /// loading — the request-scoped residency token (Unreal's FStreamableHandle, Unity's
    /// AsyncOperationHandle). It snapshots the pending assets' cache entries (keeping them
    /// resident-bound while the batch is alive) and polls their residency; it is **not** a
    /// global "is the asset system idle" drain. A loading screen aggregates the batches it
    /// cares about and reads their combined progress; the blocking smoke/tooling path waits
    /// on one. An empty batch is already resident.
    ///
    /// The batch is scoped to **one** spawn: content a simulation system spawns later (a
    /// player spawned at OnStart) carries its own batch from its own SpawnInto call, owned
    /// by the spawning system, not the level's. The level blocks only on its world prefab's
    /// batch — pushing residency into the tick loop is out of scope.
    class ResidencyBatch
    {
    public:
        /// @brief Constructs an empty (already-resident) batch.
        ResidencyBatch() = default;

        /// @brief Adds a handle's cache entry to the batch if it is not yet resident.
        ///
        /// A resident or empty handle contributes nothing — only a pending handle is tracked
        /// (and kept resident-bound through the batch's lifetime).
        /// @tparam T  The asset type the handle refers to.
        /// @param handle  The handle to track until resident.
        template <typename T>
        void Track(const AssetHandle<T>& handle)
        {
            if (handle.IsLoaded())
            {
                return;
            }
            if (Ref<Detail::AssetCacheEntry> entry = Detail::EntryOfHandle(handle))
            {
                m_Pending.push_back(std::move(entry));
            }
        }

        /// @brief Adds a type-erased AssetHandle field's cache entry to the batch if it is pending.
        ///
        /// The reflection-walk counterpart of Track: a generic walk over live components reaches a
        /// FieldClass::AssetHandle field as raw bytes, with no T to name. A resident or empty field
        /// contributes nothing.
        /// @param fieldPtr  The first byte of an AssetHandle field (its AssetId, at offset 0).
        void TrackHandleField(const void* fieldPtr)
        {
            if (Ref<Detail::AssetCacheEntry> entry = Detail::EntryOfHandleField(fieldPtr);
                entry != nullptr && entry->Resource == nullptr)
            {
                m_Pending.push_back(std::move(entry));
            }
        }

        /// @brief Returns true once every tracked asset is resident (an empty batch is resident).
        [[nodiscard]] bool IsResident() const;

        /// @brief Returns the number of tracked assets that are now resident.
        [[nodiscard]] usize ResidentCount() const;

        /// @brief Returns the total number of assets the batch tracks.
        [[nodiscard]] usize TotalCount() const { return m_Pending.size(); }

        /// @brief Returns true when the batch tracks no pending asset.
        [[nodiscard]] bool IsEmpty() const { return m_Pending.empty(); }

        /// @brief Blocks until every tracked asset is resident, pumping the task system.
        ///
        /// Pumps @p tasks each iteration so off-thread upload continuations land on the main
        /// thread, then yields briefly. An empty batch returns immediately. Used by the smoke
        /// path and any loading screen that wants to gate on a spawn before proceeding.
        ///
        /// @warning Carries no timeout: a handle that never becomes resident (a failed or
        /// dropped load) hangs this loop. A watchdog aborts with a VE_ASSERT after a bounded
        /// number of pumps — an abort is easier to diagnose than a silent hang, and this loop
        /// is reachable from editor Play and any loading screen.
        /// @param tasks  The task system pumped each iteration so continuations land.
        void WaitResident(TaskSystem& tasks);

    private:
        /// @brief Cache entries of the pending assets, kept resident-bound while the batch lives.
        vector<Ref<Detail::AssetCacheEntry>> m_Pending;
    };
}
