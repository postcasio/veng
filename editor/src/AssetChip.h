#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Veng.h>

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Human-readable name of an asset type ("Texture", "Mesh", …).
    /// @param type  The asset type.
    /// @return A static, never-null display string.
    [[nodiscard]] const char* AssetTypeName(Veng::AssetType type);

    /// @brief Short three-letter badge glyph for an asset type ("TEX", "MSH", …).
    /// @param type  The asset type.
    /// @return A static, never-null glyph string.
    [[nodiscard]] const char* AssetTypeGlyph(Veng::AssetType type);

    /// @brief Per-type badge fill color, authored sRGB.
    /// @param type  The asset type.
    /// @return The badge color for the type.
    [[nodiscard]] Veng::vec4 AssetTypeColor(Veng::AssetType type);

    /// @brief Display name of an asset, resolved from the manifest source index.
    ///
    /// Returns the source filename with the cooked extension(s) stripped
    /// (textures/brick.tex.json -> "brick"); an id absent from the index falls back to its
    /// hex spelling.
    /// @param id       The asset id to name.
    /// @param sources  Manifest source index supplying source paths.
    /// @return The asset's display name.
    [[nodiscard]] Veng::string AssetDisplayName(Veng::AssetId id, const AssetSourceIndex& sources);

    /// @brief Look and behavior of one asset chip.
    ///
    /// A chip is a bordered box: the type icon badge on the left, the asset name / type /
    /// id stacked on the right. It is a passive display by default; @ref DragSource makes it
    /// emit an asset drag, and @ref DropTarget makes it accept a drop and act as a
    /// click-to-search asset selector.
    struct AssetChipInfo
    {
        /// @brief The asset shown; a null id renders as "(none)".
        Veng::AssetId Id;
        /// @brief The asset's type — drives the icon badge and (for a drop target) the picker filter.
        Veng::AssetType Type{};
        /// @brief Disambiguating id suffix, unique per chip site (keeps the ImGui id distinct).
        Veng::string_view IdScope;
        /// @brief Display-name override; resolved from the source index when empty.
        Veng::string_view Name;
        /// @brief Fixed chip width in pixels; `0` fills the available content width.
        Veng::f32 Width = 0.0f;
        /// @brief When true, the chip is a drag source emitting an `AssetDragPayload`.
        bool DragSource = false;
        /// @brief When true, the chip accepts an asset drop and click-opens a search/select popup.
        ///
        /// A matching dropped asset or a popup pick changes the selection; clearing to "(none)"
        /// is offered in the popup. Ignored unless the type can be enumerated from the index.
        bool DropTarget = false;
    };

    /// @brief Draws an asset chip and, for a drop target, reports a new selection.
    ///
    /// Renders the bordered icon-plus-text box described by @p info. A @ref AssetChipInfo::DragSource
    /// chip attaches an `AssetDragPayload` carrying the asset's id and type. A
    /// @ref AssetChipInfo::DropTarget chip accepts a same-type drop and, on click, opens a popup
    /// listing every asset of @ref AssetChipInfo::Type from @p sources with a search box and a
    /// "(none)" clear entry.
    /// @param info     The chip's content and behavior.
    /// @param sources  Manifest source index for name resolution and picker candidates.
    /// @return The newly chosen id (a null id when cleared) when a drop target's selection
    ///         changed this frame, else nullopt.
    Veng::optional<Veng::AssetId> DrawAssetChip(const AssetChipInfo& info,
                                                const AssetSourceIndex& sources);
}
