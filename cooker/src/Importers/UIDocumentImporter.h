#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a `*.vui.xml` markup source into a binary UI-document element tree.
    ///
    /// The source is an XML dialect of element tags (`Panel` / `Text` / `Button` / …, the closed
    /// Gui::ElementKind set) carrying identity (`id`, `class`), inline style (`style="…"`),
    /// `{binding}` attributes, and `on*` handler attributes. The importer parses the markup offline
    /// (a cooker-only XML parser), interns every string, and emits a CookedUIDocumentHeader plus the
    /// referenced-stylesheet ids, the pre-order element tree, the class/binding/handler side tables,
    /// the inline-style property table, and the string region. Binding expressions and handler names
    /// are stored unresolved; the runtime resolves them against a bound context. An unknown tag or a
    /// malformed attribute is a located cook error.
    class UIDocumentImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::UIDocument.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::UIDocument; }

        /// @brief Cooks the UI document described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
