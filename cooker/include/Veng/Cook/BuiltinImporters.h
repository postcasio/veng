#pragma once

#include <Veng/Cook/Cooker.h>

namespace Veng::Cook
{
    /// @brief Registers the veng-free built-in importers (texture, mesh, shader, material, …).
    ///
    /// Called by both the full vengc and the veng-free bootstrap cooker.
    /// @param cooker  The cooker to register into.
    void RegisterBuiltinImporters(Cooker& cooker);

    /// @brief Registers the prefab importer.
    ///
    /// Separate from RegisterBuiltinImporters because it links libveng's reflection
    /// serializer (WriteFields) and is therefore absent from the veng-free bootstrap
    /// cooker. The full vengc calls both.
    /// @param cooker  The cooker to register into.
    void RegisterPrefabImporter(Cooker& cooker);
}
