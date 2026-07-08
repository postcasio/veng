#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/StyleSheet.h>

namespace Veng::Gui
{
    /// @brief One `{binding}` on a recipe element: a bound attribute name and its expression.
    ///
    /// Property is the bound attribute ("value"); Expression is the unresolved `{obj.field}`
    /// expression with braces stripped ("player.health"). Stored unresolved; the runtime resolves
    /// it against a bound context.
    struct UIBindingRecipe
    {
        /// @brief The bound attribute name ("value").
        string Property;
        /// @brief The unresolved binding expression, braces stripped ("player.health").
        string Expression;
    };

    /// @brief One named event handler on a recipe element: an event name and the handler it names.
    ///
    /// Event is the event attribute ("onClick"); Handler is the C++ handler name ("OpenMenu").
    /// Stored unresolved; the runtime resolves the handler against a bound context.
    struct UIHandlerRecipe
    {
        /// @brief The event attribute name ("onClick").
        string Event;
        /// @brief The unresolved handler name ("OpenMenu").
        string Handler;
    };

    /// @brief One element of a cooked UI document's recipe tree, in pre-order.
    ///
    /// A recipe element carries the authored identity (kind, id, classes, text), its unresolved
    /// bindings and handlers, and its inline-style declarations. ChildCount is the number of the
    /// immediately-following recipe elements (recursively) that are this element's direct children,
    /// so the pre-order array reconstructs the hierarchy in one pass. Instantiate materializes one
    /// live Element per recipe element.
    struct UIElementRecipe
    {
        /// @brief The element's kind.
        ElementKind Kind = ElementKind::Panel;
        /// @brief The element's id, or empty when untagged.
        string Id;
        /// @brief The element's class tags, in source order.
        vector<string> Classes;
        /// @brief The Text element's content, or empty.
        string Text;
        /// @brief The element's `{binding}` expressions.
        vector<UIBindingRecipe> Bindings;
        /// @brief The element's named event handlers.
        vector<UIHandlerRecipe> Handlers;
        /// @brief The element's inline-style declarations, in source order.
        vector<StyleDeclaration> InlineStyle;
        /// @brief Number of direct children of this element (they follow in pre-order).
        u32 ChildCount = 0;
        /// @brief An Image element's source texture AssetId; invalid for a non-image or un-sourced element.
        ///
        /// Resolved at Instantiate to a resident AssetHandle<Texture> on the live element and kept
        /// resident as a document texture dependency, the same shape a font declaration's AssetId takes.
        AssetId Src;
        /// @brief An Image element's tint, linear straight-alpha RGBA; opaque white by default (folds the style opacity at paint).
        vec4 Tint{1.0f};
        /// @brief An Image element's UV sub-rect (an atlas region); the whole texture by default.
        Rect Uv{.Min = vec2(0.0f), .Size = vec2(1.0f)};
    };

    /// @brief A cached, immutable cooked-UI-document asset: a recipe for a live element tree.
    ///
    /// A UI document holds the element tree the cooker parsed from `*.vui.xml` markup — pre-order,
    /// each element carrying its identity, inline style, bindings, and handlers — plus the
    /// stylesheets it references and its font/texture dependencies (kept resident). It is a recipe,
    /// not a live tree: Gui::Document::Instantiate materializes an independent Document from it, so
    /// instantiating twice yields two independent trees over one blob (the Prefab model).
    ///
    /// UIDocument is CPU data with no GPU resource. Load it through AssetManager::Load like any
    /// other asset, then Gui::Document::Instantiate it.
    class UIDocument
    {
    public:
        /// @brief Creates a UIDocument from its recipe tree, referenced stylesheets, and dependencies.
        /// @param elements      The pre-order recipe element tree.
        /// @param styleSheets   The referenced StyleSheet handles, in reference order.
        /// @param dependencies  The resolved font/texture dependency cache entries, kept resident.
        /// @return A shared UIDocument.
        static Ref<UIDocument> Create(vector<UIElementRecipe> elements,
                                      vector<AssetHandle<StyleSheet>> styleSheets,
                                      vector<Ref<Detail::AssetCacheEntry>> dependencies);

        /// @brief Returns the pre-order recipe element tree.
        [[nodiscard]] const vector<UIElementRecipe>& GetElements() const { return m_Elements; }

        /// @brief Returns the referenced StyleSheet handles, in reference order.
        [[nodiscard]] const vector<AssetHandle<StyleSheet>>& GetStyleSheets() const
        {
            return m_StyleSheets;
        }

    private:
        UIDocument(vector<UIElementRecipe> elements, vector<AssetHandle<StyleSheet>> styleSheets,
                   vector<Ref<Detail::AssetCacheEntry>> dependencies);

        vector<UIElementRecipe> m_Elements;
        vector<AssetHandle<StyleSheet>> m_StyleSheets;
        /// @brief Resolved font/texture dependency entries, kept resident for the document's lifetime.
        vector<Ref<Detail::AssetCacheEntry>> m_Dependencies;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping Gui::UIDocument to AssetType::UIDocument.
    template <>
    struct AssetTypeTrait<Gui::UIDocument>
    {
        /// @brief The asset type tag for UIDocument.
        static constexpr AssetType Type = AssetType::UIDocument;
    };
}
