#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Gui/StyleSheet.h>

#include <span>

namespace Veng
{
    /// @brief AssetTypes::StyleSheet loader.
    ///
    /// Decodes a CookedStyleSheetHeader + rule table + property table into a Gui::StyleSheet, and
    /// resolves each font declaration's AssetId as a load-time dependency (kept resident). The sheet
    /// is CPU data with no GPU resource, so the load finalizes as soon as its font dependencies are
    /// resident.
    class StyleSheetLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::StyleSheet.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::StyleSheet; }

        /// @brief Decodes the cooked stylesheet blob into a LoadJob producing a resident Gui::StyleSheet.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };

    namespace Detail
    {
        /// @brief The decoded form of a cooked stylesheet blob: rules, clips, and asset dependency ids.
        struct DecodedStyleSheet
        {
            /// @brief The flattened, resolved rules, in source order.
            vector<Gui::StyleRule> Rules;
            /// @brief The @keyframes clips, in the index order `animation` declarations reference.
            vector<Gui::StyleAnimationClip> Animations;
            /// @brief The gradients, in the index order `background-gradient` declarations reference.
            vector<Gui::StyleGradient> Gradients;
            /// @brief The sheet's own queryable variables (colors and scalars), in source order.
            vector<Gui::StyleVariable> Variables;
            /// @brief The deduplicated font AssetIds every declaration references (load-time dependencies).
            vector<AssetId> FontIds;
            /// @brief The deduplicated texture AssetIds every `background-image` names (load-time dependencies).
            vector<AssetId> TextureIds;
        };

        /// @brief Decodes a cooked stylesheet blob into its rules and asset dependency ids.
        ///
        /// The pure, device-free decode both the loader and its round-trip test share: it validates
        /// the header (version + sizes) and reads the rule/property tables. A truncated or
        /// version-mismatched blob is a recoverable AssetError::Corrupt.
        /// @param id      The asset being decoded (for error reporting).
        /// @param cooked  The cooked blob bytes.
        /// @return The decoded rules + dependency ids, or a structured load error.
        [[nodiscard]] AssetResult<DecodedStyleSheet> DecodeStyleSheet(AssetId id,
                                                                      std::span<const u8> cooked);
    }
}
