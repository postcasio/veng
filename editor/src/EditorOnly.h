#pragma once

#include <Veng/Reflection/Reflect.h>

namespace VengEditor
{
    /// @brief Marks an entity an editor adds to a document's scene for its own presentation.
    ///
    /// The prefab editor lights a document with what it lacks — the preview look's environment as
    /// its sky, and a preview light when it would otherwise be unlit — so its content is visible;
    /// that lighting belongs to the editor, not to the document. A marked
    /// entity is left out of everything that reads the scene as the document: the explorer does not
    /// list it, the toolbar does not count it, and PrefabSerialize::Save never writes it. It still
    /// renders, and it still carries into a play session's clone, which is why it is added at all.
    ///
    /// The marker carries no reflected field: its presence is the whole signal.
    struct EditorOnly
    {
    };
}

VE_TYPE(::VengEditor::EditorOnly, 0x72B68423D1F13E24ULL);
