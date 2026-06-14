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
#include <Veng/Renderer/Context.h>

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
        writer.Add(AssetId{1001}, AssetType::Raw, Bytes({1, 2, 3, 4}));
        writer.Add(AssetId{1002}, AssetType::Texture, Bytes({0xAB}));
        writer.Add(AssetId{1003}, AssetType::Raw, Bytes({9}));

        const path archivePath = std::filesystem::temp_directory_path() / "veng_asset_manager_unit.vengpack";
        const VoidResult written = writer.Write(archivePath);
        REQUIRE(written.has_value());
        return archivePath;
    }
}

TEST_CASE("AssetManager: LoadSync<RawAsset> resolves a mounted asset")
{
    const path archivePath = WriteFixtureArchive();

    Renderer::Context context;
    AssetManager manager(context);

    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<RawAsset>> loaded = manager.LoadSync<RawAsset>(AssetId{1001});
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
    AssetManager manager(context);

    REQUIRE(manager.Mount(archivePath).has_value());

    const AssetResult<AssetHandle<RawAsset>> missing = manager.LoadSync<RawAsset>(AssetId{9999});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().Kind == AssetError::NotFound);
    CHECK(missing.error().Id == AssetId{9999});

    // 1002 is cooked as AssetType::Texture; requesting it as RawAsset (Raw) is
    // a type mismatch.
    const AssetResult<AssetHandle<RawAsset>> wrongType = manager.LoadSync<RawAsset>(AssetId{1002});
    REQUIRE_FALSE(wrongType.has_value());
    CHECK(wrongType.error().Kind == AssetError::WrongType);

    std::filesystem::remove(archivePath);
}

TEST_CASE("AssetManager: Mount is idempotent, Unmount drops resolution for uncached ids")
{
    const path archivePath = WriteFixtureArchive();

    Renderer::Context context;
    AssetManager manager(context);

    REQUIRE(manager.Mount(archivePath).has_value());
    REQUIRE(manager.Mount(archivePath).has_value()); // no-op, not a duplicate TOC

    // Cache 1001 before unmounting.
    CHECK(manager.LoadSync<RawAsset>(AssetId{1001}).has_value());

    manager.Unmount(archivePath);

    // Already cached -> still resolves without touching the (now unmounted)
    // archive.
    CHECK(manager.LoadSync<RawAsset>(AssetId{1001}).has_value());

    // Never cached and no longer mounted -> NotFound.
    const AssetResult<AssetHandle<RawAsset>> afterUnmount = manager.LoadSync<RawAsset>(AssetId{1003});
    REQUIRE_FALSE(afterUnmount.has_value());
    CHECK(afterUnmount.error().Kind == AssetError::NotFound);

    std::filesystem::remove(archivePath);
}

TEST_CASE("AssetManager: dropping the last AssetHandle makes the entry evictable")
{
    const path archivePath = WriteFixtureArchive();

    Renderer::Context context;
    AssetManager manager(context);

    REQUIRE(manager.Mount(archivePath).has_value());

    WeakAssetHandle<RawAsset> weak;
    {
        const AssetResult<AssetHandle<RawAsset>> loaded = manager.LoadSync<RawAsset>(AssetId{1001});
        REQUIRE(loaded.has_value());

        weak = WeakAssetHandle<RawAsset>(*loaded);
        CHECK(manager.Get<RawAsset>(AssetId{1001}).has_value());
        CHECK(weak.Lock().has_value());

        // CollectGarbage is a no-op while `loaded` (and the cache's own copy)
        // keep the entry's refcount above 1.
        manager.CollectGarbage();
        CHECK(manager.Get<RawAsset>(AssetId{1001}).has_value());
    }

    // Only the cache's copy remains -> evictable.
    manager.CollectGarbage();
    CHECK_FALSE(manager.Get<RawAsset>(AssetId{1001}).has_value());
    CHECK_FALSE(weak.Lock().has_value());

    std::filesystem::remove(archivePath);
}
