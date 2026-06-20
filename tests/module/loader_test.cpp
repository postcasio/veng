// Boundary regression test for the module ABI + ModuleLoader.
//
// Loads a real shared library through the real platform loader and proves the
// whole path: load -> ABI-version check (now against host ABI 2) ->
// VengModuleRegister -> both registrations (the Application factory and the
// game's component type) land in the host. Also proves the builtins register
// GPU-free + idempotently, that the host registry reflects the module's
// component with the expected descriptors with NO Context constructed, that a
// wrong-version module is a recoverable Result error whose entry is never
// called, and that a nonexistent path is a recoverable Result error. Driver-free
// — no Context/Window is constructed (the registered factory is stored, never
// invoked).

#include <cstdio>

#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>

#include "probe_component.h"

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
        Check(loaded.has_value(), "good module loads (version handshake passes against ABI 2)");

        if (loaded)
        {
            ApplicationRegistry app;
            TypeRegistry types;
            RegisterBuiltinTypes(types);
            VengModuleHost host{.App = app, .Types = types, .Editor = nullptr};

            Check(!app.HasApplication(), "no Application before Register");
            Check(!types.IsRegistered(TypeIdOf<Probe>()), "game component absent before Register");

            loaded->Register(host);

            Check(app.HasApplication(), "Application factory registered after Register");
            Check(types.IsRegistered(TypeIdOf<Probe>()),
                  "game component registered after Register");

            if (types.IsRegistered(TypeIdOf<Probe>()))
            {
                const TypeInfo& info = types.Info(TypeIdOf<Probe>());
                Check(info.Name == "Probe", "reflected component has expected name");
                Check(info.Fields.size() == 1, "reflected component has one field");
                Check(!info.Fields.empty() && info.Fields[0].Name == "Value",
                      "reflected component's field is 'Value'");
            }
            // The module asserts host.Editor == nullptr internally; reaching here
            // without aborting confirms it observed the null Editor slot.
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
