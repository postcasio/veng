#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <cstddef>
#include <cstring>

namespace Veng
{
    class AssetManager;

    template <typename T>
    class AssetHandle;

    namespace Detail
    {
        template <typename T>
        struct AssetHandleLayoutGuard;

        /// @brief Type-erased owner of a loaded asset's engine resource.
        ///
        /// AssetHandle<T>::Get() downcasts via static_cast — safe because AssetManager
        /// only ever stores the Ref<T> matching AssetTypeTrait<T>::Type for a given cache entry.
        using RefAny = Ref<void>;

        /// @brief One cached asset entry shared by all AssetHandle<T> copies for a given id.
        ///
        /// AssetManager's cache map holds the "anchor" Ref; AssetHandle<T> copies share it.
        /// CollectGarbage() evicts entries where the manager's own copy is the only one left
        /// (use_count() == 1) by dropping Resource, which retires the underlying engine resource
        /// through the per-frame deferred-destruction path.
        struct AssetCacheEntry
        {
            /// @brief The asset's identity.
            AssetId Id;
            /// @brief The asset's type (used to validate typed handle downcasts).
            AssetType Type;
            /// @brief Null until load + finalize completes.
            RefAny Resource;
        };
    }

    /// @brief Maps an asset C++ type T to its AssetType enum value.
    ///
    /// Each concrete asset type specializes this trait. AssetManager::LoadSync<T>/Get<T>
    /// use it to derive the AssetType for a typed request.
    template <typename T>
    struct AssetTypeTrait;

    template <typename T>
    class WeakAssetHandle;

    /// @brief Typed, refcounted indirection into AssetManager's cache.
    ///
    /// Copies share the underlying cache entry and keep the asset resident; dropping the
    /// last handle makes the asset evictable on the next CollectGarbage(), not immediately freed.
    template <typename T>
    class AssetHandle
    {
    public:
        /// @brief Constructs an empty (null) handle.
        AssetHandle() = default;

        /// @brief Returns true when the asset is resident (loaded and finalized).
        [[nodiscard]] bool IsLoaded() const
        {
            return m_Entry != nullptr && m_Entry->Resource != nullptr;
        }

        /// @brief Returns the asset's id (may be invalid for runtime-adopted resources).
        [[nodiscard]] AssetId Id() const { return m_Id; }

        /// @brief Returns the asset pointer, or nullptr when not yet resident.
        [[nodiscard]] const T* Get() const
        {
            return m_Entry ? static_cast<const T*>(m_Entry->Resource.get()) : nullptr;
        }

        /// @brief Dereferences the handle.
        /// @pre IsLoaded() — asserts if the asset is not resident.
        const T* operator->() const
        {
            VE_ASSERT(IsLoaded(), "AssetHandle::operator->: asset {} is not resident", m_Id.Value);
            return Get();
        }

        /// @brief Returns true when the asset is resident.
        explicit operator bool() const { return IsLoaded(); }

    private:
        friend class AssetManager;
        friend class WeakAssetHandle<T>;

        AssetHandle(AssetId id, Ref<Detail::AssetCacheEntry> entry) :
            m_Id(id), m_Entry(std::move(entry))
        {
        }

        AssetId m_Id;
        Ref<Detail::AssetCacheEntry> m_Entry;

        // m_Id must be at offset 0: the reflection serializer accesses the AssetId
        // directly off the field bytes. AssetHandleLayoutGuard enforces this.
        template <typename U>
        friend struct Detail::AssetHandleLayoutGuard;
    };

    namespace Detail
    {
        /// @brief Compile-time layout assertion for AssetHandle<T>.
        ///
        /// Befriended by AssetHandle<T> to access private members. Instantiated for
        /// each built-in AssetHandle leaf type to pin the offset contract the
        /// reflection serializer depends on.
        template <typename T>
        struct AssetHandleLayoutGuard
        {
            static_assert(offsetof(AssetHandle<T>, m_Id) == 0,
                          "AssetHandle<T>::m_Id must be the first member (offset 0) — "
                          "the reflection serializer reads the AssetId off offset 0");
            static_assert(offsetof(AssetId, Value) == 0,
                          "AssetId::Value must be at offset 0 — the reflection "
                          "serializer reads the raw u64 id off offset 0");
            static_assert(offsetof(AssetHandle<T>, m_Entry) == sizeof(AssetId),
                          "AssetHandle<T>::m_Entry must immediately follow the AssetId — "
                          "the prefab loader rehydrates a type-erased handle's cache entry "
                          "at this fixed offset");
        };
    }

    /// @brief Non-owning reference to a cached asset.
    ///
    /// Lock() promotes to an AssetHandle<T> while the entry is still resident, or
    /// returns nullopt once CollectGarbage() has evicted it. Does not keep the asset
    /// alive or count toward its refcount.
    template <typename T>
    class WeakAssetHandle
    {
    public:
        /// @brief Constructs an empty weak handle.
        WeakAssetHandle() = default;

        /// @brief Constructs a weak handle from a strong handle.
        explicit WeakAssetHandle(const AssetHandle<T>& handle) :
            m_Id(handle.m_Id), m_Entry(handle.m_Entry)
        {
        }

        /// @brief Returns the asset's id.
        [[nodiscard]] AssetId Id() const { return m_Id; }

        /// @brief Promotes to a strong handle if the entry is still resident, or returns nullopt.
        [[nodiscard]] optional<AssetHandle<T>> Lock() const
        {
            if (Ref<Detail::AssetCacheEntry> entry = m_Entry.lock())
                return AssetHandle<T>(m_Id, std::move(entry));

            return std::nullopt;
        }

    private:
        AssetId m_Id;
        WeakRef<Detail::AssetCacheEntry> m_Entry;
    };

    namespace Detail
    {
        // AssetHandle<T>'s layout is T-independent, so one instantiation pins
        // the offset constraint for every handle type.
        struct AssetHandleLayoutTag;
        template struct AssetHandleLayoutGuard<AssetHandleLayoutTag>;

        /// @brief Byte offset of the cache entry Ref within any AssetHandle<T>.
        ///
        /// AssetHandle<T>'s members are T-independent: { AssetId m_Id; Ref<AssetCacheEntry> m_Entry; }.
        /// sizeof(AssetId) is 8 and Ref is 8-aligned, so this offset is stable for every handle type —
        /// letting the prefab loader rehydrate a type-erased AssetHandle field without naming its T.
        inline constexpr usize AssetHandleEntryOffset = sizeof(AssetId);

        /// @brief Writes an id and resolved cache entry into a type-erased, default-constructed AssetHandle field.
        ///
        /// Used by the prefab loader's spawn to rehydrate an embedded handle from its cooked AssetId.
        /// A null entry leaves the handle empty (the "no asset" case).
        inline void RehydrateHandleField(void* handlePtr, AssetId id, Ref<AssetCacheEntry> entry)
        {
            std::memcpy(handlePtr, &id, sizeof(id));
            auto* entrySlot = reinterpret_cast<Ref<AssetCacheEntry>*>(
                static_cast<u8*>(handlePtr) + AssetHandleEntryOffset);
            *entrySlot = std::move(entry);
        }
    }
}
