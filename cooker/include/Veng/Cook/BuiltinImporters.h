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

    /// @brief Registers the level importer.
    ///
    /// Separate from RegisterBuiltinImporters for the same reason as the prefab importer:
    /// it links libveng's reflection serializer (WriteFields) and validates against the
    /// module's reflected TypeRegistry + SystemRegistry, so it is absent from the veng-free
    /// bootstrap cooker. The full vengc registers it.
    /// @param cooker  The cooker to register into.
    void RegisterLevelImporter(Cooker& cooker);
}
