#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/StyleProperty.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Gui
{
    /// @brief One resolved style declaration: a property and the value to write onto Style.
    ///
    /// The value rides a uniform payload the style application interprets by Property:
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

    /// @brief One cooked @keyframes clip: its keyframes, ascending by offset.
    ///
    /// A rule's `animation` declaration references a clip by index (StyleDeclaration::Unit); the
    /// instantiate-time style resolve copies the referenced clip's keyframes onto the element as
    /// a live StyleAnimation, so a document never borrows the sheet across frames.
    struct StyleAnimationClip
    {
        /// @brief The clip's keyframes, ascending by Offset.
        vector<StyleKeyframe> Keyframes;
    };

    /// @brief One cooked gradient fill: its shape, packed geometry, and its baked ramp pixels.
    ///
    /// A `background-gradient` declaration references a gradient by index (StyleDeclaration::Unit).
    /// The multi-stop color is baked at cook time into a Width×1 RGBA16Sfloat ramp (linear
    /// straight-alpha half-floats, HDR-capable); the instantiate-time resolve uploads it to a texture
    /// (through the borrowed AssetManager) and materializes a ResolvedGradient onto the element's
    /// Style. Geometry is in the element's normalized box space and interpreted per Kind (see
    /// Gui::GradientFill).
    struct StyleGradient
    {
        /// @brief The gradient shape (Linear / Radial / Conic).
        GradientKind Kind = GradientKind::Linear;
        /// @brief Linear start point / radial + conic center, in normalized box space.
        vec2 P0{0.0f};
        /// @brief Linear end point / radial (x, y) radii, in normalized box space.
        vec2 P1{0.0f};
        /// @brief Conic start turn in [0, 1); unused by the other kinds.
        f32 AngleOffset = 0.0f;
        /// @brief The ramp's texel count (a Width×1 RGBA16Sfloat image; Ramp holds Width * 8 bytes).
        u32 Width = 0;
        /// @brief The baked ramp pixels, linear straight-alpha RGBA16Sfloat, largest offset t last.
        vector<u8> Ramp;
    };

    /// @brief The value kind of a queryable stylesheet variable.
    enum class StyleVariableKind : u32
    {
        /// @brief A color value, read from all four Payload channels (linear straight-alpha).
        Color = 0,
        /// @brief A single scalar value, read from Payload.x.
        Scalar = 1,
    };

    /// @brief One queryable stylesheet variable: its name, value kind, and resolved payload.
    ///
    /// A sheet carries only its own top-level `--` variables whose value resolves to a color or a
    /// single number; a multi-token variable is cook-time-only and absent. Name is the authored
    /// variable name without the leading `--` (`--accent` is queried as "accent"). Payload holds a
    /// linear straight-alpha color (all four channels) for a Color, or a scalar in Payload.x.
    struct StyleVariable
    {
        /// @brief The variable name without its leading `--`.
        string Name;
        /// @brief Whether the value is a color (all four Payload channels) or a scalar (Payload.x).
        StyleVariableKind Kind = StyleVariableKind::Color;
        /// @brief The resolved value: a linear straight-alpha color, or a scalar in x.
        vec4 Payload{0.0f};
    };

    /// @brief One resolved USS rule: a selector, a pseudo-state, and its declarations.
    ///
    /// The selector matches an element by its element type (Type), one class tag (Class), and/or
    /// one id (Id); an empty field is a wildcard on that axis. State is the interaction state the
    /// rule applies in (None for the base rule; a single pseudo-state bit like Hovered). The style
    /// application matches an element's tags against the rules, cascades the base survivors onto
    /// its resolved Style, and keeps the state-scoped survivors as variants.
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

    /// @brief Applies one resolved declaration onto a Style, overwriting the property it sets.
    ///
    /// Writes the declaration's value onto the matching Style field per its Property, interpreting
    /// the uniform payload (Unit as a LengthKind/enumerator ordinal, Values as the numeric payload).
    /// A TextFont declaration resolves its AssetId to a resident AssetHandle<Font> through `assets`
    /// (a cache lookup: the font is already resident as a load-time dependency) when `assets` is
    /// non-null and the declaration's font is valid; every other property is resolved data written
    /// directly. This is the single write path both inline-style materialization and stylesheet
    /// cascade share, so an inline override and a rule set a property identically.
    /// @param style        The style to write onto.
    /// @param declaration  The declaration to apply.
    /// @param assets       The asset manager a TextFont declaration's AssetId resolves through;
    ///                     borrowed, and may be null (leaving the style's font unchanged, the
    ///                     device-free path).
    void ApplyDeclaration(Style& style, const StyleDeclaration& declaration, AssetManager* assets);

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
        /// @brief Creates a StyleSheet from its resolved rules, clips, gradients, and dependencies.
        /// @param rules         The flattened, resolved rules, in source order.
        /// @param animations    The cooked @keyframes clips, indexed by `animation` declarations.
        /// @param gradients     The cooked gradients, indexed by `background-gradient` declarations.
        /// @param variables     The sheet's own queryable variables (colors and scalars).
        /// @param dependencies  The resolved font dependency cache entries, kept resident.
        /// @return A shared StyleSheet.
        static Ref<StyleSheet> Create(vector<StyleRule> rules,
                                      vector<StyleAnimationClip> animations,
                                      vector<StyleGradient> gradients,
                                      vector<StyleVariable> variables,
                                      vector<Ref<Detail::AssetCacheEntry>> dependencies);

        /// @brief Returns the sheet's resolved rules, in source order.
        [[nodiscard]] const vector<StyleRule>& GetRules() const { return m_Rules; }

        /// @brief Returns the sheet's @keyframes clips, in the index order declarations reference.
        [[nodiscard]] const vector<StyleAnimationClip>& GetAnimations() const
        {
            return m_Animations;
        }

        /// @brief Returns the sheet's gradients, in the index order `background-gradient` references.
        [[nodiscard]] const vector<StyleGradient>& GetGradients() const { return m_Gradients; }

        /// @brief Returns the sheet's own queryable variables (colors and scalars).
        [[nodiscard]] const vector<StyleVariable>& GetVariables() const { return m_Variables; }

        /// @brief Looks up a color variable this sheet owns, by its name without the leading `--`.
        ///
        /// Only the sheet's own top-level `--` variables whose value resolved to a color are found;
        /// a scalar variable, a multi-token variable, and an `@use`d variable are all absent (an
        /// `@use`d variable is queried on the theme sheet that owns it).
        /// @param name  The variable name without its leading `--` (`--accent` is queried as "accent").
        /// @return The linear straight-alpha color, or nullopt if no color variable of that name exists.
        [[nodiscard]] optional<vec4> FindVariableColor(string_view name) const;

        /// @brief Looks up a scalar variable this sheet owns, by its name without the leading `--`.
        ///
        /// Only the sheet's own top-level `--` variables whose value resolved to a single number are
        /// found; a color variable, a multi-token variable, and an `@use`d variable are all absent.
        /// @param name  The variable name without its leading `--` (`--gap` is queried as "gap").
        /// @return The scalar value, or nullopt if no scalar variable of that name exists.
        [[nodiscard]] optional<f32> FindVariableScalar(string_view name) const;

    private:
        StyleSheet(vector<StyleRule> rules, vector<StyleAnimationClip> animations,
                   vector<StyleGradient> gradients, vector<StyleVariable> variables,
                   vector<Ref<Detail::AssetCacheEntry>> dependencies);

        vector<StyleRule> m_Rules;
        /// @brief The cooked @keyframes clips, indexed by an `animation` declaration's Unit.
        vector<StyleAnimationClip> m_Animations;
        /// @brief The cooked gradients, indexed by a `background-gradient` declaration's Unit.
        vector<StyleGradient> m_Gradients;
        /// @brief The sheet's own queryable variables (colors and scalars), in source order.
        vector<StyleVariable> m_Variables;
        /// @brief Resolved font dependency entries, kept resident so a declaration's font stays loaded.
        vector<Ref<Detail::AssetCacheEntry>> m_Dependencies;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping Gui::StyleSheet to AssetTypes::StyleSheet.
    template <>
    struct AssetTypeTrait<Gui::StyleSheet>
    {
        /// @brief The asset type tag for StyleSheet.
        static constexpr AssetTypeId Type = AssetTypes::StyleSheet;
    };
}
