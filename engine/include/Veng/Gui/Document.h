#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/StyleSheet.h>
#include <Veng/Reflection/TypeId.h>

namespace Veng
{
    class AssetManager;
    class TypeRegistry;
}

namespace Veng::Renderer
{
    class Viewport;
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
        /// recipe element's kind, id, classes, text, bindings, and handlers onto the corresponding
        /// live Element, then cascades the referenced stylesheets onto each element. Cascade
        /// precedence, lowest to highest: a matching rule's None-state declarations in stylesheet
        /// reference order and rule source order, then the element's inline-style declarations —
        /// inline always wins. The result is each element's BaseStyle (and initial ComputedStyle).
        /// State-scoped rule survivors (`:hover`/`:active`/…) are kept as the element's Variants,
        /// selected against its interaction state by Update. The instantiated tree is independent of
        /// the recipe and of any other instance of it — instantiating one UIDocument twice yields two
        /// trees that mutate separately (the Prefab model).
        ///
        /// The document resolves its asset declarations through the given manager — a font
        /// declaration's AssetId loads via a blocking LoadSync<Font>. The document borrows the
        /// manager for its whole life (the resolve re-runs on later style resolves), so the manager
        /// must outlive the document.
        /// @param recipe  The cooked UI document to materialize; must be resident.
        /// @param assets  The asset manager the document's asset declarations load through; borrowed.
        /// @return A newly constructed Document owning an independent copy of the recipe's tree.
        [[nodiscard]] static Unique<Document> Instantiate(const UIDocument& recipe,
                                                          AssetManager& assets);

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

        /// @brief Returns the first element carrying an authored id, or nullptr when none does.
        ///
        /// A depth-first walk from the root in tree order, matching Element::Id exactly. An
        /// empty id matches nothing. Ids are not enforced unique; with duplicates the first in
        /// tree order wins.
        /// @param id  The authored id to find (the markup `id="…"` tag).
        /// @return The first matching element, or nullptr on a miss.
        [[nodiscard]] Element* FindById(string_view id);

        /// @brief Returns the first element carrying an authored id (const), or nullptr.
        /// @param id  The authored id to find.
        [[nodiscard]] const Element* FindById(string_view id) const;

        /// @brief Returns every element carrying the class, in depth-first tree order.
        ///
        /// A depth-first pre-order walk from the root, matching any of Element::Classes exactly. An
        /// empty name matches nothing. Like FindById this is an unindexed O(tree) walk — resolve a
        /// pool once and cache the returned vector rather than calling it per frame.
        /// @param name  The class to match (a markup `class="…"` tag).
        /// @return Every matching element in tree order, or an empty vector on a miss.
        [[nodiscard]] vector<Element*> FindAllByClass(string_view name);

        /// @brief Returns every element carrying the class, in tree order (const).
        /// @param name  The class to match.
        /// @return Every matching element in tree order, or an empty vector on a miss.
        [[nodiscard]] vector<const Element*> FindAllByClass(string_view name) const;

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

        /// @brief Sets a Text element's content, marking layout dirty on a real change.
        ///
        /// Setting the text to its current value is a no-op — no layout dirty, no re-measure — so a
        /// naive per-frame write of unchanged text costs nothing and needs no caller-side guard.
        /// @param element  The element whose text to set.
        /// @param text     The new UTF-8 text content.
        void SetText(Element& element, string_view text);

        /// @brief Sets whether an element (and its subtree) participates in layout and drawing.
        /// @param element  The element to show or hide.
        /// @param visible  True to lay out and draw it, false to skip it.
        void SetVisible(Element& element, bool visible);

        /// @brief Replaces an element's resolved style and marks layout dirty.
        ///
        /// Sets both the element's live ComputedStyle and its BaseStyle, so a later variant/
        /// transition resolve folds active variants over this new base. Marks layout dirty.
        /// @param element  The element whose style to set.
        /// @param style    The new resolved style.
        void SetStyle(Element& element, const Style& style);

        /// @brief Sets an element's opacity multiplier — a paint-only write, no layout re-solve.
        ///
        /// Writes the element's base (and live) style directly, so a per-frame fade never
        /// re-runs the flexbox solve the way a whole-style SetStyle does. An active variant or
        /// in-flight transition on the same property still resolves over the new base. The
        /// opacity composites over the whole subtree at draw, so fading a panel fades its
        /// text, widgets, and children with it.
        /// @param element  The element whose opacity to set.
        /// @param opacity  The opacity multiplier, in 0..1.
        void SetOpacity(Element& element, f32 opacity);

        /// @brief Sets an element's paint rotation — a paint-only write, no layout re-solve.
        ///
        /// Writes the element's base (and live) style directly, so spinning a needle each frame
        /// never re-runs the flexbox solve. An active variant or in-flight transition on the same
        /// property still resolves over the new base. The whole subtree rotates rigidly about the
        /// element's Origin anchor at draw, so rotating a container turns its children, text, and
        /// borders with it.
        /// @param element  The element whose rotation to set.
        /// @param degrees  The rotation in degrees, clockwise-positive in the y-down document space.
        /// @warning Paint only: the element keeps its unrotated flex box, hit-testing stays
        ///          axis-aligned against the unrotated Layout rect, and content clips stay
        ///          axis-aligned scissors.
        void SetRotation(Element& element, f32 degrees);

        /// @brief Sets an element's background fill — a paint-only write, no layout re-solve.
        /// @param element  The element whose background to set.
        /// @param color    The fill color, linear straight-alpha RGBA.
        void SetBackground(Element& element, vec4 color);

        /// @brief Sets (or clears) an element's gradient background — a paint-only write.
        ///
        /// Writes the element's base and live style directly, so the gradient survives the next
        /// Update and re-uploads each frame. Mutating a gradient's P0/P1/AngleOffset and re-setting
        /// it per frame (reusing its ramp handle) animates the gradient — moving a linear axis,
        /// growing a radial, or spinning a conic — with no re-solve. Pass nullopt to remove the
        /// gradient fill and fall back to the flat background color.
        /// @param element   The element whose gradient to set.
        /// @param gradient  The resolved gradient, or nullopt to clear.
        void SetBackgroundGradient(Element& element, optional<ResolvedGradient> gradient);

        /// @brief Sets an element's text fill color — a paint-only write, no layout re-solve.
        /// @param element  The element whose text color to set.
        /// @param color    The text color, linear straight-alpha RGBA.
        void SetTextColor(Element& element, vec4 color);

        /// @brief Sets an Image element's sampled UV sub-rect — a paint-only write, no layout re-solve.
        ///
        /// Writes the element's UV rectangle directly, so re-pointing an Image at a different atlas
        /// region each frame animates a flipbook with no flexbox re-solve; the change takes effect
        /// on the next Build. The UV is unused off an Image element, so setting it there is inert.
        /// @param element  The element whose sampled UV rect to set.
        /// @param uv       The UV sub-rect to sample, in normalized 0..1 texture coordinates.
        void SetImageUv(Element& element, const Rect& uv);

        /// @brief Pins an element absolutely at a rect, dirtying layout only on a real change.
        ///
        /// The one-call form of the pin-at-rect idiom: absolute position, Left/Top insets at
        /// topLeft (Right/Bottom left unset), and a fixed pixel size. Layout is marked dirty
        /// only when the placement actually moved, so re-pinning an unchanged rect each frame
        /// costs no re-solve.
        /// @param element  The element to pin; its position type becomes Absolute.
        /// @param topLeft  The element's top-left corner, in parent-space pixels.
        /// @param size     The element's fixed size, in pixels.
        void SetPlacement(Element& element, vec2 topLeft, vec2 size);

        /// @brief Sets an element's interaction-state mask, re-resolving its live style.
        ///
        /// Selects the variants active in the new mask and folds them over the element's base
        /// style, starting a transition on any transition-able property whose target moved. A
        /// change to a layout input re-dirties the layout; a paint-only change does not. This is
        /// the entry point an interaction layer drives a hover/press/focus change through.
        /// @param element  The element whose state to set.
        /// @param state    The new interaction-state mask.
        void SetState(Element& element, ElementState state);

        /// @brief Replaces an element's per-property transition list.
        ///
        /// Each entry names a property and an ease duration; a property with no entry snaps on a
        /// target change. In-flight tweens for a dropped property are cleared (its value snaps to
        /// the current target on the next resolve).
        /// @param element      The element whose transitions to set.
        /// @param transitions  The transition list, one entry per eased property.
        void SetTransitions(Element& element, vector<StyleTransition> transitions);

        /// @brief Replaces an element's live style-animation list.
        ///
        /// The imperative sibling of a stylesheet `animation` declaration: each entry's
        /// keyframes are applied over the element's resolved style every Update, its clock
        /// starting at the entry's Time. Passing an empty list stops all animation.
        /// @param element     The element whose animations to set.
        /// @param animations  The animation list; keyframes must be ascending by Offset.
        void SetAnimations(Element& element, vector<StyleAnimation> animations);

        /// @brief Advances the style pipeline one frame: variants, transitions, then animations.
        ///
        /// For every element it re-selects the active variants over the base style, advances any
        /// in-flight property tween by delta, and applies each live style animation at its
        /// advanced clock, writing the resolved values into ComputedStyle. A resolved,
        /// transitioned, or animated change to a layout input re-dirties the layout so the
        /// following Solve re-runs; a pure paint change (color/opacity) does not force a
        /// re-solve. Call this once per frame before Solve. A delta of zero resolves variants
        /// without advancing tweens or animation clocks.
        /// @param delta  The frame time step, in seconds.
        void Update(f32 delta);

        /// @brief Returns whether any element has an in-flight transition still animating.
        ///
        /// True while at least one tween has not reached its duration, so a driver knows to keep
        /// calling Update (and re-Building) until the UI settles.
        [[nodiscard]] bool IsAnimating() const;

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

        /// @brief Drives the document's per-frame pipeline into a draw list in one call.
        ///
        /// Runs the three per-frame stages in order — Update(delta) to re-select style variants
        /// and advance transitions and animations, Solve(available) to lay the tree out at the
        /// target extent, then Build(out) to emit its draw primitives — so a host recording the
        /// result into any sink (a viewport composite, a persistent render target) drives a document
        /// through one shared pipeline rather than re-sequencing the stages at each call site. Any
        /// data-binding refresh (UpdateBindings) is the caller's, ahead of this call.
        /// @param available  The available layout region, in the document's layout space (the target
        ///                   extent) — framebuffer pixels at UI scale 1, logical points otherwise.
        /// @param delta      The frame time step, in seconds, forwarded to Update.
        /// @param out        The draw list the built primitives are appended into.
        void Drive(vec2 available, f32 delta, DrawList& out);

        /// @brief Returns whether the tree needs a Solve (structure or style changed since the last).
        [[nodiscard]] bool IsDirty() const { return m_Dirty; }

        /// @brief Returns the topmost visible element whose box contains a document-space point.
        ///
        /// Walks the laid-out tree front-to-back — a later child paints over an earlier one, so the
        /// last child under the point wins — descending only into a subtree whose ancestor clip
        /// rectangles all contain the point (a clipping element hides the parts of its children it
        /// clips away, so those parts do not hit-test). An element styled `pointer-events: none`
        /// is skipped along with its whole subtree. Runs against Element::Layout, so a Solve
        /// must have filled the rects. Returns nullptr when no visible element covers the point.
        /// @param point  The point to test, in document-space pixels (Element::Layout's space).
        /// @return The topmost element under the point, or nullptr on a miss.
        /// @warning Hit-testing is axis-aligned: a `rotation`-styled element is tested against its
        ///          unrotated Layout rect, so a rotated element hit-tests as if unrotated. Rotated
        ///          decor is styled `pointer-events: none` in practice.
        [[nodiscard]] Element* HitTest(vec2 point);

        /// @brief Binds the reflection/handler context every binding and named handler resolves against.
        ///
        /// A `{obj.field}` binding resolves its field path against the context's data object through
        /// the given TypeRegistry, and an `onClick`-style handler name resolves against the
        /// context's handler table. The document borrows both the context and the registry; they
        /// must outlive the binding. Binding a context marks every binding for a re-read on the next
        /// UpdateBindings. Passing a null context clears the binding (handlers stop firing, bound
        /// values stop updating).
        /// @param context   The game-owned binding context, or nullptr to clear.
        /// @param registry  The type registry the field paths are resolved through; must be non-null
        ///                  when context is non-null.
        void BindContext(BindingContext* context, const TypeRegistry* registry);

        /// @brief Binds a context using the registry the document was instantiated against.
        ///
        /// The self-sufficient form for a caller that holds no TypeRegistry of its own — a GuiOverlay
        /// driver binding its view-model in OnInstantiate. Resolves the registry from the document's
        /// own AssetManager, so `{obj.field}` bindings and named handlers resolve without the caller
        /// re-supplying it. Passing null clears the binding. The context is borrowed and must outlive
        /// the binding.
        /// @param context  The game-owned binding context, or nullptr to clear.
        void BindContext(BindingContext* context);

        /// @brief Re-resolves every `{path}` binding whose context changed and writes the elements.
        ///
        /// Compares the bound context's version against the one last resolved; on a move it walks
        /// each bound element, resolves each binding's field path against the data object through
        /// reflection, and writes the resolved value into the target attribute (text/visibility),
        /// dirtying layout where the write changes a layout input. A no-op when no context is bound
        /// or its version is unchanged since the last resolve. Call once per frame before Solve.
        void UpdateBindings();

        /// @brief Routes a pointer event into the tree with capture → target → bubble propagation.
        ///
        /// Hit-tests the event position to a target, drives the hover/active interaction state as
        /// the pointer enters/leaves and presses/releases (through SetState, so a styling layer
        /// reacts), and dispatches the event down the ancestor path (root→target) then up
        /// (target→root). A press followed by a release on the same element synthesizes a Click,
        /// which fires the element's `onClick` handler. An element's handler consumes the event by
        /// setting PointerEvent::Handled, stopping further propagation.
        /// @param event  The pointer event; its Position is in document-space pixels.
        /// @return True when the event was consumed by some element (or drove an interaction).
        bool DispatchPointer(PointerEvent& event);

        /// @brief Routes a navigation action: moves focus, or activates/cancels the focused element.
        ///
        /// A Move*/Next/Previous action moves focus to the resolved focusable and updates the
        /// focused interaction state (the focus ring is a `:focus` variant a styling layer draws).
        /// Confirm synthesizes a primary click on the focused element (so a button fires the same
        /// `onClick` whether clicked or confirmed); Cancel fires the focused element's `onCancel`
        /// handler if present. Directional moves pick the nearest focusable in the pressed direction
        /// by laid-out geometry; Next/Previous walk tree order.
        /// @param action  The navigation action to apply.
        /// @return True when the action moved focus or fired a handler.
        bool Navigate(NavAction action);

        /// @brief Delivers a typed Unicode codepoint to the focused element.
        ///
        /// Routes the codepoint to the focused element's `onText` handler when one is registered;
        /// the widget layer's text input reads it through GetPendingCodepoint while the handler
        /// runs. A no-op when nothing is focused or the focused element registers no text handler.
        /// @param codepoint  The Unicode codepoint produced by text input.
        /// @return True when the codepoint was delivered to a handler.
        bool DispatchText(u32 codepoint);

        /// @brief Returns the codepoint being delivered while an `onText` handler runs, else zero.
        ///
        /// DispatchText sets this for the duration of the handler call so a bare EventHandler (which
        /// carries only the element) reads the character a text-input widget consumes; it is zero
        /// outside a text dispatch.
        [[nodiscard]] u32 GetPendingCodepoint() const { return m_PendingCodepoint; }

        /// @brief Sets which element holds focus, updating the focused interaction state.
        ///
        /// Clears the Focused bit on the previously-focused element and sets it on the new one
        /// (through SetState, so the `:focus` variant resolves). Passing nullptr blurs without a new
        /// focus. An element that is not focusable is rejected (focus is unchanged).
        /// @param element  The element to focus, or nullptr to blur.
        void SetFocus(Element* element);

        /// @brief Returns the focused element, or nullptr when nothing holds focus.
        [[nodiscard]] Element* GetFocused() const { return m_Focused; }

        /// @brief Returns whether the viewport (context) treats this document as display-only.
        ///
        /// A document is **display-only by default**: its bindings update and it draws, but it
        /// hit-tests and takes no focus until the game opens interactivity on it. The input layer
        /// (the router consumer) skips a display-only document. This reflects the flag SetInteractive
        /// set; it is not itself an input takeover (the game opens a SeatFocusScope on the
        /// document's seat for that).
        [[nodiscard]] bool IsInteractive() const { return m_Interactive; }

        /// @brief Marks the document interactive (hit-testable, focusable) or display-only.
        ///
        /// The game flips this on while it holds the document's seat (a SeatFocusScope) so the input
        /// consumer routes pointer/focus into it, and off when it releases the seat. Off — the
        /// default — the document draws and its bindings update but it consumes no input. Clearing
        /// it blurs any focused element.
        /// @param interactive  True to route input into the document, false for display-only.
        void SetInteractive(bool interactive);

        /// @brief Returns the viewport this document is attached to, or nullptr when detached.
        ///
        /// A document attaches to at most one viewport at a time (Viewport::AttachDocument). The
        /// back-reference the destructor self-detaches through, and the document's input identity:
        /// its inherited seat is the host viewport's associated seat (Viewport::GetSeat).
        [[nodiscard]] Renderer::Viewport* GetHostViewport() const { return m_HostViewport; }

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

        /// @brief Measures an arbitrary run against a resolved style, the same way an element is.
        ///
        /// Uses the installed text measurer when one is set, otherwise shapes the run through the
        /// style's resident font; returns a zero size when neither is available. Measuring a run
        /// that is not an element's whole text is what a caret offset needs — the width of the
        /// value's prefix up to the edit position.
        /// @param text            The run to measure.
        /// @param style           The resolved style whose font and size to shape against.
        /// @param availableWidth  The width to wrap within, or nullopt for an unconstrained measure.
        /// @return The measured text block size, in pixels.
        [[nodiscard]] vec2 MeasureStyledText(string_view text, const Style& style,
                                             optional<f32> availableWidth) const;

        /// @brief Sets a Slider's or Checkbox's scalar value, clamping, stepping, and notifying.
        ///
        /// Clamps the value to the control's `[Min, Max]` range and snaps it to Step when the step is
        /// positive, writes it into the element's WidgetState, and — when the value actually changed
        /// — fires the element's `onChange` handler so a two-way control writes back through the
        /// bound handler path. A Checkbox additionally reflects the value into its Checked state bit
        /// (a non-zero value is checked), driving the `:checked` style variant. A no-op on an element
        /// that is not a value-bearing control.
        /// @param element  The control element whose value to set.
        /// @param value    The new value, before clamping/stepping.
        void SetWidgetValue(Element& element, f32 value);

        /// @brief Returns a control element's current scalar value from its WidgetState.
        /// @param element  The control element.
        /// @return The value the widget layer holds for it.
        [[nodiscard]] f32 GetWidgetValue(const Element& element) const
        {
            return element.Widget.Value;
        }

        /// @brief Scrolls a ScrollView by a pixel delta, clamped to its content extent.
        ///
        /// Adds the delta to the ScrollView's offset and clamps it so the content stays within its
        /// scrollable range (never scrolling past the last child). The offset shifts the laid-out
        /// children on the next Solve. A no-op on an element that is not a ScrollView.
        /// @param element  The ScrollView element to scroll.
        /// @param delta    The scroll delta, in pixels (positive scrolls content up/left).
        void ScrollBy(Element& element, vec2 delta);

        /// @brief Initializes a control's widget state from its authored config and kind.
        ///
        /// Reads the control's config attributes off its Bindings map — a Slider's `min`/`max`/
        /// `step`/`value`, a ProgressBar's initial `value`, a Checkbox's initial checked state — into
        /// its WidgetState, and marks the interactive kinds (Button/Checkbox/Slider/TextInput/
        /// ScrollView) focusable. Instantiate calls this per element; an imperative tree calls it on a
        /// control after setting its Bindings and before the first frame. A no-op on a plain
        /// Panel/Text/Image.
        /// @param element  The control element to initialize.
        void InitWidget(Element& element);

    private:
        // Viewport::AttachDocument / DetachDocument set and clear m_HostViewport, so the destructor
        // self-detaches through it — the sole writer of the back-reference.
        friend class Renderer::Viewport;

        struct YogaTree;

        /// @brief Allocates a new element in the arena and mirrors it into the layout tree.
        Element& CreateElement(ElementKind kind);

        /// @brief Destroys an element and its subtree, releasing their layout nodes.
        void DestroySubtree(Element& element);

        /// @brief Re-selects active variants over the base and advances one element's tweens.
        void UpdateElement(Element& element, f32 delta);

        /// @brief Pushes one element's style layout inputs into its mirrored layout node.
        void ApplyStyle(Element& element);

        /// @brief Reads each element's computed rect back into Element::Layout from the mirror.
        void ReadLayout(Element& element, vec2 origin);

        /// @brief Widens every Table's cells to their solved per-column maxima.
        ///
        /// Runs between the two layout passes of a Solve on a document holding a Table: reads each
        /// cell's natural margin-box width from the first solve, takes the per-column maximum
        /// across the table's rows, and raises each cell's layout-node min-width to its column's
        /// width. Returns whether any node changed (the caller re-runs the layout when so).
        bool AlignTableColumns();

        /// @brief Emits one element's primitives, then recurses into its children.
        ///
        /// The inherited opacity is the product of every ancestor's style opacity; it folds
        /// into each primitive's alpha and multiplies down the subtree, so an element's
        /// opacity fades its whole subtree as one. A zero product skips the subtree entirely.
        void BuildElement(const Element& element, DrawList& list, f32 inherited) const;

        /// @brief Recursive front-to-back hit-test honoring the ancestor clip chain.
        [[nodiscard]] Element* HitTestElement(Element& element, vec2 point, optional<Rect> clip);

        /// @brief Resolves and writes one element's bindings against the bound data object.
        void ResolveElementBindings(Element& element);

        /// @brief Re-syncs every List's item children against its bound array's current size.
        void SyncLists();

        /// @brief Instantiates or removes a List's item children to match the bound array size.
        void SyncList(Element& list);

        /// @brief Returns whether an element sits under a List (an instantiated item subtree).
        [[nodiscard]] bool IsListItem(const Element& element) const;

        /// @brief Lifts an authored subtree out of the live tree into standalone template nodes.
        ///
        /// Moves the element and every descendant into `owned` (which takes their storage) and
        /// returns the detached root; the live Yoga nodes are freed and the elements dropped from
        /// live storage.
        Element* DetachTemplate(Element& element, vector<Unique<Element>>& owned);

        /// @brief Instantiates a live copy of a template subtree as a child of a parent element.
        Element& CloneTemplate(Element& parent, const Element& node);

        /// @brief Resolves a List item subtree's bindings against one array element's fields.
        void ResolveItemBindings(Element& element, void* itemBase, TypeId itemType);

        /// @brief Sets whether an element takes focus by kind, marking the interactive controls.
        void ApplyWidgetFocusability(Element& element);

        /// @brief Applies a pointer press/drag to a Slider or ScrollView, updating its value/offset.
        bool DriveWidgetPointer(Element& element, const PointerEvent& event);

        /// @brief Applies a directional nudge to the focused Slider, or a scroll to a ScrollView.
        bool DriveWidgetNavigation(Element& element, NavAction action);

        /// @brief Inserts or backspaces a codepoint into the focused TextInput's bound text.
        bool DriveWidgetText(Element& element, u32 codepoint);

        /// @brief Emits a control's own painted parts (a Slider track/thumb, a ProgressBar fill).
        ///
        /// The opacity is the element's composited subtree opacity, folded into each part's alpha.
        void BuildWidget(const Element& element, DrawList& list, f32 opacity) const;

        /// @brief Emits a TextInput's own value run and, while it holds focus, its caret bar.
        ///
        /// The opacity is the element's composited subtree opacity, folded into both alphas.
        void BuildTextInput(const Element& element, DrawList& list, f32 opacity) const;

        /// @brief Collects every focusable, visible element into m_FocusOrder in tree order.
        void GatherFocusables(Element& element, vector<Element*>& out) const;

        /// @brief Fires the named handler an element carries for an event, if the context has it.
        [[nodiscard]] bool FireHandler(Element& element, string_view event);

        /// @brief Dispatches a pointer event down (capture) then up (bubble) the target's ancestor path.
        bool RoutePointerPath(PointerEvent& event);

        /// @brief The layout mirror wrapping the flexbox node tree and the Element↔node map.
        Unique<YogaTree> m_Yoga;

        /// @brief Stable storage of every element; entries are never relocated.
        vector<Unique<Element>> m_Elements;

        /// @brief The root element (owned by m_Elements).
        Element* m_Root = nullptr;

        /// @brief The optional measurement override; unset uses the style's font.
        TextMeasurer m_Measurer;

        /// @brief The asset manager a font declaration resolves through; null on the device-free path.
        ///
        /// Borrowed from Instantiate and kept for the document's life, since a state resolve re-runs
        /// the font binding. Null (an imperatively-built or test document with no manager) leaves a
        /// font declaration unresolved, exactly as an empty resolver did.
        AssetManager* m_Assets = nullptr;

        /// @brief Whether structure or style changed since the last Solve.
        bool m_Dirty = true;

        /// @brief The available size the last Solve ran against; a change re-runs Solve.
        vec2 m_LastAvailable{-1.0f};

        /// @brief The viewport hosting this document, or nullptr when detached.
        ///
        /// Set by Viewport::AttachDocument and cleared by DetachDocument; the destructor detaches
        /// through it so dropping the owning Unique removes the document from its viewport's layer
        /// stack with no dangling pointer.
        Renderer::Viewport* m_HostViewport = nullptr;

        /// @brief The bound reflection/handler context, or nullptr when none is bound.
        BindingContext* m_Context = nullptr;

        /// @brief The type registry binding field paths resolve through; null when no context.
        const TypeRegistry* m_Registry = nullptr;

        /// @brief The context version the last binding resolve read; a move re-reads bindings.
        u64 m_BoundVersion = 0;

        /// @brief The focused element, or nullptr when nothing holds focus.
        Element* m_Focused = nullptr;

        /// @brief One List's detached item template: the authored subtree cloned per array element.
        ///
        /// A List's authored children are its item template. On first sync the template subtree is
        /// detached from the live tree into this store (so it never lays out or draws), and each
        /// bound-array element instantiates a clone of it as a live child of the List. Owned holds
        /// every template node (roots and descendants) so the whole subtree stays alive; Roots names
        /// the top-level template nodes cloned once per array element.
        struct ListTemplate
        {
            /// @brief Every node of the template subtree, owning the tree's storage.
            vector<Unique<Element>> Owned;
            /// @brief The top-level template roots, one clone of each per array element.
            vector<Element*> Roots;
        };

        /// @brief Per-List detached item templates, keyed by the List element.
        map<Element*, ListTemplate> m_ListTemplates;

        /// @brief The element a pointer press landed on, awaiting a release to complete a click.
        Element* m_PressTarget = nullptr;

        /// @brief The element the pointer last hovered, tracked so a move emits Enter/Leave.
        Element* m_HoverTarget = nullptr;

        /// @brief The pointer position the last ScrollView drag sampled, for a per-move pan delta.
        vec2 m_LastScrollPointer{0.0f};

        /// @brief Whether the document routes input (hit-tests, takes focus) or is display-only.
        bool m_Interactive = false;

        /// @brief The codepoint DispatchText exposes while an onText handler runs; zero otherwise.
        u32 m_PendingCodepoint = 0;
    };
}
