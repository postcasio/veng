// The game-defined asset-type seam, exercised from the cooker.
//
// Proves the two halves meet: veng_test_module registers the type's identity and name through the
// runtime module ABI, veng_test_cook_module registers its importer through the cook module ABI,
// and only with both does a pack entry naming that type cook. Also covers the cook ABI handshake
// (a stale cook module is a located error whose entry never runs) and the sibling-path convention.

#include <doctest/doctest.h>

#include <Veng/Cook/CookModule.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Cook/Verify.h>

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
