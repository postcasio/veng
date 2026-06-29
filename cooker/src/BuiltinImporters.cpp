#include <Veng/Cook/BuiltinImporters.h>

#include "Importers/GraphShaderSource.h"

namespace Veng::Cook
{
    void RegisterBuiltinImporters(Cooker& cooker)
    {
        // Install the graph emit walk (veng::graph, linked here) as the resolver the
        // veng-free shader/material importers call through. The core-pack bootstrap
        // registers RegisterCoreImporters alone, leaving this null — it cooks no graph
        // sources.
        SetGraphShaderResolver(&ResolveGraphShaderSource);

        RegisterCoreImporters(cooker);
        RegisterPrefabImporter(cooker);
        RegisterLevelImporter(cooker);
    }
}
