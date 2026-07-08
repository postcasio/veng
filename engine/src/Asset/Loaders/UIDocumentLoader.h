#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Gui/UIDocument.h>

#include <span>

namespace Veng
{
    /// @brief AssetType::UIDocument loader.
    ///
    /// Decodes a CookedUIDocumentHeader + stylesheet id list + element tree + inline-property table
    /// + string region into a Gui::UIDocument recipe, resolving the referenced stylesheets and the
    /// inline styles' font dependencies as load-time dependencies (kept resident). The document is a
    /// CPU recipe with no GPU resource; Gui::Document::Instantiate materializes an independent live
    /// tree from it.
    class UIDocumentLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetType::UIDocument.
        [[nodiscard]] AssetType Type() const override { return AssetType::UIDocument; }

        /// @brief Decodes the cooked UI-document blob into a LoadJob producing a resident Gui::UIDocument.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };

    namespace Detail
    {
        /// @brief The decoded form of a cooked UI-document blob: its recipe tree and dependency ids.
        struct DecodedUIDocument
        {
            /// @brief The pre-order recipe element tree.
            vector<Gui::UIElementRecipe> Elements;
            /// @brief The referenced StyleSheet AssetIds, in reference order.
            vector<AssetId> StyleSheetIds;
            /// @brief The deduplicated font AssetIds the inline styles reference (load-time dependencies).
            vector<AssetId> FontIds;
            /// @brief The deduplicated texture AssetIds the Image elements source (load-time dependencies).
            vector<AssetId> TextureIds;
        };

        /// @brief Decodes a cooked UI-document blob into its recipe tree and dependency ids.
        ///
        /// The pure, device-free decode both the loader and its round-trip test share: it validates
        /// the header (version + sizes), reads the stylesheet id list, the element tree, the
        /// inline-property table, and the string region. A truncated or version-mismatched blob is a
        /// recoverable AssetError::Corrupt.
        /// @param id      The asset being decoded (for error reporting).
        /// @param cooked  The cooked blob bytes.
        /// @return The decoded recipe + dependency ids, or a structured load error.
        [[nodiscard]] AssetResult<DecodedUIDocument> DecodeUIDocument(AssetId id,
                                                                      std::span<const u8> cooked);
    }
}
