#include <Veng/Cook/CookModule.h>

#include <Veng/Assert.h>
#include <Veng/Cook/Cooker.h>

#include <fmt/format.h>

namespace Veng::Cook
{
    void AssetImporterRegistry::MoveInto(Cooker& cooker)
    {
        for (Unique<AssetImporter>& importer : m_Importers)
        {
            cooker.Register(std::move(importer));
        }
        m_Importers.clear();
    }

    Result<LoadedCookModule> LoadCookModule(const path& modulePath)
    {
        Result<LoadedModule> loaded = ModuleLoader::Load(modulePath, "VengCookModuleAbiVersion",
                                                         VENG_COOK_MODULE_ABI_VERSION);
        if (!loaded)
        {
            return std::unexpected(loaded.error());
        }

        LoadedCookModule result{.Module = std::move(*loaded), .Importers = {}};

        using EntryFn = void (*)(VengCookModuleHost*);
        auto entry = reinterpret_cast<EntryFn>(result.Module.Resolve("VengCookModuleRegister"));
        if (entry == nullptr)
        {
            return std::unexpected(fmt::format("cook module '{}' is version-matched but exports no "
                                               "VengCookModuleRegister entry",
                                               modulePath.string()));
        }

        VengCookModuleHost host{.Importers = result.Importers};
        entry(&host);

        return result;
    }

    path SiblingCookModulePath(const path& runtimeModulePath)
    {
        path sibling = runtimeModulePath;
        sibling.replace_filename(runtimeModulePath.stem().string() + "_cook" +
                                 runtimeModulePath.extension().string());
        return sibling;
    }
}
