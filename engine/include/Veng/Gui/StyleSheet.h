#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/StyleProperty.h>

namespace Veng::Gui
{
    /// @brief One resolved style declaration: a property and the value to write onto Style.
    ///
    /// The value rides a uniform payload the style application (plan 04) interprets by Property:
    /// Unit carries a Length's LengthKind ordinal or an enum property's enumerator ordinal; Values
    /// holds the numeric payload (a Length value in x, a vec4/CornerRadii/Insets in xyzw, a scalar
    /// in x); Font carries a font property's AssetId (else invalid). Colors are already linear
    /// straight-alpha (the cooker resolved authored sRGB), matching the draw-list contract.
    struct StyleDeclaration
    {
        /// @brief Which Style field this declaration sets.
        StyleProperty Property = StyleProperty::Background;
        /// @brief A Length's LengthKind ordinal or an enum property's enumerator ordinal; 0 otherwise.
        u32 Unit = 0;
        /// @brief Numeric payload: a Length value (x), a vec4/CornerRadii/Insets (xyzw), or a scalar (x).
        vec4 Values{0.0f};
        /// @brief A font property's AssetId (resolved through the sheet's dependencies); invalid otherwise.
        AssetId Font;
    };

    /// @brief One resolved USS rule: a selector, a pseudo-state, and its declarations.
    ///
    /// The selector matches an element by its element type (Type), one class tag (Class), and/or
    /// one id (Id); an empty field is a wildcard on that axis. State is the interaction state the
    /// rule applies in (None for the base rule; a single pseudo-state bit like Hovered). The style
    /// application (plan 04) matches an element's tags against the rules, cascades the base
    /// survivors onto its resolved Style, and keeps the state-scoped survivors as variants.
    struct StyleRule
    {
        /// @brief The element type the selector requires ("Button"), or empty for any type.
        string Type;
        /// @brief The class tag the selector requires ("primary"), or empty for no class constraint.
        string Class;
        /// @brief The id the selector requires ("start-button"), or empty for no id constraint.
        string Id;
        /// @brief The interaction state the rule applies in (None for the base rule, one bit for a pseudo-state).
        ElementState State = ElementState::None;
        /// @brief The declarations this rule sets, in source order.
        vector<StyleDeclaration> Declarations;
    };

    /// @brief Resolves a font AssetId to a resident handle, for applying a font declaration.
    ///
    /// The style application resolves a StyleDeclaration whose Property is TextFont through this,
    /// turning the declaration's AssetId into the live AssetHandle<Font> written onto Style. An
    /// empty function (or one returning an invalid handle) leaves the style's font unchanged.
    using FontResolver = function<AssetHandle<Font>(AssetId)>;

    /// @brief Applies one resolved declaration onto a Style, overwriting the property it sets.
    ///
    /// Writes the declaration's value onto the matching Style field per its Property, interpreting
    /// the uniform payload (Unit as a LengthKind/enumerator ordinal, Values as the numeric payload).
    /// A TextFont declaration resolves its AssetId through `fonts` when set; every other property is
    /// resolved data written directly. This is the single write path both inline-style
    /// materialization and stylesheet cascade share, so an inline override and a rule set a property
    /// identically.
    /// @param style        The style to write onto.
    /// @param declaration  The declaration to apply.
    /// @param fonts        Resolver for a TextFont declaration's AssetId; may be empty.
    void ApplyDeclaration(Style& style, const StyleDeclaration& declaration,
                          const FontResolver& fonts);

    /// @brief A cached, reusable stylesheet asset: a flattened, resolved set of USS-like rules.
    ///
    /// A stylesheet holds the rules the cooker flattened from a `*.vuss` source — each a selector
    /// plus its declarations, grouped by pseudo-state — as a standalone, reusable asset one
    /// document references. The runtime never runs a selector engine: the style application (plan
    /// 04) matches an element's tags against these rules and cascades the survivors onto the
    /// element's resolved Style. Font declarations resolve as ordinary load-time dependencies kept
    /// resident for the sheet's lifetime.
    ///
    /// StyleSheet is CPU data with no GPU resource. Load it through AssetManager::Load like any
    /// other asset.
    class StyleSheet
    {
    public:
        /// @brief Creates a StyleSheet from its resolved rules and its resolved dependency entries.
        /// @param rules         The flattened, resolved rules, in source order.
        /// @param dependencies  The resolved font dependency cache entries, kept resident.
        /// @return A shared StyleSheet.
        static Ref<StyleSheet> Create(vector<StyleRule> rules,
                                      vector<Ref<Detail::AssetCacheEntry>> dependencies);

        /// @brief Returns the sheet's resolved rules, in source order.
        [[nodiscard]] const vector<StyleRule>& GetRules() const { return m_Rules; }

    private:
        StyleSheet(vector<StyleRule> rules, vector<Ref<Detail::AssetCacheEntry>> dependencies);

        vector<StyleRule> m_Rules;
        /// @brief Resolved font dependency entries, kept resident so a declaration's font stays loaded.
        vector<Ref<Detail::AssetCacheEntry>> m_Dependencies;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping Gui::StyleSheet to AssetType::StyleSheet.
    template <>
    struct AssetTypeTrait<Gui::StyleSheet>
    {
        /// @brief The asset type tag for StyleSheet.
        static constexpr AssetType Type = AssetType::StyleSheet;
    };
}
