#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a `*.vuss` USS-like stylesheet source into flattened, resolved rule tables.
    ///
    /// The source is a USS subset: type / class / id selectors plus a single pseudo-state
    /// (`:hover` / `:active` / `:focus` / `:disabled` / `:checked`), each paired with a declaration
    /// block. The importer tokenizes and parses the source offline (a cooker-only CSS tokenizer),
    /// resolves every declaration's value (colors sRGB → linear, lengths, enums), and emits a
    /// CookedStyleSheetHeader plus a rule table and a property table. The runtime never runs a
    /// selector engine — it matches an element's tags against these rules and cascades the
    /// survivors onto its style. A stylesheet is a standalone, reusable asset one document references.
    class StyleSheetImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetType::StyleSheet.
        [[nodiscard]] AssetType Type() const override { return AssetType::StyleSheet; }

        /// @brief Cooks the stylesheet described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
