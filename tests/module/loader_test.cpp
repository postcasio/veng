// Boundary regression test for the module ABI + ModuleLoader.
//
// Loads a real shared library through the real platform loader and proves the
// whole path: load -> ABI-version check ->
// VengModuleRegister -> both registrations (the Application factory and the
// game's component type) land in the host. Also proves the builtins register
// GPU-free + idempotently, that the host registry reflects the module's
// component with the expected descriptors with NO Context constructed, that a
// wrong-version module is a recoverable Result error whose entry is never
// called, and that a nonexistent path is a recoverable Result error.
//
// It then runs the *runtime* half of the asset seam the registrations exist for: a real
// AssetManager built over the module's registries mounts an archive carrying a
// module-typed blob and loads it, so dispatch on a non-builtin AssetTypeId, the
// AssetTypeTrait binding, and the module loader's own decode are all executed rather than
// merely registered. Driver-free throughout: Renderer::Context is default-constructed and
// never initialized, and no loader on this path touches a device.

#include <cstdio>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetLoaderRegistry.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Task/TaskSystem.h>

#include <algorithm>
#include <filesystem>

#include "probe_component.h"
#include "support/TempPath.h"

using namespace Veng;

namespace
{
    int g_Failures = 0;

    void Check(bool condition, const char* what)
    {
        if (condition)
        {
            std::printf("[ok]   %s\n", what);
        }
        else
        {
            std::printf("[FAIL] %s\n", what);
            ++g_Failures;
        }
    }
}

int main()
{
    // 0. Builtins register GPU-free (no Context constructed here) and idempotently.
    {
        TypeRegistry builtins;
        Check(builtins.Count() == 0, "fresh registry is empty");
        RegisterBuiltinTypes(builtins);
        const usize afterFirst = builtins.Count();
        Check(afterFirst > 0, "RegisterBuiltinTypes registers builtins");
        Check(builtins.IsRegistered(TypeIdOf<Transform>()), "Transform builtin present");
        Check(builtins.IsRegistered(TypeIdOf<MeshRenderer>()), "MeshRenderer builtin present");

        // Called twice: each type is a no-op, no duplicate/collision assert.
        RegisterBuiltinTypes(builtins);
        Check(builtins.Count() == afterFirst, "RegisterBuiltinTypes is idempotent");
    }

    // 1. Load the good module, run the handshake, register into the host. The
    // host registry is real and has the builtins pre-registered — the launcher
    // path in miniature, with NO Context constructed (the contract plan 02's
    // cooker depends on). The LoadedModule is declared first so it is destroyed
    // (dlclose) LAST — after the registries. The registered factory is a closure
    // and the registered type descriptors are code/data whose definitions live in
    // the module image, so neither registry may outlive the module, or a
    // destructor would call into an unloaded library.
    {
        Result<LoadedModule> loaded = ModuleLoader::Load(VENG_TEST_MODULE_PATH);
        Check(loaded.has_value(), "good module loads (version handshake passes)");

        if (loaded)
        {
            ApplicationRegistry app;
            TypeRegistry types;
            SystemRegistry systems;
            AssetTypeRegistry assetTypes;
            AssetLoaderRegistry assetLoaders;
            RegisterBuiltinTypes(types);
            RegisterBuiltinAssetTypes(assetTypes);
            VengModuleHost host{.App = app,
                                .Types = types,
                                .Systems = systems,
                                .AssetTypes = assetTypes,
                                .AssetLoaders = assetLoaders,
                                .Drivers = nullptr,
                                .Editor = nullptr};

            Check(!app.HasApplication(), "no Application before Register");
            Check(!types.IsRegistered(TypeIdOf<Probe>()), "game component absent before Register");
            Check(assetTypes.IsRegistered(AssetTypes::Texture),
                  "builtin asset type present before Register");
            Check(!assetTypes.IsRegistered(ProbeAssetType),
                  "game asset type absent before Register");
            Check(!assetLoaders.IsRegistered(ProbeAssetType),
                  "game loader factory absent before Register");

            loaded->Register(host);

            Check(app.HasApplication(), "Application factory registered after Register");
            Check(types.IsRegistered(TypeIdOf<Probe>()),
                  "game component registered after Register");

            // The asset-type seam: identity + display metadata and an inert loader factory both
            // land with no Context, AssetManager, or device anywhere in the process.
            Check(assetTypes.IsRegistered(ProbeAssetType),
                  "game asset type registered after Register");
            Check(assetTypes.GetName(ProbeAssetType) == ProbeAssetTypeName,
                  "game asset type resolves to its registered manifest name");
            Check(assetTypes.FindByName(ProbeAssetTypeName) ==
                      optional<AssetTypeId>(ProbeAssetType),
                  "game asset type resolves by manifest name");
            Check(assetTypes.GetDisplayName(ProbeAssetType) == "Probe Asset",
                  "game asset type carries its editor display name");
            Check(assetLoaders.IsRegistered(ProbeAssetType),
                  "game loader factory registered after Register");

            // The factory produces a loader claiming exactly the type it was registered under —
            // the invariant the AssetManager asserts when it instantiates the registry.
            const auto factory = assetLoaders.All().find(ProbeAssetType);
            Check(factory != assetLoaders.All().end() && factory->second() != nullptr &&
                      factory->second()->Type() == ProbeAssetType,
                  "game loader factory produces a loader for its own asset type");

            // The registered handle-leaf mapping: the reverse index resolves the component's
            // AssetHandle field back to the module's own asset type. Without it a prefab
            // carrying that component is a load error and a cook error, which is exactly the
            // defect this fixture exists to catch.
            Check(assetTypes.FindByHandleField(TypeIdOf<AssetHandle<ProbeAsset>>()) ==
                      optional<AssetTypeId>(ProbeAssetType),
                  "game asset type claims its AssetHandle leaf type");

            if (types.IsRegistered(TypeIdOf<Probe>()))
            {
                const TypeInfo& info = types.Info(TypeIdOf<Probe>());
                Check(info.Name == "Probe", "reflected component has expected name");
                Check(info.Fields.size() == 2, "reflected component has two fields");
                Check(!info.Fields.empty() && info.Fields[0].Name == "Value",
                      "reflected component's first field is 'Value'");
                Check(info.Fields.size() > 1 && info.Fields[1].Name == "Asset" &&
                          info.Fields[1].Class == FieldClass::AssetHandle &&
                          info.Fields[1].Type == TypeIdOf<AssetHandle<ProbeAsset>>(),
                      "reflected component's second field is an AssetHandle of the game type");
            }
            // The module asserts host.Editor == nullptr internally; reaching here
            // without aborting confirms it observed the null Editor slot.

            // The runtime half: a real AssetManager over the module's registries dispatches a
            // non-builtin AssetTypeId to the module's loader and decodes its blob. Everything
            // above proves registration landed; only this proves the seam runs.
            {
                const vector<u8> blob{0x11, 0x22, 0x33, 0x44};

                ArchiveWriter writer;
                writer.Add(AssetId{0x2041}, ProbeAssetType, blob);
                writer.Add(AssetId{0x2042}, ProbeAssetType, vector<u8>{});
                const path archivePath =
                    Veng::TestSupport::TempDir() / "veng_loader_probe_asset.vengpack";
                Check(writer.Write(archivePath).has_value(), "probe archive writes");

                Renderer::Context context;
                TaskSystem tasks;
                TypeRegistry assetTypesForManager;
                AssetManager manager(
                    context, tasks, assetTypesForManager,
                    AssetManagerInfo{.AssetTypes = &assetTypes, .Loaders = &assetLoaders});

                Check(manager.Mount(archivePath).has_value(), "probe archive mounts");

                const AssetResult<AssetHandle<ProbeAsset>> loaded =
                    manager.LoadSync<ProbeAsset>(AssetId{0x2041});
                Check(loaded.has_value(), "LoadSync of a module-defined type resolves");
                Check(loaded.has_value() && loaded->IsLoaded() &&
                          std::ranges::equal((*loaded)->Bytes, blob),
                      "the module's loader decoded the cooked blob");

                // The manager knows the type by the name the module registered, not by a bare
                // hex id — the diagnostic a developer meets when a load goes wrong.
                Check(manager.GetAssetTypes().GetName(ProbeAssetType) == ProbeAssetTypeName,
                      "the manager's registry carries the module's type name");

                // A decode failure in a module loader is recoverable, not fatal.
                const AssetResult<AssetHandle<ProbeAsset>> corrupt =
                    manager.LoadSync<ProbeAsset>(AssetId{0x2042});
                Check(!corrupt.has_value() && corrupt.error().Kind == AssetError::Corrupt,
                      "a module loader's decode error is a recoverable AssetError");

                std::filesystem::remove(archivePath);
            }
        }
    }

    // 2. The wrong-version module is rejected at load; its entry never runs.
    {
        Result<LoadedModule> loaded = ModuleLoader::Load(VENG_BAD_VERSION_MODULE_PATH);
        Check(!loaded.has_value(), "wrong-version module is a Result error");
        if (!loaded)
        {
            std::printf("       error: %s\n", loaded.error().c_str());
        }
    }

    // 3. A nonexistent path is a recoverable Result error.
    {
        const Result<LoadedModule> loaded =
            ModuleLoader::Load(path{"this-module-does-not-exist.dylib"});
        Check(!loaded.has_value(), "nonexistent path is a Result error");
    }

    if (g_Failures == 0)
    {
        std::printf("loader_test: all checks passed\n");
        return 0;
    }

    std::printf("loader_test: %d check(s) failed\n", g_Failures);
    return 1;
}
