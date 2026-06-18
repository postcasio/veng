#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

namespace VengEditor
{
    // The capability surface a panel may call on its host. EditorHost implements
    // it; a panel holds a PanelHost& and never the concrete host, so it depends on
    // what it can ask for, not on the whole application. It grows by adding
    // virtuals as panels need more from the host.
    class PanelHost
    {
    public:
        virtual ~PanelHost() = default;

        // Open the registered editor for an asset: the host resolves the type's
        // factory, constructs the panel, and adopts it into its set. Adoption is
        // deferred to a safe point in the frame, so calling this from inside a
        // panel's OnImGui is safe. A no-op for a type with no registered editor.
        virtual void OpenAssetEditor(Veng::AssetType type, Veng::AssetId id) = 0;
    };
}
