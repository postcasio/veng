#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

// AssetHandle<T> / WeakAssetHandle<T> (planset-5 plan 04): typed, refcounted
// indirection into AssetManager's cache. AssetManager itself owns the cache
// (map<AssetId, Ref<AssetCacheEntry>>); handles share that entry via Ref/WeakRef
// copies. This indirection is what later enables hot-reload (swap the entry's
// Resource behind existing handles) — Reload() itself is not implemented this
// planset.

namespace Veng
{
    class AssetManager;

    namespace Detail
    {
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
    // each concrete asset type specializes it (RawAsset.h, and 06-09 add their
    // own).
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
    };

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
}
