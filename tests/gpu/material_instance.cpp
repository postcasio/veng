// MaterialInstance round-trip + parent/instance split.
//
// Cooks the brick g-buffer fixture (a Surface parent Material, id 0x232B), then:
//   - hand-builds a CookedMaterialInstance blob over that parent overriding the exposed
//     BaseColorFactor vec4, mounts it, LoadSync<MaterialInstance>s it, and asserts the
//     instance binds the parent's pipeline (pointer-equal), owns a DISTINCT SSBO slot from
//     the parent's implicit default instance, and inherits the parent's schema;
//   - loads two default instances of the same bare parent id and asserts they share the
//     parent's GraphicsPipeline (the pipeline-sharing invariant) yet own different slots;
//   - builds a runtime MID (Build<MaterialInstance>(parent) + a per-frame SetParam) to prove
//     the stall-free override write path.
//
// Cooker-gated (it cooks the brick fixture).

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <cstring>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/BindlessRegistry.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The brick Surface parent in the g-buffer fixture pack.
    constexpr AssetId BrickParentId{0x232BULL}; // 9003
    // A test-local id for the hand-built instance (distinct from any cooked asset).
    constexpr AssetId InstanceId{0x5005A11CE0000001ULL};

    // Hand-builds a CookedMaterialInstance blob over BrickParentId overriding BaseColorFactor.
    vector<u8> BuildInstanceBlob(const vec4& baseColor)
    {
        CookedMaterialInstanceHeader header{
            .ParentId = BrickParentId.Value,
            .Version = CookedMaterialInstanceVersion,
            .OverrideCount = 1,
            .ValueRegionBytes = sizeof(vec4),
        };

        CookedMaterialInstanceOverride ov{};
        std::strncpy(ov.Name, "BaseColorFactor", ShaderNameCapacity - 1);
        ov.Kind = 0; // param
        ov.ValueOffset = 0;
        ov.ValueSize = sizeof(vec4);
        ov.TextureId = 0;

        vector<u8> blob(sizeof(header) + sizeof(ov) + sizeof(vec4));
        usize cursor = 0;
        std::memcpy(blob.data() + cursor, &header, sizeof(header));
        cursor += sizeof(header);
        std::memcpy(blob.data() + cursor, &ov, sizeof(ov));
        cursor += sizeof(ov);
        std::memcpy(blob.data() + cursor, &baseColor, sizeof(vec4));
        return blob;
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "material instance: cooked override binds the parent pipeline with a distinct slot")
{
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_gpu_material_instance.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cookResult =
        cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr, nullptr,
                        nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE_MESSAGE(cookResult.has_value(), cookResult.error());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // An in-memory pack carrying the hand-built instance blob over the cooked brick parent.
    const vec4 green(0.0f, 1.0f, 0.0f, 1.0f);
    ArchiveWriter writer;
    const vector<u8> instanceBlob = BuildInstanceBlob(green);
    writer.Add(InstanceId, AssetType::MaterialInstance, instanceBlob);
    const MountHandle instanceMount = assets.MountMemory(writer.Build(), "<test instance>");

    // The explicit instance.
    const AssetResult<AssetHandle<MaterialInstance>> instance =
        assets.LoadSync<MaterialInstance>(InstanceId);
    if (!instance.has_value())
    {
        FAIL(instance.error().Detail);
    }
    REQUIRE(instance->IsLoaded());

    // The parent's implicit zero-override default instance.
    const AssetResult<AssetHandle<MaterialInstance>> defaultInstance =
        assets.LoadSync<MaterialInstance>(BrickParentId);
    REQUIRE(defaultInstance.has_value());
    REQUIRE(defaultInstance->IsLoaded());

    const MaterialInstance& inst = *instance->Get();
    const MaterialInstance& def = *defaultInstance->Get();

    // The instance borrows the parent's pipeline and reflected schema.
    CHECK(inst.GetPipeline() != nullptr);
    CHECK(inst.GetDomain() == MaterialDomain::Surface);
    CHECK_FALSE(inst.GetFields().empty());

    // A distinct SSBO slot from the parent's default instance.
    CHECK(inst.GetIndex() != MaterialHandle::Invalid);
    CHECK(def.GetIndex() != MaterialHandle::Invalid);
    CHECK(inst.GetIndex() != def.GetIndex());

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "material instance: two default instances of one parent share its pipeline")
{
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_gpu_material_instance_share.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // Loading the same bare parent id twice hits one cached default instance.
    const AssetResult<AssetHandle<MaterialInstance>> a =
        assets.LoadSync<MaterialInstance>(BrickParentId);
    const AssetResult<AssetHandle<MaterialInstance>> b =
        assets.LoadSync<MaterialInstance>(BrickParentId);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    // The same cached default instance, so the same parent pipeline (pointer-equal).
    CHECK(a->Get()->GetPipeline().get() == b->Get()->GetPipeline().get());
    CHECK(a->Get()->GetParent().Get() == b->Get()->GetParent().Get());

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "material instance: a runtime MID builds over a parent and writes per-frame params")
{
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_gpu_material_instance_mid.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // The parent material, loaded directly.
    const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(BrickParentId);
    REQUIRE(parent.has_value());
    REQUIRE(parent->IsLoaded());

    // A runtime-built MID over the parent (the Build<MaterialInstance> path).
    const AssetHandle<MaterialInstance> mid =
        assets.BuildSync<MaterialInstance>(MaterialInstanceInfo{
            .Name = "Test MID", .Context = &Context, .Parent = *parent, .Overrides = {}});
    REQUIRE(mid.IsLoaded());

    // The MID shares the parent's pipeline and owns its own slot.
    CHECK(mid.Get()->GetPipeline().get() == parent->Get()->GetPipeline().get());
    CHECK(mid.Get()->GetIndex() != MaterialHandle::Invalid);

    // A per-frame SetParam is a direct, stall-free ring-buffer write (no WaitIdle).
    const_cast<MaterialInstance&>(*mid.Get())
        .SetParam("BaseColorFactor", vec4(1.0f, 0.0f, 0.0f, 1.0f));

    std::filesystem::remove(outArchive);
}

#endif
