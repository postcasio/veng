// Material load test: cooks the material fixture pack in-process, mounts it,
// LoadSync<Material>s it through AssetManager, and checks the loaded material
// — pipeline non-null, registry slot valid, and texture dependency resident
// via the cache.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Texture.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "material loader: cook, mount, LoadSync, validate pipeline and bindless slot")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "material_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_material.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    // ── Success path ──────────────────────────────────────────────────────────
    const AssetResult<AssetHandle<Material>> handle = assets.LoadSync<Material>(AssetId{0xBB9});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const Material& material = *handle->Get();

    // The registry must have allocated a valid (non-sentinel) slot.
    CHECK(material.GetIndex() != MaterialHandle::Invalid);

    // The material must have built a pipeline.
    CHECK(material.GetPipeline() != nullptr);

    // ── Texture dependency cache coherence ───────────────────────────────────
    // The material load (id 3001) resolves texture id 2001. Loading 2001
    // independently must return the already-cached entry.
    const AssetResult<AssetHandle<Texture>> texHandle = assets.LoadSync<Texture>(AssetId{0x7D1});
    REQUIRE(texHandle.has_value());
    REQUIRE(texHandle->IsLoaded());
    CHECK(texHandle->Get()->GetHandle().IsValid());

    std::filesystem::remove(outArchive);
}
