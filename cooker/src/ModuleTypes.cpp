#include <Veng/Cook/ModuleTypes.h>

#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Scene/BuiltinTypes.h>

#include <random>

namespace Veng::Cook
{
    Result<LoadedModuleTypes> LoadModuleTypes(const path& modulePath)
    {
        Result<LoadedModule> loaded = ModuleLoader::Load(modulePath);
        if (!loaded)
            return std::unexpected(loaded.error());

        LoadedModuleTypes result{.Module = std::move(*loaded), .Types = {}};

        // Pre-register the engine builtins (GPU-free), then run the module's
        // VengModuleRegister so the game's component types land alongside them.
        // The Application factory it also registers is captured into this
        // throwaway registry and never invoked — the cooker constructs no app.
        RegisterBuiltinTypes(result.Types);

        ApplicationRegistry throwaway;
        VengModuleHost host{.App = throwaway, .Types = result.Types, .Editor = nullptr};
        result.Module.Register(host);

        return result;
    }

    TypeId GenerateTypeId(const TypeRegistry& existing)
    {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        std::uniform_int_distribution<u64> dist(1, UINT64_MAX);

        TypeId id = InvalidTypeId;
        do
        {
            id = dist(rng);
        } while (id == InvalidTypeId || existing.IsRegistered(id));

        return id;
    }
}
