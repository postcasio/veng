// AssetManager unit cases: mount/resolve/load/cache/GC, exercised against the
// built-in AssetType::Raw loader so the path is testable (no GPU). No Vulkan
// call is made —
// Context is default-constructed (not Initialize()d) and never touched by the
// Raw loader, so this runs with no ICD present.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/RawAsset.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    vector<u8> Bytes(std::initializer_list<u8> values)
    {
        return vector<u8>(values.begin(), values.end());
    }

    path WriteFixtureArchive()
    {
        ArchiveWriter writer;
        writer.Add(AssetId{0x3E9}, AssetType::Raw, Bytes({1, 2, 3, 4}));
        writer.Add(AssetId{0x3EA}, AssetType::Texture, Bytes({0xAB}));
        writer.Add(AssetId{0x3EB}, AssetType::Raw, Bytes({9}));

        const path archivePath =
            std::filesystem::temp_directory_path() / "veng_asset_manager_unit.vengpack";
        const VoidResult written = writer.Write(archivePath);
        REQUIRE(written.has_value());
        return archivePath;
    }
}

TEST_CASE("AssetManager: LoadSync<RawAsset> resolves a mounted asset")
{
    const path archivePath = WriteFixtureArchive();

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<RawAsset>> loaded = manager.LoadSync<RawAsset>(AssetId{0x3E9});
    REQUIRE(loaded.has_value());
    CHECK(loaded->IsLoaded());
    CHECK(static_cast<bool>(*loaded));
    CHECK(std::ranges::equal((*loaded)->Bytes, Bytes({1, 2, 3, 4})));

    std::filesystem::remove(archivePath);
}

TEST_CASE("AssetManager: LoadSync error kinds — NotFound and WrongType")
{
    const path archivePath = WriteFixtureArchive();

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<RawAsset>> missing = manager.LoadSync<RawAsset>(AssetId{0x270F});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().Kind == AssetError::NotFound);
    CHECK(missing.error().Id == AssetId{0x270F});

    // 0x3EA is cooked as AssetType::Texture; requesting it as RawAsset (Raw) is
    // a type mismatch.
    const AssetResult<AssetHandle<RawAsset>> wrongType = manager.LoadSync<RawAsset>(AssetId{0x3EA});
    REQUIRE_FALSE(wrongType.has_value());
    CHECK(wrongType.error().Kind == AssetError::WrongType);

    std::filesystem::remove(archivePath);
}

TEST_CASE("AssetManager: Mount is idempotent, Unmount drops resolution for uncached ids")
{
    const path archivePath = WriteFixtureArchive();

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    REQUIRE(manager.Mount(archivePath).has_value());
    REQUIRE(manager.Mount(archivePath).has_value()); // no-op, not a duplicate TOC

    // Cache 0x3E9 before unmounting.
    CHECK(manager.LoadSync<RawAsset>(AssetId{0x3E9}).has_value());

    manager.Unmount(archivePath);

    // Already cached -> still resolves without touching the (now unmounted)
    // archive.
    CHECK(manager.LoadSync<RawAsset>(AssetId{0x3E9}).has_value());

    // Never cached and no longer mounted -> NotFound.
    const AssetResult<AssetHandle<RawAsset>> afterUnmount =
        manager.LoadSync<RawAsset>(AssetId{0x3EB});
    REQUIRE_FALSE(afterUnmount.has_value());
    CHECK(afterUnmount.error().Kind == AssetError::NotFound);

    std::filesystem::remove(archivePath);
}

TEST_CASE(
    "AssetManager: CreateAsync returns a pending handle that becomes resident through the pump")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    // A factory that resolves on a worker; the continuation lands the resource on
    // the main thread during PumpMainThread().
    Task<Ref<RawAsset>> factory = tasks.Submit(
        []
        {
            auto raw = CreateRef<RawAsset>();
            raw->Bytes = Bytes({7, 8, 9});
            return raw;
        });

    const AssetHandle<RawAsset> handle = manager.CreateAsync<RawAsset>(std::move(factory));

    // Pending: not resident, null pointer, and the invalid (runtime) id.
    CHECK_FALSE(handle.IsLoaded());
    CHECK(handle.Get() == nullptr);
    CHECK_FALSE(handle.Id().IsValid());

    // Drain the worker, then run the pump so the continuation finalizes the entry.
    tasks.WaitForAll();
    tasks.PumpMainThread();

    // Resident: the resource is reachable through the same handle.
    CHECK(handle.IsLoaded());
    REQUIRE(handle.Get() != nullptr);
    CHECK(std::ranges::equal(handle->Bytes, Bytes({7, 8, 9})));
}

TEST_CASE("AssetManager: a pending CreateAsync entry is detached and survives CollectGarbage")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    Task<Ref<RawAsset>> factory = tasks.Submit(
        []
        {
            auto raw = CreateRef<RawAsset>();
            raw->Bytes = Bytes({1});
            return raw;
        });

    const AssetHandle<RawAsset> handle = manager.CreateAsync<RawAsset>(std::move(factory));

    // Detached: the invalid id is never inserted into the AssetId map.
    CHECK(manager.CachedEntry(AssetId{}) == nullptr);

    // The keep-alive holds a still-pending entry off eviction even with no resident
    // resource and even if the worker has not finished.
    manager.CollectGarbage();

    tasks.WaitForAll();
    tasks.PumpMainThread();
    CHECK(handle.IsLoaded());

    // Resolved: still detached, still kept alive by the live handle.
    CHECK(manager.CachedEntry(AssetId{}) == nullptr);
    manager.CollectGarbage();
    CHECK(handle.IsLoaded());
}

TEST_CASE("AssetManager: a resolved CreateAsync entry is freed once its last handle drops")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    WeakAssetHandle<RawAsset> weak;
    {
        Task<Ref<RawAsset>> factory = tasks.Submit(
            []
            {
                auto raw = CreateRef<RawAsset>();
                raw->Bytes = Bytes({2});
                return raw;
            });

        const AssetHandle<RawAsset> handle = manager.CreateAsync<RawAsset>(std::move(factory));
        weak = WeakAssetHandle<RawAsset>(handle);

        tasks.WaitForAll();
        tasks.PumpMainThread();
        REQUIRE(handle.IsLoaded());
        CHECK(weak.Lock().has_value());

        // The handle keeps it alive across a collect.
        manager.CollectGarbage();
        CHECK(weak.Lock().has_value());
    }

    // The last handle is gone and the keep-alive was dropped on finalize, so the
    // detached entry is collectible — exactly the Adopt lifetime.
    manager.CollectGarbage();
    CHECK_FALSE(weak.Lock().has_value());
}

TEST_CASE("AssetManager: dropping the last AssetHandle makes the entry evictable")
{
    const path archivePath = WriteFixtureArchive();

    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager manager(context, tasks, types);

    REQUIRE(manager.Mount(archivePath).has_value());

    WeakAssetHandle<RawAsset> weak;
    {
        const AssetResult<AssetHandle<RawAsset>> loaded =
            manager.LoadSync<RawAsset>(AssetId{0x3E9});
        REQUIRE(loaded.has_value());

        weak = WeakAssetHandle<RawAsset>(*loaded);
        CHECK(manager.Get<RawAsset>(AssetId{0x3E9}).has_value());
        CHECK(weak.Lock().has_value());

        // CollectGarbage is a no-op while `loaded` (and the cache's own copy)
        // keep the entry's refcount above 1.
        manager.CollectGarbage();
        CHECK(manager.Get<RawAsset>(AssetId{0x3E9}).has_value());
    }

    // Only the cache's copy remains -> evictable.
    manager.CollectGarbage();
    CHECK_FALSE(manager.Get<RawAsset>(AssetId{0x3E9}).has_value());
    CHECK_FALSE(weak.Lock().has_value());

    std::filesystem::remove(archivePath);
}
