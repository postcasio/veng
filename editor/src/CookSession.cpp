#include "CookSession.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/CookModule.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>

#include <filesystem>

namespace VengEditor
{
    using namespace Veng;

    Task<vector<u8>> CookSession::Cook(const CookRequest& request, TaskSystem& tasks)
    {
        // Capture by value: the worker outlives this call and holds no shared state.
        return tasks.Submit(
            [request]() -> Result<vector<u8>>
            {
                // A level cook validates its system ids and config fields against the
                // module's reflected catalogs; reflect the module on the worker when one
                // is named. Both module handles are declared before the Cooker so they are
                // destroyed after it — the cook module's importers move into it, and their
                // code lives in that image.
                optional<Cook::LoadedModuleTypes> moduleTypes;
                optional<Cook::LoadedCookModule> cookModule;
                const TypeRegistry* types = nullptr;
                const SystemRegistry* systems = nullptr;
                if (!request.ModulePath.empty())
                {
                    Result<Cook::LoadedModuleTypes> loaded =
                        Cook::LoadModuleTypes(request.ModulePath);
                    if (!loaded)
                    {
                        return std::unexpected(loaded.error());
                    }
                    moduleTypes = std::move(*loaded);
                    types = &moduleTypes->Types;
                    systems = &moduleTypes->Systems;

                    // A game type recooks in-editor only if its importer is present, so load the
                    // cook module sitting beside the runtime module. Its absence simply means the
                    // game defines no importers; a present-but-unloadable one is a cook error.
                    const path cookModulePath = Cook::SiblingCookModulePath(request.ModulePath);
                    if (std::filesystem::exists(cookModulePath))
                    {
                        Result<Cook::LoadedCookModule> loadedCook =
                            Cook::LoadCookModule(cookModulePath);
                        if (!loadedCook)
                        {
                            return std::unexpected(loadedCook.error());
                        }
                        cookModule = std::move(*loadedCook);
                    }
                }

                Cook::Cooker cooker;
                Cook::RegisterBuiltinImporters(cooker);
                if (moduleTypes)
                {
                    Cook::MergeAssetTypes(moduleTypes->AssetTypes, cooker.GetAssetTypes());
                }
                if (cookModule)
                {
                    cookModule->Importers.MoveInto(cooker);
                }

                // The project's packs share one AssetId namespace; cooking one source resolves
                // ids against every pack the host passed.
                const vector<path>& referencePacks = request.ReferenceManifests;

                // The active build configuration, when set, resolves a texture's role to a
                // concrete format exactly as the file-based build does; an unset config cooks
                // with the zero-config defaults.
                const BuildConfiguration* config =
                    request.ActiveConfig ? &*request.ActiveConfig : nullptr;

                return cooker.CookSource(request.SourcePath, request.TargetId, request.Type,
                                         referencePacks, types, systems, config,
                                         request.ShaderIncludeDir);
            });
    }
}
