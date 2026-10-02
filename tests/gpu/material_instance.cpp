// MaterialInstance round-trip + parent/instance split.
//
// Cooks the brick g-buffer fixture (a Surface parent Material, id 0x232B, whose pack declares a
// `defaultInstance` id 0x895443), then:
//   - hand-builds a CookedMaterialInstance blob over that parent overriding the exposed
//     BaseColorFactor vec4, mounts it, LoadSync<MaterialInstance>s it, and asserts the
//     instance binds the parent's pipeline (pointer-equal), owns a DISTINCT SSBO slot from
//     the cooked default instance, and inherits the parent's schema;
//   - asserts the parent Material id and its default-instance id are distinct assets that each
//     resolve under their own type, and that a MaterialInstance request for the bare parent id
//     is now NotFound (the parent-id default-instance bridge is gone);
//   - loads the cooked default instance twice and asserts the same cached instance (its pipeline
//     and parent pointer-equal) — the per-id cache;
//   - builds a runtime MID (Build<MaterialInstance>(parent) + a per-frame SetParam) to prove
//     the stall-free override write path.
//
// Cooker-gated (it cooks the brick fixture).

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>
#include "support/TempPath.h"

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
    // The companion zero-override default instance the material's `defaultInstance` key emits.
    constexpr AssetId BrickDefaultInstanceId{0x895443ULL}; // 9000003
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
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_material_instance.vengpack";

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
    writer.Add(InstanceId, AssetTypes::MaterialInstance, instanceBlob);
    const MountHandle instanceMount = assets.MountMemory(writer.Build(), "<test instance>");

    // The explicit instance.
    const AssetResult<AssetHandle<MaterialInstance>> instance =
        assets.LoadSync<MaterialInstance>(InstanceId);
    if (!instance.has_value())
    {
        FAIL(instance.error().Detail);
    }
    REQUIRE(instance->IsLoaded());

    // The cooked zero-override default instance the pack emitted beside the parent.
    const AssetResult<AssetHandle<MaterialInstance>> defaultInstance =
        assets.LoadSync<MaterialInstance>(BrickDefaultInstanceId);
    REQUIRE(defaultInstance.has_value());
    REQUIRE(defaultInstance->IsLoaded());

    // The parent id and its default-instance id are distinct assets, each resolving under its
    // own type: the parent id is a Material, the default-instance id a MaterialInstance.
    CHECK(BrickParentId.Value != BrickDefaultInstanceId.Value);
    const AssetResult<AssetHandle<Material>> parentTyped = assets.LoadSync<Material>(BrickParentId);
    REQUIRE(parentTyped.has_value());

    // The parent-id default-instance bridge is gone: a MaterialInstance request for the bare
    // parent Material id is an ordinary NotFound, not a synthesized default instance.
    const AssetResult<AssetHandle<MaterialInstance>> bridged =
        assets.LoadSync<MaterialInstance>(BrickParentId);
    REQUIRE_FALSE(bridged.has_value());
    CHECK(bridged.error().Kind == AssetError::WrongType);

    const MaterialInstance& inst = *instance->Get();
    const MaterialInstance& def = *defaultInstance->Get();

    // The instance borrows the parent's pipeline and reflected schema.
    CHECK(inst.GetPipeline() != nullptr);
    CHECK(inst.GetDomain() == MaterialDomain::Surface);
    CHECK_FALSE(inst.GetFields().empty());

    // A distinct SSBO slot from the cooked default instance.
    CHECK(inst.GetBlockOffset() != MaterialHandle::Invalid);
    CHECK(def.GetBlockOffset() != MaterialHandle::Invalid);
    CHECK(inst.GetBlockOffset() != def.GetBlockOffset());

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "material instance: a cold async load finalizes behind its pending parent")
{
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_gpu_material_instance_async.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    // The parent material's texture uploads through the async transfer path.
    Context.InitializeTransferPools(Tasks);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // A genuinely cold async request: nothing is resident. The instance parses on a worker, so its
    // parent Material is fanned out only when that parse lands, and is still pending when the
    // instance is constructed.
    const AssetHandle<MaterialInstance> handle =
        assets.Load<MaterialInstance>(BrickDefaultInstanceId);
    CHECK_FALSE(handle.IsLoaded());
    CHECK(assets.CachedEntry(BrickParentId) == nullptr);

    for (int i = 0; i < 100 && !handle.IsLoaded(); ++i)
    {
        Tasks.WaitForAll();
        Tasks.PumpMainThread();
        assets.PumpFinalizes();
    }

    // The dependency ordering finalized the parent first, so the instance's block was seeded from
    // a patched default block and it owns a real slot.
    REQUIRE(handle.IsLoaded());
    CHECK(handle.Get()->GetParent().IsLoaded());
    CHECK(handle.Get()->GetPipeline() != nullptr);
    CHECK(handle.Get()->GetBlockOffset() != MaterialHandle::Invalid);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "material instance: the cooked default instance caches by id")
{
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_gpu_material_instance_share.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // Loading the same default-instance id twice hits one cached instance.
    const AssetResult<AssetHandle<MaterialInstance>> a =
        assets.LoadSync<MaterialInstance>(BrickDefaultInstanceId);
    const AssetResult<AssetHandle<MaterialInstance>> b =
        assets.LoadSync<MaterialInstance>(BrickDefaultInstanceId);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    // The same cached instance, so the same parent pipeline (pointer-equal).
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
        Veng::TestSupport::TempDir() / "veng_gpu_material_instance_mid.vengpack";

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
    CHECK(mid.Get()->GetBlockOffset() != MaterialHandle::Invalid);

    // A per-frame SetParam is a direct, stall-free ring-buffer write (no WaitIdle).
    const_cast<MaterialInstance&>(*mid.Get())
        .SetParam("BaseColorFactor", vec4(1.0f, 0.0f, 0.0f, 1.0f));

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "material fields: a name resolves once, and a texture field names its sampler")
{
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_gpu_material_instance_fields.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(BrickParentId);
    REQUIRE(parent.has_value());
    const Material& material = *parent->Get();
    const std::span<const MaterialField> fields = material.GetFields();

    // A resolved handle names the field the linear scan would have found, for each kind the
    // setters write: a param, a texture handle, and a sampler handle.
    const auto scan = [fields](std::string_view name) -> const MaterialField*
    {
        for (const MaterialField& field : fields)
        {
            if (field.Name == name)
            {
                return &field;
            }
        }
        return nullptr;
    };
    for (const std::string_view name : {"BaseColorFactor", "BaseColor", "BaseColorSampler"})
    {
        const MaterialFieldHandle handle = material.Field(name);
        REQUIRE(handle.IsValid());
        CHECK(handle.Parent == &material);
        const MaterialField* scanned = scan(name);
        REQUIRE(scanned != nullptr);
        CHECK(&fields[handle.Index] == scanned);
    }

    // A name the schema does not carry resolves to an invalid handle rather than a fatal, so a
    // consumer probing for an optional field branches on it.
    CHECK_FALSE(material.Field("NoSuchField").IsValid());

    // A TextureHandle field carries the index of its <name>Sampler companion; a sampler field and
    // a plain param carry none.
    const MaterialFieldHandle baseColor = material.Field("BaseColor");
    const MaterialFieldHandle baseColorSampler = material.Field("BaseColorSampler");
    REQUIRE(fields[baseColor.Index].PairedSampler != MaterialFieldHandle::Invalid);
    CHECK(fields[baseColor.Index].PairedSampler == baseColorSampler.Index);
    CHECK(fields[baseColorSampler.Index].PairedSampler == MaterialFieldHandle::Invalid);
    CHECK(fields[material.Field("BaseColorFactor").Index].PairedSampler ==
          MaterialFieldHandle::Invalid);

    // An instance delegates the resolve to its parent, so one handle addresses the same field in
    // every instance of that parent — which is what makes it hoistable out of a per-frame write.
    const AssetHandle<MaterialInstance> instance =
        assets.BuildSync<MaterialInstance>(MaterialInstanceInfo{
            .Name = "Field Handles", .Context = &Context, .Parent = *parent, .Overrides = {}});
    REQUIRE(instance.IsLoaded());
    const MaterialFieldHandle viaInstance = instance.Get()->Field("BaseColorFactor");
    CHECK(viaInstance.Index == material.Field("BaseColorFactor").Index);
    CHECK(viaInstance.Parent == &material);

    // Each kind's handle overload writes through, so a bad bound or kind check would fault here.
    const u32 revision = instance.Get()->GetRevision();
    instance.Get()->SetParam(viaInstance, vec4(0.25f, 0.5f, 0.75f, 1.0f));
    instance.Get()->SetParam(material.Field("RoughnessFactor"), 0.5f);
    instance.Get()->SetTextureHandle(baseColor, TextureHandle{.Index = 3});
    instance.Get()->SetSamplerHandle(baseColorSampler, SamplerHandle{.Index = 2});
    CHECK(instance.Get()->GetRevision() == revision + 4);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "material instance: Clone is an independent per-instance copy")
{
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_gpu_material_instance_clone.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(BrickParentId);
    REQUIRE(parent.has_value());

    // A source instance with an established look, then a clone of it.
    const AssetHandle<MaterialInstance> source =
        assets.BuildSync<MaterialInstance>(MaterialInstanceInfo{
            .Name = "Clone Source", .Context = &Context, .Parent = *parent, .Overrides = {}});
    REQUIRE(source.IsLoaded());
    source.Get()->SetParam("BaseColorFactor", vec4(0.2f, 0.4f, 0.6f, 1.0f));

    const AssetHandle<MaterialInstance> clone =
        assets.Adopt<MaterialInstance>(source.Get()->Clone("Clone Copy"));
    REQUIRE(clone.IsLoaded());

    // The copy shares the parent (pipeline pointer-equal) but owns its own bindless slot, so it is
    // a distinct target the renderer selects separately.
    CHECK(clone.Get()->GetPipeline().get() == source.Get()->GetPipeline().get());
    CHECK(clone.Get()->GetBlockOffset() != MaterialHandle::Invalid);
    CHECK(clone.Get()->GetBlockOffset() != source.Get()->GetBlockOffset());

    // Mutating the copy touches only the copy: its revision moves while the source's holds, which is
    // the whole point — a write into a clone reaches no other drawer of the shared source.
    const u32 sourceRevision = source.Get()->GetRevision();
    const u32 cloneRevision = clone.Get()->GetRevision();
    clone.Get()->SetParam("BaseColorFactor", vec4(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK(clone.Get()->GetRevision() > cloneRevision);
    CHECK(source.Get()->GetRevision() == sourceRevision);

    std::filesystem::remove(outArchive);
}

#endif
