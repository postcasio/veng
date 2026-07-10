#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class Window;
    class Input;
    class InputConsumer;
    class Event;
}

namespace Veng::Renderer
{
    class Viewport;
}

namespace Veng
{
    /// @brief The layer that owns a seat's input this frame.
    enum class InputFocus
    {
        /// @brief UI owns the seat's input: events reach the consumer registry and the Input
        ///        snapshot both.
        UI,
        /// @brief The running game owns the seat's input exclusively: events reach the Input
        ///        snapshot only.
        Gameplay,
    };

    /// @brief An opaque handle to one pushed focus entry, returned by PushFocus.
    ///
    /// Popping is by token (PopFocus), so an entry is removed wherever it sits in its seat's
    /// stack rather than by an anonymous count decrement — an interleaved second scope cannot
    /// pop the wrong entry, and a mispaired or double pop is caught rather than silently
    /// corrupting the mode. A default-constructed token is invalid and names no entry.
    struct FocusToken
    {
        /// @brief The token's identity; zero is the invalid, names-no-entry token.
        u64 Value = 0;

        /// @brief Whether this token names a real pushed entry.
        [[nodiscard]] bool IsValid() const { return Value != 0; }

        /// @brief Member-wise equality on the identity value.
        bool operator==(const FocusToken&) const = default;
    };

    /// @brief The seat the free pointer belongs to this frame, and its viewport-local position.
    ///
    /// The router recomputes this once per frame from the pointer's window point against the
    /// associated Presented viewport regions: the first region containing the point owns the
    /// pointer, and the position is that region's normalized [0,1] coordinate scaled to the
    /// region's extent. A seat's SeatInputView reads the pointer only when it owns it this frame
    /// (and holds the keyboard/mouse). While the cursor is captured (gameplay focus) the region
    /// hit-test is skipped and the single keyboard/mouse seat owns the pointer wholly. When no
    /// region contains the pointer (or none is associated) Owner is Entity::Null and no seat reads
    /// a position.
    struct PointerRouting
    {
        /// @brief The seat that owns the pointer this frame, or Entity::Null when none does.
        Entity Owner = Entity::Null;
        /// @brief The pointer position in the owner's region, in region-local pixels.
        ///
        /// The WindowToViewport normalized coordinate scaled by the owner region's extent. Zero
        /// when Owner is null, and unused under capture (look reads raw delta, not position).
        vec2 LocalPosition = {};

        /// @brief The seat that owns the pointer this frame, or Entity::Null when none does.
        [[nodiscard]] Entity OwnerThisFrame() const { return Owner; }
    };

    /// @brief A Presented viewport's region paired with the seat its pointer input routes to.
    ///
    /// The unit the pointer selection resolves over: SelectPointerOwner scans these in order and
    /// the first region containing the point wins.
    struct PointerRegionSeat
    {
        /// @brief The viewport's window placement region, hit-tested against the pointer.
        Renderer::ViewportRegion Region;
        /// @brief The seat the region's pointer input routes to.
        Entity Viewer = Entity::Null;
    };

    /// @brief Resolves the free pointer to the first region-seat containing its window point.
    ///
    /// Scans @p regions in order (association order is the priority for any overlap; split-screen
    /// regions are non-overlapping), hit-testing @p pointerWindowPoint against each region exactly
    /// as Viewport::WindowToViewport does — containment, then a normalized [0,1] remap scaled to the
    /// region's extent. The first containing region yields its seat and the pointer's region-local
    /// position; no containing region (or an empty list) yields an empty routing (Owner ==
    /// Entity::Null). Pure and device-free: the router calls it over its live viewport regions, and
    /// it is the point→seat selection the unit test drives directly.
    /// @param regions            The region-seat pairs in priority order.
    /// @param pointerWindowPoint  The pointer position in window framebuffer pixels.
    /// @return The pointer's owner and region-local position, or an empty routing on no hit.
    [[nodiscard]] VE_API PointerRouting
    SelectPointerOwner(std::span<const PointerRegionSeat> regions, ivec2 pointerWindowPoint);

    /// @brief Routes window events by per-seat focus stacks, so each seat's input has one owner.
    ///
    /// Each frame the application drains the Window's event queue through Dispatch. The router
    /// holds one focus stack per seat, keyed by the seat's Viewer entity, whose top decides that
    /// seat's routing. The cursor seat — the single keyboard/mouse seat — is the one whose focus
    /// top gates the drained window events and drives the OS cursor capture: under UI focus an
    /// input event is offered to the consumer registry *and* folded into the Input snapshot (so
    /// the editor camera reads it); under Gameplay focus the event is folded into the Input
    /// snapshot only and **swallowed** — the consumers never see it, so input is exclusive to the
    /// running game. Window/system events (resize, close, focus) always reach the consumers
    /// regardless of any focus. Registered consumers are offered every UI-owned event in priority
    /// order until one accepts it.
    ///
    /// Gameplay focus pairs with the OS cursor capture: pushing it on the cursor seat hides+locks
    /// the cursor, popping it (the release chord, or window-focus loss) restores it. The release
    /// chord is Shift+Esc, checked here and not delivered to the game.
    ///
    /// A seat's focus stack decides where that seat's own devices route: split-screen seat A can
    /// hold UI focus (routing A's events to the consumers) while seat B stays in gameplay. A
    /// single-seat app is one stack — its behavior matches a single global stack exactly.
    class InputRouter
    {
    public:
        /// @brief Constructs the router over the borrowed services.
        /// @param window  Window whose cursor capture follows the cursor seat's focus; nullptr headless.
        /// @param input   The frame-coherent Input snapshot routed events fold into.
        InputRouter(Window* window, Input& input);

        /// @brief Registers a consumer at the tail of the priority-ordered consumer list.
        ///
        /// Each UI-owned event is offered to the registered consumers in registration order until
        /// one accepts it (the first registered has the highest priority). The dev/editor overlay
        /// registers first, so it is offered every UI-owned event ahead of any later consumer. The
        /// router borrows the consumer; the caller keeps it alive for the router's lifetime and
        /// registers it once.
        /// @param consumer  The consumer to append; must outlive the router.
        void RegisterConsumer(InputConsumer& consumer);

        /// @brief Sets the seat whose focus top gates window events and drives the cursor capture.
        ///
        /// There is one OS cursor and one keyboard/mouse seat, so the drained window events route by
        /// that seat's focus and the cursor capture derives from it. The app sets it to its
        /// keyboard/mouse seat's Viewer entity; the default (Entity::Null) is the implicit single
        /// seat of a keyboardless or one-seat app. Recomputes the cursor capture from the new seat's
        /// focus top.
        /// @param seat  The keyboard/mouse seat's Viewer entity, or Entity::Null for the implicit seat.
        void SetCursorSeat(Entity seat);

        /// @brief Returns the seat whose focus gates window events and drives the cursor capture.
        [[nodiscard]] Entity GetCursorSeat() const { return m_CursorSeat; }

        /// @brief Pushes a focus layer onto a seat's stack and returns its token.
        ///
        /// The token identifies this exact entry for a later PopFocus, so an interleaved scope pops
        /// its own entry wherever it sits. Pushing Gameplay onto the cursor seat captures the OS
        /// cursor; the cursor state is recomputed from the cursor seat's new top.
        /// @param seat   The seat whose stack the layer is pushed onto.
        /// @param focus  The layer to push.
        /// @return The token naming the pushed entry.
        FocusToken PushFocus(Entity seat, InputFocus focus);

        /// @brief Pushes a focus layer onto the cursor seat's stack and returns its token.
        ///
        /// The one-seat convenience over PushFocus(GetCursorSeat(), focus).
        /// @param focus  The layer to push.
        /// @return The token naming the pushed entry.
        FocusToken PushFocus(InputFocus focus) { return PushFocus(m_CursorSeat, focus); }

        /// @brief Pops the focus entry the token names, wherever it sits in its seat's stack.
        ///
        /// The entry is removed and its seat's owning top recomputed. A token that names no live
        /// entry — a mispaired or double pop — is a fatal assert, not a silent no-op. If the popped
        /// entry belonged to the cursor seat, the OS cursor capture is recomputed from its new top.
        /// @param token  The token a PushFocus returned; must name a live entry.
        void PopFocus(FocusToken token);

        /// @brief Pops the top focus entry of the cursor seat, restoring the one beneath.
        ///
        /// The one-seat convenience: pops the cursor seat's top (UI when it empties) and recomputes
        /// the cursor capture. Popping an already-empty cursor-seat stack is a no-op, never an
        /// underflow.
        void PopFocus();

        /// @brief Returns the focus layer owning a seat's input (UI when its stack is empty).
        /// @param seat  The seat whose focus top to read.
        [[nodiscard]] InputFocus GetFocus(Entity seat) const;

        /// @brief Returns the focus layer owning the cursor seat's input (UI when empty).
        [[nodiscard]] InputFocus GetFocus() const { return GetFocus(m_CursorSeat); }

        /// @brief Returns true if the running game owns a seat's input exclusively.
        /// @param seat  The seat to test.
        [[nodiscard]] bool IsGameplayFocused(Entity seat) const
        {
            return GetFocus(seat) == InputFocus::Gameplay;
        }

        /// @brief Returns true if the running game owns the cursor seat's input exclusively.
        [[nodiscard]] bool IsGameplayFocused() const
        {
            return GetFocus(m_CursorSeat) == InputFocus::Gameplay;
        }

        /// @brief Routes one drained window event to the consumers and/or the Input snapshot by focus.
        /// @param event  The event to route.
        void Dispatch(Event& event);

        /// @brief Associates a Presented viewport's region with the seat it feeds pointer input to.
        ///
        /// The app associates the viewport it renders a seat's camera into with that seat's Viewer
        /// entity, so a pointer over the viewport routes to the seat. The association is transient
        /// router state, keyed by viewport pointer; associating an already-associated viewport
        /// updates its seat. Association order is the hit-test priority for any overlap (regions are
        /// non-overlapping for split-screen), tracking the app's viewport registration order. An
        /// unassociated Presented viewport's region routes no pointer, so the app must associate a
        /// viewport in the same step it registers it, leaving no live region without a seat.
        /// @param viewport  The Presented viewport whose region owns pointer input for the seat.
        /// @param viewer    The seat entity the viewport's pointer input routes to.
        void AssociateViewportSeat(const Renderer::Viewport& viewport, Entity viewer);

        /// @brief Drops a viewport's seat association, so its region routes no pointer.
        ///
        /// The app calls this when it tears down or repurposes an associated viewport; a viewport
        /// that was never associated is ignored.
        /// @param viewport  The viewport to disassociate.
        void ClearViewportSeat(const Renderer::Viewport& viewport);

        /// @brief Resolves which seat the free pointer belongs to this frame.
        ///
        /// While the cursor is captured (gameplay focus) the region hit-test is skipped and the
        /// result names the single keyboard/mouse seat, whose SeatInputView reads look as raw delta;
        /// @p captureOwner supplies that seat (Entity::Null if there is none). When free, the
        /// pointer's window point is hit-tested against each associated Presented viewport's region
        /// in association order (WindowToViewport); the first containing region wins, and the result
        /// carries that seat and the pointer's region-local position. No containing region leaves the
        /// owner Entity::Null. Computed once per frame and read by every seat's view construction.
        /// @param pointerWindowPoint  The pointer position in window framebuffer pixels.
        /// @param captured            Whether the cursor is captured (gameplay focus).
        /// @param captureOwner        The single keyboard/mouse seat the pointer routes to under capture.
        /// @return The pointer's owner and region-local position for this frame.
        [[nodiscard]] PointerRouting ResolvePointer(ivec2 pointerWindowPoint, bool captured,
                                                    Entity captureOwner) const;

        /// @brief Resolves the Presented viewport that owns the pointer this frame, or null.
        ///
        /// The scene-scoping companion to ResolvePointer: the resolved routing applies only to the
        /// simulation whose scene this viewport presents. While captured the cursor seat's associated
        /// viewport (there is one OS cursor / one cursor seat); when free the first associated
        /// viewport whose region contains @p pointerWindowPoint, hit-tested exactly as ResolvePointer
        /// does. Null when none applies — no association for the cursor seat, or a free pointer over
        /// no associated region — leaving the caller to fall back (e.g. to the primary world).
        /// @param pointerWindowPoint  The pointer position in window framebuffer pixels.
        /// @param captured            Whether the cursor is captured (gameplay focus).
        /// @return The owning Presented viewport, or nullptr when none applies.
        [[nodiscard]] const Renderer::Viewport* ResolvePointerViewport(ivec2 pointerWindowPoint,
                                                                       bool captured) const;

    private:
        /// @brief Matches the OS cursor capture and the consumers to the cursor seat's focus top.
        ///
        /// Gameplay focus on the cursor seat captures the cursor and signals the consumers the
        /// cursor is captured (so a polling consumer can suspend its cursor poll); any other focus
        /// releases the cursor and clears the signal.
        void SyncCursorState();

        /// @brief The cursor seat's current focus owner (UI when its stack is empty).
        [[nodiscard]] InputFocus CursorFocus() const { return GetFocus(m_CursorSeat); }

        /// @brief Offers one UI-owned event to the consumers, stopping at the first that accepts it.
        /// @param event  The event to offer.
        void OfferConsumers(const Event& event);

        /// @brief One pushed focus entry: its identifying token and the layer it owns.
        struct FocusEntry
        {
            /// @brief The token PushFocus returned for this entry.
            FocusToken Token;
            /// @brief The focus layer this entry owns.
            InputFocus Focus = InputFocus::UI;
        };

        /// @brief Borrowed window; nullptr headless. Its cursor capture follows the cursor seat.
        Window* m_Window;
        /// @brief The Input snapshot routed key/mouse events fold into.
        Input& m_Input;
        /// @brief Consumers offered each UI-owned event, in priority (registration) order.
        vector<InputConsumer*> m_Consumers;
        /// @brief Per-seat focus stacks; a seat's back is its current owner, absent/empty is UI.
        unordered_map<Entity, vector<FocusEntry>> m_Stacks;
        /// @brief The seat whose focus gates window events and drives the cursor capture.
        Entity m_CursorSeat = Entity::Null;
        /// @brief Monotonic source of focus-token identities; never reuses a value, 0 stays invalid.
        u64 m_NextToken = 1;

        /// @brief A Presented viewport paired with the seat its region routes pointer input to.
        struct ViewportAssociation
        {
            /// @brief The associated Presented viewport, queried live for its region each resolve.
            const Renderer::Viewport* Viewport = nullptr;
            /// @brief The seat entity the viewport's pointer input routes to.
            Entity Viewer = Entity::Null;
        };
        /// @brief Viewport→seat associations in registration order (the hit-test priority order).
        vector<ViewportAssociation> m_Associations;
    };
}
