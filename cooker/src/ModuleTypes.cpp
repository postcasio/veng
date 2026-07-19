#include <Veng/Cook/ModuleTypes.h>

#include <Veng/Asset/AssetLoaderRegistry.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/Module.h>
#include <Veng/Scene/BuiltinSystems.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/SystemRegistry.h>

#include <random>

namespace Veng::Cook
{
    Result<LoadedModuleTypes> LoadModuleTypes(const path& modulePath)
    {
        Result<LoadedModule> loaded = ModuleLoader::Load(modulePath);
        if (!loaded)
        {
            return std::unexpected(loaded.error());
        }

        LoadedModuleTypes result{
            .Module = std::move(*loaded), .Types = {}, .Systems = {}, .AssetTypes = {}};

        // Pre-register engine builtins, then run the module's VengModuleRegister. The module's
        // own registrations land in result.Types/Systems/AssetTypes (the level importer resolves a
        // level's system ids against the catalog — builtins included — so it validates the engine
        // systems a level names without the module re-declaring them); the Application factory it
        // also registers lands in a throwaway registry and is never invoked.
        RegisterBuiltinTypes(result.Types);
        RegisterBuiltinSystems(result.Systems);
        RegisterBuiltinAssetTypes(result.AssetTypes);

        // The cooker constructs no AssetManager, so a module's loader factories have nowhere to
        // go: they register into a throwaway and are discarded inert, exactly as the Application
        // factory is. The importers that actually run here come from the cook module instead.
        ApplicationRegistry throwawayApps;
        AssetLoaderRegistry throwawayLoaders;
        VengModuleHost host{.App = throwawayApps,
                            .Types = result.Types,
                            .Systems = result.Systems,
                            .AssetTypes = result.AssetTypes,
                            .AssetLoaders = throwawayLoaders,
                            .Drivers = nullptr,
                            .Editor = nullptr};
        result.Module.Register(host);

        return result;
    }

    void MergeAssetTypes(const AssetTypeRegistry& source, AssetTypeRegistry& destination)
    {
        for (const auto& [id, info] : source.All())
        {
            if (!destination.IsRegistered(id))
            {
                destination.Register(info);
            }
        }
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
