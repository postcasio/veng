#pragma once

#include <Veng/Cook/Cooker.h>

// Registers the cooker's built-in importers.

namespace Veng::Cook
{
    // The veng-free importers (texture/mesh/shader/material/...). Registered by
    // both the full vengc and the veng-free bootstrap cooker.
    void RegisterBuiltinImporters(Cooker& cooker);

    // The prefab importer. Separate because it reuses libveng's reflection
    // serializer (WriteFields), so it lives in the veng-linked part of the cooker
    // and is not pulled into the veng-free bootstrap cooker (which cooks no
    // prefabs). The full vengc registers it alongside RegisterBuiltinImporters.
    void RegisterPrefabImporter(Cooker& cooker);
}
