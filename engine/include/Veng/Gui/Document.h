#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Style.h>

namespace Veng
{
    template <class T>
    class AssetHandle;
}

namespace Veng::Gui
{
    class UIDocument;

    /// @brief A retained tree of UI elements with flexbox layout, drawn through a DrawList.
    ///
    /// A Document single-owns a persistent tree rooted at Root(). It mirrors the tree into an
    /// internal flexbox solver: Solve pushes each element's style layout inputs into the mirror,
    /// runs the layout, and reads each computed rect back into Element::Layout; Build walks the
    /// laid-out tree and emits the draw primitives (background, border, text, then children,
    /// clip-pushed when the element clips). Structure and style are mutated only through the
    /// Document so the mirror stays in sync and the layout is marked dirty. A Document survives
    /// across frames; a clean Solve at an unchanged available size is a no-op.
    ///
    /// The tree is not thread-safe; construct, mutate, Solve, and Build it on one thread.
    class Document
    {
    public:
        /// @brief A device-free text-measurement function: a string and style to an intrinsic size.
        ///
        /// Given the element's text, its resolved style, and the available width the solver offers
        /// (nullopt for an unconstrained measure), it returns the text block's pixel size. When set
        /// through SetTextMeasurer it overrides the font-driven measurement for every Text element.
        using TextMeasurer =
            function<vec2(string_view text, const Style& style, optional<f32> availableWidth)>;

        /// @brief Materializes an independent live tree from a cooked UIDocument recipe.
        ///
        /// Builds a fresh Document whose element tree mirrors the recipe's structure, copying each
        /// recipe element's kind, id, classes, text, inline-style declarations, bindings, and
        /// handlers onto the corresponding live Element. The instantiated tree is independent of the
        /// recipe and of any other instance of it — instantiating one UIDocument twice yields two
        /// trees that mutate separately (the Prefab model). The referenced stylesheets are not yet
        /// applied: matching their rules onto each element's resolved Style is the style-application
        /// step. Inline-style declarations are stored on each Element as its authored overrides.
        /// @param recipe  The cooked UI document to materialize; must be resident.
        /// @return A newly constructed Document owning an independent copy of the recipe's tree.
        [[nodiscard]] static Unique<Document> Instantiate(const UIDocument& recipe);

        /// @brief Constructs an empty document with a single Panel root.
        Document();

        /// @brief Destroys the document and its whole tree.
        ~Document();

        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;

        /// @brief Returns the root element (a Panel, created with the document).
        [[nodiscard]] Element& Root();

        /// @brief Returns the root element (const).
        [[nodiscard]] const Element& Root() const;

        /// @brief Adds a new child element of the given kind under a parent.
        ///
        /// The child is appended after the parent's existing children and mirrored into the layout
        /// tree. The returned reference is stable for the life of the element.
        /// @param parent  The element to add the child under; must belong to this document.
        /// @param kind    The kind of the new element.
        /// @return A reference to the newly added element.
        Element& Add(Element& parent, ElementKind kind);

        /// @brief Removes an element and its whole subtree from the document.
        ///
        /// Detaches the element from its parent and destroys it with every descendant, releasing
        /// their mirrored layout nodes. The root cannot be removed.
        /// @param element  The element to remove; must belong to this document and not be the root.
        void Remove(Element& element);

        /// @brief Sets a Text element's content and marks layout dirty.
        /// @param element  The element whose text to set.
        /// @param text     The new UTF-8 text content.
        void SetText(Element& element, string_view text);

        /// @brief Sets whether an element (and its subtree) participates in layout and drawing.
        /// @param element  The element to show or hide.
        /// @param visible  True to lay out and draw it, false to skip it.
        void SetVisible(Element& element, bool visible);

        /// @brief Replaces an element's resolved style and marks layout dirty.
        /// @param element  The element whose style to set.
        /// @param style    The new resolved style.
        void SetStyle(Element& element, const Style& style);

        /// @brief Installs a text-measurement function overriding font-driven Text sizing.
        ///
        /// With a measurer set, Text elements size through it instead of their style's font, which
        /// makes layout exercisable without a resident font. Passing an empty function restores the
        /// font-driven path. Marks layout dirty.
        /// @param measurer  The measurement function, or an empty function to clear the override.
        void SetTextMeasurer(TextMeasurer measurer);

        /// @brief Lays out the tree to fill the available size, filling every Element::Layout.
        ///
        /// Pushes each element's layout style into the mirror, runs the flexbox solve at the
        /// available extent, and reads each computed rect back. It is a no-op when the tree is clean
        /// and the available size is unchanged since the last Solve; a structural or style change,
        /// or a changed available size, re-runs it.
        /// @param available  The available layout region, in framebuffer pixels.
        void Solve(vec2 available);

        /// @brief Walks the laid-out tree and emits its draw primitives into the draw list.
        ///
        /// Per visible element, in tree order: the background quad and border, then its text or
        /// image content, then its children — pushing a clip when the element clips its content.
        /// @param list  The draw list to append into.
        /// @pre Solve has run since the last structural or style change.
        void Build(DrawList& list) const;

        /// @brief Returns whether the tree needs a Solve (structure or style changed since the last).
        [[nodiscard]] bool IsDirty() const { return m_Dirty; }

        /// @brief Measures a Text element's intrinsic size for a given available width.
        ///
        /// Uses the installed text measurer when one is set, otherwise shapes the element's text
        /// through its style's resident font. Returns a zero size when neither a measurer nor a
        /// loaded font is available. This is the device-free sizing the layout leaf calls.
        /// @param element         The element whose text to measure.
        /// @param availableWidth  The width to wrap within, or nullopt for an unconstrained measure.
        /// @return The measured text block size, in pixels.
        [[nodiscard]] vec2 MeasureElementText(const Element& element,
                                              optional<f32> availableWidth) const;

    private:
        struct YogaTree;

        /// @brief Allocates a new element in the arena and mirrors it into the layout tree.
        Element& CreateElement(ElementKind kind);

        /// @brief Destroys an element and its subtree, releasing their layout nodes.
        void DestroySubtree(Element& element);

        /// @brief Pushes one element's style layout inputs into its mirrored layout node.
        void ApplyStyle(Element& element);

        /// @brief Reads each element's computed rect back into Element::Layout from the mirror.
        void ReadLayout(Element& element, vec2 origin);

        /// @brief Emits one element's primitives, then recurses into its children.
        void BuildElement(const Element& element, DrawList& list) const;

        /// @brief The layout mirror wrapping the flexbox node tree and the Element↔node map.
        Unique<YogaTree> m_Yoga;

        /// @brief Stable storage of every element; entries are never relocated.
        vector<Unique<Element>> m_Elements;

        /// @brief The root element (owned by m_Elements).
        Element* m_Root = nullptr;

        /// @brief The optional measurement override; unset uses the style's font.
        TextMeasurer m_Measurer;

        /// @brief Whether structure or style changed since the last Solve.
        bool m_Dirty = true;

        /// @brief The available size the last Solve ran against; a change re-runs Solve.
        vec2 m_LastAvailable{-1.0f};
    };
}
