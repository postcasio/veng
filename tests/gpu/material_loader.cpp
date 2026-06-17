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

    // GetFields exposes the reflected field table: the 3 declared fields
    // (Albedo / AlbedoSampler handles, Factors authored param), pads omitted.
    const std::span<const MaterialField> fields = material.GetFields();
    CHECK(fields.size() == 3);
    bool sawFactors = false;
    for (const MaterialField& f : fields)
    {
        if (f.Name == "Factors")
        {
            sawFactors = true;
            CHECK(f.Kind == MaterialField::FieldKind::Param);
        }
    }
    CHECK(sawFactors);

    // ── Texture dependency cache coherence ───────────────────────────────────
    // The material load (id 3001) resolves texture id 2001. Loading 2001
    // independently must return the already-cached entry.
    const AssetResult<AssetHandle<Texture>> texHandle = assets.LoadSync<Texture>(AssetId{0x7D1});
    REQUIRE(texHandle.has_value());
    REQUIRE(texHandle->IsLoaded());
    CHECK(texHandle->Get()->GetHandle().IsValid());

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "material loader: a handles-only material loads with a zero-size authored block")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "material_handles_only_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_material_handles_only.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Material>> handle = assets.LoadSync<Material>(AssetId{3102});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const Material& material = *handle->Get();
    CHECK(material.GetIndex() != MaterialHandle::Invalid);
    CHECK(material.GetPipeline() != nullptr);
    CHECK(material.GetFields().size() == 2); // only handle fields

    std::filesystem::remove(outArchive);
}
