#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;
}

namespace VengEditor
{
    /// @brief Shared editing state of one open prefab document.
    ///
    /// Owned by the PrefabEditorPanel and referenced by its child panels: the explorer
    /// mutates Selection, the inspector and viewport read it, all over the one Scene the
    /// document spawned. The references the children hold stay valid for the document's
    /// lifetime (Scene is a stable Unique the document owns).
    struct PrefabEditContext
    {
        /// @brief The scene the document spawned the prefab into.
        Veng::Scene* Scene = nullptr;
        /// @brief The selected entity, or nullopt when nothing is selected.
        Veng::optional<Veng::Entity> Selection;
    };
}
