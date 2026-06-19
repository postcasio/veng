// Cooker-side module reflection test (plan 01's GPU-free registration contract,
// exercised from the cooker). Points LoadModuleTypes at the built
// libhello_triangle and asserts the registry holds the engine builtins AND the
// game's Spinner with its authored field/TypeId — with NO Vulkan device. Also
// covers the ABI-mismatch path (a wrong-version module is a located Result
// error) and that CookPack rejects a prefab entry with no --module.

#include <doctest/doctest.h>

#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>

#include <filesystem>
#include <fstream>

using namespace Veng;
using namespace Veng::Cook;

// Spinner's authored id and field, mirrored from the hello-triangle module.
// The cooker reflects them out of the loaded image with no compile-time
// knowledge of the type.
namespace
{
    constexpr TypeId SpinnerTypeId = 0xAEF00D5EFC2444DAULL;
}

TEST_CASE("module reflect: LoadModuleTypes reflects builtins + the game's Spinner, no device")
{
    const path modulePath = VENG_HELLO_TRIANGLE_MODULE_PATH;

    Result<LoadedModuleTypes> loaded = LoadModuleTypes(modulePath);
    REQUIRE_MESSAGE(loaded.has_value(),
        "LoadModuleTypes failed: ", loaded ? string{} : loaded.error());

    const TypeRegistry& types = loaded->Types;

    // Engine builtins, pre-registered GPU-free.
    CHECK(types.IsRegistered(TypeIdOf<Transform>()));
    CHECK(types.IsRegistered(TypeIdOf<Name>()));
    CHECK(types.IsRegistered(TypeIdOf<Parent>()));
    CHECK(types.IsRegistered(TypeIdOf<MeshRenderer>()));
    CHECK(types.IsRegistered(TypeIdOf<CameraComponent>()));

    // The Light builtin, with its authored TypeId and field set — reflected from
    // the loaded module with no device.
    REQUIRE(types.IsRegistered(TypeIdOf<Light>()));
    const TypeInfo& light = types.Info(TypeIdOf<Light>());
    CHECK(light.Id == 0xECF6442708DF7C00ULL);
    CHECK(light.Name == "::Veng::Light");
    REQUIRE(light.Fields.size() == 7);
    CHECK(light.Fields[0].Name == "Type");
    CHECK(light.Fields[1].Name == "Direction");
    CHECK(light.Fields[2].Name == "Color");
    CHECK(light.Fields[3].Name == "Intensity");
    CHECK(light.Fields[4].Name == "Range");
    CHECK(light.Fields[5].Name == "InnerCone");
    CHECK(light.Fields[6].Name == "OuterCone");

    // The game's component, registered by the module's VengModuleRegister.
    REQUIRE(types.IsRegistered(SpinnerTypeId));

    const TypeInfo& spinner = types.Info(SpinnerTypeId);
    CHECK(spinner.Name == "Spinner");
    CHECK(spinner.Id == SpinnerTypeId);
    REQUIRE(spinner.Fields.size() == 1);
    CHECK(spinner.Fields[0].Name == "SpeedRadiansPerSec");
}

TEST_CASE("module reflect: an ABI-mismatched module is a located Result error")
{
    Result<LoadedModuleTypes> loaded = LoadModuleTypes(path{VENG_BAD_VERSION_MODULE_PATH});
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("module reflect: a nonexistent module path is a located Result error")
{
    Result<LoadedModuleTypes> loaded = LoadModuleTypes(path{"this-module-does-not-exist.dylib"});
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("generate-type-id: mints a non-zero, distinct, collision-free id")
{
    TypeRegistry builtins;
    RegisterBuiltinTypes(builtins);

    const TypeId a = GenerateTypeId(builtins);
    const TypeId b = GenerateTypeId(builtins);

    CHECK(a != InvalidTypeId);
    CHECK(b != InvalidTypeId);
    CHECK(a != b);
    CHECK_FALSE(builtins.IsRegistered(a));
    CHECK_FALSE(builtins.IsRegistered(b));
}

TEST_CASE("generate-type-id: --module does not collide with a registered game id")
{
    Result<LoadedModuleTypes> loaded = LoadModuleTypes(path{VENG_HELLO_TRIANGLE_MODULE_PATH});
    REQUIRE(loaded.has_value());

    const TypeId id = GenerateTypeId(loaded->Types);
    CHECK(id != InvalidTypeId);
    CHECK_FALSE(loaded->Types.IsRegistered(id));
    CHECK(id != SpinnerTypeId);
}

TEST_CASE("module reflect: cooking a prefab entry with no --module is a located error")
{
    const path packPath = std::filesystem::temp_directory_path() / "veng_cooker_prefab_no_module_pack.json";
    {
        std::ofstream out(packPath, std::ios::binary);
        out << R"({ "version": 1, "assets": [ { "id": 4242, "type": "prefab", "source": "x.prefab.json" } ] })";
    }

    Cooker cooker;
    const path outPath = std::filesystem::temp_directory_path() / "veng_cooker_prefab_no_module.vengpack";

    // No TypeRegistry passed (the no --module case).
    const VoidResult result = cooker.CookPack(packPath, outPath);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("prefab cooking requires --module") != string::npos);
}
