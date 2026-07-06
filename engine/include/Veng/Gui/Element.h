#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Style.h>

namespace Veng::Gui
{
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
        /// @brief A clickable command control.
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

    class Document;

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
        /// @brief The resolved style: the layout inputs Solve reads and the paint inputs Build reads.
        Style ComputedStyle;
        /// @brief The computed rect in document space, filled by Document::Solve.
        Rect Layout;

        /// @brief The interaction-state mask a styling/event layer sets and reads.
        ElementState State = ElementState::None;
        /// @brief Whether the element (and its subtree) is laid out and drawn.
        bool Visible = true;

        /// @brief The element's id tag for id-selector matching; empty when untagged.
        string Id;
        /// @brief The element's class tags for class-selector matching.
        vector<string> Classes;

        /// @brief The Text element's string content; unused by other kinds.
        string Text;

        /// @brief Named bound-value slots a binding layer resolves against a context.
        map<string, string> Bindings;

        /// @brief The element's children, in flow order (owned by the Document).
        vector<Element*> Children;
        /// @brief The element's parent, or nullptr for the root.
        Element* Parent = nullptr;
    };
}
