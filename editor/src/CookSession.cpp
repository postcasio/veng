#include "CookSession.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>

namespace VengEditor
{
    using namespace Veng;

    Task<vector<u8>> CookSession::Cook(const CookRequest& request, TaskSystem& tasks)
    {
        // Capture by value: the worker outlives this call and holds no shared state.
        return tasks.Submit(
            [request]() -> Result<vector<u8>>
            {
                Cook::Cooker cooker;
                Cook::RegisterBuiltinImporters(cooker);

                // The project's packs share one AssetId namespace; cooking one source resolves
                // ids against every pack the host passed.
                const vector<path>& referencePacks = request.ReferenceManifests;

                // A level cook validates its system ids and config fields against the
                // module's reflected catalogs; reflect the module on the worker when one
                // is named. The handle must outlive CookSource (the registries point into
                // the loaded image).
                optional<Cook::LoadedModuleTypes> moduleTypes;
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
                }

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
