// Boundary regression test for the module ABI + ModuleLoader.
//
// Loads a real shared library through the real platform loader and proves the
// whole path: load -> ABI-version check -> VengModuleRegister -> registration
// lands. Also proves a wrong-version module is a recoverable Result error whose
// entry is never called, and that a nonexistent path is a recoverable Result
// error. Driver-free — no Context/Window is constructed (the registered factory
// is stored, never invoked).

#include <cstdio>

#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Module/ModuleLoader.h>

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
    // 1. Load the good module, run the handshake, register into the host.
    // The LoadedModule is declared first so it is destroyed (dlclose) LAST —
    // after the registry. The registered factory is a closure whose code lives
    // in the module image, so the registry holding it must not outlive the
    // module, or its destructor would call into an unloaded library.
    {
        Result<LoadedModule> loaded = ModuleLoader::Load(VENG_TEST_MODULE_PATH);
        Check(loaded.has_value(), "good module loads (version handshake passes)");

        if (loaded)
        {
            ApplicationRegistry app;
            VengModuleHost host{.App = app, .Editor = nullptr};

            Check(!app.HasApplication(), "no Application before Register");
            loaded->Register(host);
            Check(app.HasApplication(), "Application factory registered after Register");
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
        Result<LoadedModule> loaded =
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
