#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

namespace VengEditor
{
    /// @brief Capability surface a panel may call on its host.
    ///
    /// EditorHost implements this interface; a panel holds a PanelHost& rather
    /// than the concrete host, so it depends only on the operations it needs.
    class PanelHost
    {
    public:
        virtual ~PanelHost() = default;

        /// @brief Opens the registered editor for an asset.
        ///
        /// The host resolves the type's factory, constructs the panel, and adopts
        /// it into the panel set at the next safe point in the frame, so calling
        /// this from inside OnUI is safe. A no-op for a type with no registered editor.
        /// @param type The asset type determining which editor factory to use.
        /// @param id   The asset to open in the editor.
        virtual void OpenAssetEditor(Veng::AssetType type, Veng::AssetId id) = 0;

        /// @brief Returns whether an asset type has a registered editor.
        ///
        /// A panel uses this to gate the double-click-to-open affordance — an asset of a
        /// type with no editor opens nothing.
        /// @param type The asset type to query.
        /// @return True when a factory is registered for the type.
        [[nodiscard]] virtual bool HasAssetEditor(Veng::AssetType type) const = 0;
    };
}
