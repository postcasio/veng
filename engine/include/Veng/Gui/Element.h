#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/StyleProperty.h>

namespace Veng::Gui
{
    struct StyleDeclaration;
    /// @brief The closed set of element kinds a document tree can hold.
    ///
    /// The whole set is enumerated so a document can carry any authored tag. Panel, Text,
    /// and Image are the kinds with layout and paint behavior: Panel is a styled flex box,
    /// Text is a shaped text leaf whose measured size drives layout, and Image is a textured
    /// box. The remaining kinds are structural placeholders that lay out and paint as a Panel
    /// does; their interactive behavior is provided by the widget layer.
    enum class ElementKind : u32
    {
        /// @brief A styled flex container — the general layout box.
        Panel,
        /// @brief A shaped, styleable text leaf.
        Text,
        /// @brief A textured box drawing an image.
        Image,
        /// @brief A clickable command control: a label leaf sized by and drawing its Text, centered.
        Button,
        /// @brief A two-state toggle control.
        Checkbox,
        /// @brief A draggable value control.
        Slider,
        /// @brief A determinate progress indicator.
        ProgressBar,
        /// @brief A single-line editable text control.
        TextInput,
        /// @brief A clipped, scrollable content region.
        ScrollView,
        /// @brief A data-bound repeater of child elements.
        List,
        /// @brief A column-aligning container: its rows' cells share per-column widths.
        ///
        /// Lays out and paints as a Panel; each direct child is a row (a row-direction flex
        /// container), and the k-th in-flow cell of every row is widened to the column's widest
        /// cell, so the rows read as a table. A cell with a positive flex-grow is an elastic
        /// filler rather than a column: it absorbs each row's slack, so the fixed columns after
        /// it right-anchor to the rows' shared right edge. With an `items` binding it repeats
        /// its authored row template per bound array element, exactly as a List does.
        Table,
    };

    /// @brief Transient interaction-state bits an element carries for styling and events.
    ///
    /// The bits are a bitmask (combine with the bitwise operators). They record the pointer
    /// and focus state an interaction layer sets and a styling layer reads; the layout and
    /// draw here do not consult them.
    enum class ElementState : u32
    {
        /// @brief No interaction state.
        None = 0,
        /// @brief The pointer is over the element.
        Hovered = 1u << 0,
        /// @brief The element is being pressed.
        Active = 1u << 1,
        /// @brief The element holds input focus.
        Focused = 1u << 2,
        /// @brief The element is disabled and rejects interaction.
        Disabled = 1u << 3,
        /// @brief The element is in a checked/on state.
        Checked = 1u << 4,
    };

    /// @brief Returns the union of two interaction-state masks.
    /// @param a  The first mask.
    /// @param b  The second mask.
    /// @return The bitwise-or of the two masks.
    constexpr ElementState operator|(ElementState a, ElementState b)
    {
        return static_cast<ElementState>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    /// @brief Returns the intersection of two interaction-state masks.
    /// @param a  The first mask.
    /// @param b  The second mask.
    /// @return The bitwise-and of the two masks.
    constexpr ElementState operator&(ElementState a, ElementState b)
    {
        return static_cast<ElementState>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    /// @brief One state-scoped style variant: a pseudo-state and the declarations it applies.
    ///
    /// The resolved declarations a stylesheet cascade kept for a single interaction state
    /// (`:hover`/`:active`/`:focus`/`:disabled`/`:checked`). State is exactly one pseudo-state
    /// bit; Declarations are in cascade source order (later entries win). Variant selection folds
    /// the variants whose State bit is set in the element's live interaction mask over its base
    /// style, so the runtime never runs a selector engine — matching happened once at resolve time.
    struct StyleVariant
    {
        /// @brief The single pseudo-state bit this variant applies in.
        ElementState State = ElementState::None;
        /// @brief The declarations to apply when the state is active, in cascade source order.
        vector<StyleDeclaration> Declarations;
    };

    /// @brief A per-property time-based transition: which property eases and over how long.
    ///
    /// When the target value of Property changes as the active style switches, the live value
    /// eases from its old value to the new one over Duration seconds rather than snapping. A
    /// property with no transition entry snaps. Only animatable properties (colors, opacity,
    /// scalar sizes, and length/inset payloads) interpolate; a non-animatable property is set to
    /// its target immediately even with a transition entry.
    struct StyleTransition
    {
        /// @brief The style property this transition eases.
        StyleProperty Property = StyleProperty::Background;
        /// @brief The ease duration, in seconds; a non-positive duration snaps.
        f32 Duration = 0.0f;
    };

    /// @brief One keyframe of a style animation: a normalized time offset and its declarations.
    struct StyleKeyframe
    {
        /// @brief The keyframe's position along the clip, normalized to [0, 1].
        f32 Offset = 0.0f;
        /// @brief The property values this keyframe holds, in source order.
        vector<StyleDeclaration> Declarations;
    };

    /// @brief How a style animation's clock maps onto its clip once the first pass completes.
    enum class AnimationLoopMode : u8
    {
        /// @brief Wraps back to the start each cycle.
        Loop,
        /// @brief Plays forward then backward, mirroring each alternate cycle.
        PingPong,
        /// @brief Plays once and holds the final keyframe.
        Once,
    };

    /// @brief One live style animation on an element: the clip's keyframes, timing, and clock.
    ///
    /// The keyframes are held by value (copied from the stylesheet's clip at instantiate, or
    /// authored imperatively), in ascending Offset order. Document::Update advances Time and
    /// writes each animated property's interpolated value into the element's live style each
    /// frame — over the variant-resolved target, under any in-flight transition tween.
    struct StyleAnimation
    {
        /// @brief The clip's keyframes, ascending by Offset.
        vector<StyleKeyframe> Keyframes;
        /// @brief One clip cycle's length, in seconds.
        f32 Duration = 1.0f;
        /// @brief How the clock maps onto the clip after the first cycle.
        AnimationLoopMode Mode = AnimationLoopMode::Loop;
        /// @brief The running clock, in seconds; advanced by Document::Update.
        f32 Time = 0.0f;
    };

    class Document;

    /// @brief The per-element runtime state the widget layer maintains behind a control's kind.
    ///
    /// A control element carries a scalar Value plus the range and step the widget interprets it
    /// against, a scroll offset, and a text caret — the mutable state its behavior reads and writes
    /// each frame. Which fields are live depends on the element's kind: a Slider reads Value against
    /// Min/Max/Step, a ProgressBar reads Value as a `[0,1]` fill, a Checkbox holds Value 0 or 1, a
    /// TextInput tracks Caret over the element's Text, and a ScrollView holds ScrollOffset. It is
    /// default-constructed on every element and left untouched on a plain Panel/Text/Image.
    struct WidgetState
    {
        /// @brief The control's scalar value: a Slider's position, a ProgressBar's fill, a Checkbox's 0/1.
        f32 Value = 0.0f;
        /// @brief The lower bound of a Slider's value range.
        f32 Min = 0.0f;
        /// @brief The upper bound of a Slider's value range.
        f32 Max = 1.0f;
        /// @brief The Slider's discrete step; a non-positive step is continuous.
        f32 Step = 0.0f;
        /// @brief Whether a Slider runs vertically (Min at the bottom, Max at the top).
        ///
        /// Authored as the markup `orientation="vertical"` attribute; the default is a
        /// horizontal slider (Min at the left, Max at the right).
        bool Vertical = false;
        /// @brief The ScrollView's content offset, in pixels (subtracted from child positions).
        vec2 ScrollOffset{0.0f};
        /// @brief The TextInput caret position, as a codepoint index into the element's Text.
        u32 Caret = 0;
        /// @brief The bound-array context version a List last synced its item children against.
        u64 SyncVersion = 0;
    };

    /// @brief One in-flight property tween: the property, its source value, and elapsed time.
    ///
    /// Records a property transitioning toward its resolved target. From holds the value the
    /// property held when the target last changed; Elapsed advances by the per-frame delta up to
    /// the transition duration, and the live value is the interpolation of From toward the target
    /// at Elapsed/Duration. Internal state Document::Update maintains; not authored.
    struct StyleTween
    {
        /// @brief The property being eased.
        StyleProperty Property = StyleProperty::Background;
        /// @brief The value the property held when its target last changed (the ease start).
        vec4 From{0.0f};
        /// @brief The target value being eased toward (the ease end).
        vec4 To{0.0f};
        /// @brief Seconds elapsed since the target changed.
        f32 Elapsed = 0.0f;
        /// @brief The ease duration in seconds, copied from the matching transition.
        f32 Duration = 0.0f;
    };

    /// @brief One node of a retained document tree: a kind, a resolved style, and a computed rect.
    ///
    /// An element owns its children, held by the enclosing Document (which single-owns the whole
    /// tree). It carries its resolved Style, the interaction state a styling/event layer drives,
    /// its id and class tags for selector matching, its Text content, and bound-value slots a
    /// binding layer writes. Layout is the rect Document::Solve computes in document space; it is
    /// meaningful only after a Solve. Mutate structure and style through the Document so the layout
    /// is marked dirty and the mirrored layout node stays in sync.
    struct Element
    {
        /// @brief The element's kind, fixed at construction.
        ElementKind Kind = ElementKind::Panel;
        /// @brief The live resolved style: the layout inputs Solve reads and the paint inputs Build reads.
        Style ComputedStyle;
        /// @brief The base resolved style — inline + None-state cascade, before any active variant.
        Style BaseStyle;
        /// @brief The computed rect in document space, filled by Document::Solve.
        Rect Layout;

        /// @brief The state-scoped style variants, in cascade source order.
        vector<StyleVariant> Variants;
        /// @brief The per-property transitions that ease a target change over time.
        vector<StyleTransition> Transitions;
        /// @brief The in-flight property tweens Document::Update maintains.
        vector<StyleTween> Tweens;
        /// @brief The live style animations Document::Update advances and applies each frame.
        vector<StyleAnimation> Animations;

        /// @brief The interaction-state mask a styling/event layer sets and reads.
        ElementState State = ElementState::None;
        /// @brief Whether the element (and its subtree) is laid out and drawn.
        bool Visible = true;

        /// @brief Whether the element can hold input focus and take part in focus navigation.
        ///
        /// A focusable element is a stop for keyboard/gamepad directional and Tab navigation and
        /// receives Confirm/Cancel while focused. The widget layer sets this on the controls that
        /// take focus (a button, a text input); a plain Panel/Text leaves it false, so it never
        /// steals focus from the controls inside it.
        bool Focusable = false;

        /// @brief The element's id tag for id-selector matching; empty when untagged.
        string Id;
        /// @brief The element's class tags for class-selector matching.
        vector<string> Classes;

        /// @brief The Text element's string content; unused by other kinds.
        string Text;

        /// @brief An Image element's resident source texture; empty leaves the element un-textured.
        ///
        /// Instantiate resolves the recipe's `src` AssetId through the borrowed AssetManager and
        /// stores the handle here so the texture stays loaded for the document's lifetime, exactly
        /// as Style::TextFont keeps a font resident. The paint reads the bindless slots
        /// ImageTexture/ImageSampler, which the resolve fills from this handle; an imperative author
        /// may instead set those slots directly to a runtime texture with no cache entry.
        AssetHandle<Texture> Image;
        /// @brief The bindless texture slot an Image paints from; invalid leaves the element un-textured.
        Renderer::TextureHandle ImageTexture;
        /// @brief The bindless sampler slot an Image paints with.
        Renderer::SamplerHandle ImageSampler;
        /// @brief An Image element's tint, linear straight-alpha RGBA; the style opacity folds into the alpha at paint.
        vec4 ImageTint{1.0f};
        /// @brief The UV sub-rect an Image samples (an atlas region); the whole texture by default.
        Rect ImageUv{.Min = vec2(0.0f), .Size = vec2(1.0f)};

        /// @brief Named bound-value slots a binding layer resolves against a context.
        map<string, string> Bindings;

        /// @brief The widget-layer runtime state a control's behavior reads and writes.
        WidgetState Widget;

        /// @brief The element's children, in flow order (owned by the Document).
        vector<Element*> Children;
        /// @brief The element's parent, or nullptr for the root.
        Element* Parent = nullptr;
    };
}
