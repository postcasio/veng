// The game-defined asset-type seam, exercised from the cooker.
//
// Proves the two halves meet: veng_test_module registers the type's identity and name through the
// runtime module ABI, veng_test_cook_module registers its importer through the cook module ABI,
// and only with both does a pack entry naming that type cook. Also covers the cook ABI handshake
// (a stale cook module is a located error whose entry never runs) and the sibling-path convention.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/CookModule.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Cook/Verify.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinSystems.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>

#include <filesystem>
#include <fstream>

#include "module/probe_component.h"
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // A one-entry pack naming the module-defined type. The type name — not an id — is what a
    // manifest carries, so it only resolves once the module's registrations are merged in.
    path WriteProbePack(const string& fileName)
    {
        const path packPath = Veng::TestSupport::TempDir() / fileName;
        std::ofstream out(packPath, std::ios::binary);
        out << R"({ "version": 1, "assets": [ { "id": "0x0000000000002041", "type": ")"
            << ProbeAssetTypeName << R"(", "value": 305419896 } ] })";
        return packPath;
    }
}

TEST_CASE("cook module: loads through its own ABI handshake and registers its importer")
{
    Result<LoadedCookModule> loaded = LoadCookModule(path{VENG_TEST_COOK_MODULE_PATH});
    REQUIRE_MESSAGE(loaded.has_value(),
                    "LoadCookModule failed: ", loaded ? string{} : loaded.error());

    REQUIRE(loaded->Importers.All().size() == 1);
    CHECK(loaded->Importers.All().front()->Type() == ProbeAssetType);
}

TEST_CASE("cook module: an ABI-mismatched cook module is a located Result error")
{
    const Result<LoadedCookModule> loaded = LoadCookModule(path{VENG_BAD_VERSION_COOK_MODULE_PATH});
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("cook module: a runtime module is not a cook module")
{
    // The runtime module exports VengModuleAbiVersion, not VengCookModuleAbiVersion — the two
    // contracts version independently, so loading one as the other fails at the handshake.
    const Result<LoadedCookModule> loaded = LoadCookModule(path{VENG_TEST_MODULE_PATH});
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("cook module: a nonexistent cook module path is a located Result error")
{
    const Result<LoadedCookModule> loaded =
        LoadCookModule(path{"this-cook-module-does-not-exist.dylib"});
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("cook module: the sibling path convention suffixes the stem, keeping dir and extension")
{
    CHECK(SiblingCookModulePath(path{"/build/libgame.dylib"}) == path{"/build/libgame_cook.dylib"});
    CHECK(SiblingCookModulePath(path{"/build/game.dll"}) == path{"/build/game_cook.dll"});
}

TEST_CASE("cook module: the runtime-module path inverts the sibling convention")
{
    CHECK(SiblingRuntimeModulePath(path{"/build/libgame_cook.dylib"}) ==
          optional<path>(path{"/build/libgame.dylib"}));
    CHECK(SiblingRuntimeModulePath(path{"/build/game_cook.dll"}) ==
          optional<path>(path{"/build/game.dll"}));

    // A stem that is not _cook-suffixed has no defined inverse, and a bare "_cook" would name
    // an empty runtime module.
    CHECK_FALSE(SiblingRuntimeModulePath(path{"/build/libgame.dylib"}).has_value());
    CHECK_FALSE(SiblingRuntimeModulePath(path{"/build/_cook.dylib"}).has_value());
}

TEST_CASE("cook module: a game type is unknown to a cooker that loaded no module")
{
    const path packPath = WriteProbePack("veng_cook_module_no_module_pack.json");
    const path outPath = Veng::TestSupport::TempDir() / "veng_cook_module_no_module.vengpack";

    const Cooker cooker;
    const VoidResult result = cooker.CookPack(packPath, outPath);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("unknown type") != string::npos);
}

TEST_CASE("cook module: a known game type with no cook module is a missing-importer error")
{
    Result<LoadedModuleTypes> moduleTypes = LoadModuleTypes(path{VENG_TEST_MODULE_PATH});
    REQUIRE(moduleTypes.has_value());

    // The name resolves — the runtime module registered the identity — but nothing cooks it.
    Cooker cooker;
    MergeAssetTypes(moduleTypes->AssetTypes, cooker.GetAssetTypes());
    CHECK(cooker.GetAssetTypes().FindByName(ProbeAssetTypeName) ==
          optional<AssetTypeId>(ProbeAssetType));

    const path packPath = WriteProbePack("veng_cook_module_no_importer_pack.json");
    const path outPath = Veng::TestSupport::TempDir() / "veng_cook_module_no_importer.vengpack";

    const VoidResult result = cooker.CookPack(packPath, outPath);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("no importer") != string::npos);
}

TEST_CASE("cook module: both halves together cook a game type and the archive verifies")
{
    // Both handles are declared before the Cooker so they are destroyed after it: the importer
    // moves into the cooker and its code lives in the cook-module image.
    Result<LoadedModuleTypes> moduleTypes = LoadModuleTypes(path{VENG_TEST_MODULE_PATH});
    REQUIRE(moduleTypes.has_value());
    Result<LoadedCookModule> cookModule = LoadCookModule(path{VENG_TEST_COOK_MODULE_PATH});
    REQUIRE(cookModule.has_value());

    Cooker cooker;
    MergeAssetTypes(moduleTypes->AssetTypes, cooker.GetAssetTypes());
    cookModule->Importers.MoveInto(cooker);

    const path packPath = WriteProbePack("veng_cook_module_pack.json");
    const path outPath = Veng::TestSupport::TempDir() / "veng_cook_module.vengpack";

    const VoidResult result = cooker.CookPack(packPath, outPath);
    REQUIRE_MESSAGE(result.has_value(), "cook failed: ", result ? string{} : result.error());

    // The archive round-trips: the TOC carries the minted game type id (never an engine one) and
    // every blob's content hash checks out.
    const VerifyReport report = VerifyArchive(outPath);
    CHECK(report.Ok());
    REQUIRE(report.Assets.size() == 1);
    CHECK(report.Assets.front().Type == ProbeAssetType);
}

namespace
{
    // Everything a host holds around a loaded game module, in the order a host must declare it:
    // the module handle first, so dlclose runs after the registries whose contents live in its
    // image. The launcher's own shape, small enough to build inside a test case.
    struct ModuleHost
    {
        // Optional only because LoadedModule has no default constructor; declared first so it
        // is destroyed last, after the registries whose contents live in its image.
        optional<LoadedModule> Module;
        ApplicationRegistry Apps;
        TypeRegistry Types;
        SystemRegistry Systems;
        AssetTypeRegistry AssetTypes;
        AssetLoaderRegistry AssetLoaders;
    };

    // Fills a host exactly as launcher_main does — builtins first, then the module's own
    // registrations. Unlike LoadModuleTypes this keeps the loader registry, which is what an
    // AssetManager needs to dispatch the module's asset type.
    void InitHost(ModuleHost& host)
    {
        Result<LoadedModule> loaded = ModuleLoader::Load(path{VENG_TEST_MODULE_PATH});
        REQUIRE_MESSAGE(loaded.has_value(),
                        "module load failed: ", loaded ? string{} : loaded.error());
        host.Module.emplace(std::move(*loaded));

        RegisterBuiltinTypes(host.Types);
        RegisterBuiltinSystems(host.Systems);
        RegisterBuiltinAssetTypes(host.AssetTypes);

        VengModuleHost moduleHost{.App = host.Apps,
                                  .Types = host.Types,
                                  .Systems = host.Systems,
                                  .AssetTypes = host.AssetTypes,
                                  .AssetLoaders = host.AssetLoaders,
                                  .Drivers = nullptr,
                                  .Editor = nullptr};
        host.Module->Register(moduleHost);
    }
}

TEST_CASE("cook module: a game-typed asset is referenced from a component, cooked, and loaded back")
{
    // The whole custom-type seam in one case: a component's AssetHandle field naming a
    // game-defined asset type is validated at cook time against the module's registered
    // handle-leaf mapping, then resolved at load time through the same mapping, dispatched to
    // the module's own loader, and rehydrated into the spawned component.
    ModuleHost host;
    InitHost(host);
    Result<LoadedCookModule> cookModule = LoadCookModule(path{VENG_TEST_COOK_MODULE_PATH});
    REQUIRE(cookModule.has_value());

    const path outPath = Veng::TestSupport::TempDir() / "veng_cook_module_prefab.vengpack";
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        MergeAssetTypes(host.AssetTypes, cooker.GetAssetTypes());
        cookModule->Importers.MoveInto(cooker);

        const VoidResult result =
            cooker.CookPack(path(VENG_COOKER_TEST_FIXTURE_DIR) / "probe_prefab_pack.json", outPath,
                            {}, &host.Types);
        REQUIRE_MESSAGE(result.has_value(), "cook failed: ", result ? string{} : result.error());
    }

    Renderer::Context context;
    TaskSystem tasks;
    AssetManager manager(
        context, tasks, host.Types,
        AssetManagerInfo{.AssetTypes = &host.AssetTypes, .Loaders = &host.AssetLoaders});
    REQUIRE(manager.Mount(outPath).has_value());

    // Loading the prefab pulls the game-typed asset in as an ordinary load-time dependency:
    // the leaf TypeId resolves through the registered mapping and dispatches to the module.
    const AssetResult<AssetHandle<Prefab>> prefab = manager.LoadSync<Prefab>(AssetId{0x2042});
    REQUIRE_MESSAGE(prefab.has_value(),
                    "prefab load failed: ", prefab ? string{} : prefab.error().Detail);
    CHECK(manager.Get<ProbeAsset>(AssetId{0x2041}).has_value());

    // Spawn rehydrates the field, so the component holds a resident handle to the blob the
    // cook module produced — the reference a game authors, end to end.
    const Veng::Unique<Scene> scene = Scene::Create(host.Types);
    const Prefab::SpawnResult spawned = (*prefab)->SpawnInto(*scene, manager);
    REQUIRE(spawned.Roots.size() == 1);

    const Probe* const probe = scene->TryGet<Probe>(spawned.Roots.front());
    REQUIRE(probe != nullptr);
    CHECK(probe->Value == doctest::Approx(2.5f));
    REQUIRE(probe->Asset.IsLoaded());
    CHECK(probe->Asset->Bytes == vector<u8>{0x78, 0x56, 0x34, 0x12});

    std::filesystem::remove(outPath);
}

TEST_CASE("generate-asset-type: --module mints against the module's registered asset types")
{
    Result<LoadedModuleTypes> moduleTypes = LoadModuleTypes(path{VENG_TEST_MODULE_PATH});
    REQUIRE(moduleTypes.has_value());
    REQUIRE(moduleTypes->AssetTypes.IsRegistered(ProbeAssetType));

    const AssetTypeId id = GenerateAssetTypeId(moduleTypes->AssetTypes);
    CHECK(id.IsValid());
    CHECK(id != ProbeAssetType);
    CHECK_FALSE(moduleTypes->AssetTypes.IsRegistered(id));
}
