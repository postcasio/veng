#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <cstddef>
#include <cstring>

// AssetHandle<T> / WeakAssetHandle<T>: typed, refcounted indirection into
// AssetManager's cache. AssetManager itself owns the cache
// (map<AssetId, Ref<AssetCacheEntry>>); handles share that entry via Ref/WeakRef
// copies. This indirection allows hot-reload (swap the entry's Resource behind
// existing handles) without invalidating outstanding handles.

namespace Veng
{
    class AssetManager;

    template <typename T>
    class AssetHandle;

    namespace Detail
    {
        template <typename T>
        struct AssetHandleLayoutGuard;

        // Type-erased owner of a loaded asset's engine resource (e.g. Ref<RawAsset>,
        // a future Ref<Texture>, ...). AssetHandle<T>::Get() downcasts via
        // static_cast — safe because AssetManager only ever stores the Ref<T>
        // matching AssetTypeTrait<T>::Type for a given cache entry.
        using RefAny = Ref<void>;

        // One cached asset. AssetManager's cache map holds the "anchor" Ref;
        // AssetHandle<T> copies share it. CollectGarbage() evicts entries where
        // the manager's own copy is the only one left (use_count() == 1) by
        // dropping Resource, which retires the underlying engine resource
        // through the existing per-frame deferred-destruction path.
        struct AssetCacheEntry
        {
            AssetId Id;
            AssetType Type;
            RefAny Resource;
        };
    }

    // AssetManager::LoadSync<T>/Get<T> map T -> AssetType through this trait;
    // each concrete asset type specializes it.
    template <typename T>
    struct AssetTypeTrait;

    template <typename T>
    class WeakAssetHandle;

    // Typed, refcounted indirection into AssetManager's cache. Copies share the
    // underlying cache entry and keep it resident; dropping the last handle
    // makes the asset evictable on the next CollectGarbage(), not immediately
    // freed.
    template <typename T>
    class AssetHandle
    {
    public:
        AssetHandle() = default;

        [[nodiscard]] bool IsLoaded() const
        {
            return m_Entry != nullptr && m_Entry->Resource != nullptr;
        }

        [[nodiscard]] AssetId Id() const { return m_Id; }

        // nullptr until resident.
        [[nodiscard]] const T* Get() const
        {
            return m_Entry ? static_cast<const T*>(m_Entry->Resource.get()) : nullptr;
        }

        // Asserts resident (engine contract: callers check IsLoaded()/operator
        // bool first).
        const T* operator->() const
        {
            VE_ASSERT(IsLoaded(), "AssetHandle::operator->: asset {} is not resident", m_Id.Value);
            return Get();
        }

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

        // The reflection serializer (WriteFields/ReadFields) records an
        // AssetHandle field by reading/writing its leading u64 AssetId straight
        // off the field bytes — it relies on m_Id (an AssetId, itself a leading
        // u64) sitting at offset 0 of the handle. Pin it through a friend so
        // reordering a member is a loud compile error, not a silent prefab
        // corruption.
        template <typename U>
        friend struct Detail::AssetHandleLayoutGuard;
    };

    namespace Detail
    {
        // Befriended by AssetHandle<T> so its offsetof can read the private m_Id;
        // instantiated below for each builtin AssetHandle leaf.
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

    // A non-owning reference to a cached asset. Lock() promotes to an
    // AssetHandle<T> while the entry is still resident, or returns nullopt once
    // CollectGarbage() has evicted it. Does not keep the asset alive or count
    // toward its refcount.
    template <typename T>
    class WeakAssetHandle
    {
    public:
        WeakAssetHandle() = default;

        explicit WeakAssetHandle(const AssetHandle<T>& handle) :
            m_Id(handle.m_Id), m_Entry(handle.m_Entry)
        {
        }

        [[nodiscard]] AssetId Id() const { return m_Id; }

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
        // AssetHandle<T>'s layout is T-independent (an AssetId + a Ref<void>),
        // so one instantiation pins the offset for every handle type.
        struct AssetHandleLayoutTag;
        template struct AssetHandleLayoutGuard<AssetHandleLayoutTag>;

        // AssetHandle<T>'s members are T-independent: { AssetId m_Id;
        // Ref<AssetCacheEntry> m_Entry; }. The cache entry Ref sits immediately
        // after the leading AssetId (offset 0, pinned by the layout guard above).
        // sizeof(AssetId) is 8 and Ref is 8-aligned, so this offset is stable for
        // every handle type — letting the prefab loader rehydrate a type-erased
        // AssetHandle field by id without naming its concrete T.
        inline constexpr usize AssetHandleEntryOffset = sizeof(AssetId);

        // Writes an id + resolved cache entry into the type-erased AssetHandle
        // field at handlePtr (default-constructed: a null entry Ref). Used by the
        // prefab loader's spawn to rehydrate an embedded handle from its cooked
        // AssetId. A null entry leaves the handle empty (the "no asset" case).
        inline void RehydrateHandleField(void* handlePtr, AssetId id, Ref<AssetCacheEntry> entry)
        {
            std::memcpy(handlePtr, &id, sizeof(id));
            auto* entrySlot = reinterpret_cast<Ref<AssetCacheEntry>*>(
                static_cast<u8*>(handlePtr) + AssetHandleEntryOffset);
            *entrySlot = std::move(entry);
        }
    }
}
