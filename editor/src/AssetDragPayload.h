#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Veng.h>

namespace VengEditor
{
    /// @brief Drag-drop payload type tag for an asset dragged from the asset browser.
    ///
    /// Matched by `UI::SetDragDropPayload`/`AcceptDragDropPayload`; the payload bytes are
    /// an `AssetDragPayload`.
    inline constexpr Veng::string_view AssetPayload = "VENG_ASSET";

    /// @brief Bytes carried by an asset drag: the asset's id and type.
    ///
    /// A trivially-copyable POD copied verbatim into the ImGui payload. A drop target reads
    /// the type to decide whether the asset fits (an AssetHandle field matches its own type).
    struct AssetDragPayload
    {
        /// @brief The dragged asset's id.
        Veng::AssetId Id;
        /// @brief The dragged asset's type.
        Veng::AssetType Type{};
    };
}
