#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class Window;
    class Input;
    class ImGuiLayer;
    class Event;
}

namespace Veng::Renderer
{
    class Viewport;
}

namespace Veng
{
    /// @brief The layer that owns input this frame.
    enum class InputFocus
    {
        /// @brief Editor/HUD UI owns input: events reach ImGui and the Input snapshot both.
        UI,
        /// @brief The running game owns input exclusively: events reach the Input snapshot only.
        Gameplay,
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

    /// @brief Routes window events to consumers by a focus stack, so input has a single owner.
    ///
    /// Each frame the application drains the Window's event queue through Dispatch. The router
    /// holds a focus stack whose top decides routing: under UI focus an input event is
    /// forwarded to ImGui *and* folded into the Input snapshot (so the editor camera reads
    /// it); under Gameplay focus the event is folded into the Input snapshot only and
    /// **swallowed** — ImGui never sees it, so input is exclusive to the running game.
    /// Window/system events (resize, close, focus) always reach ImGui regardless of the stack.
    ///
    /// Gameplay focus pairs with the OS cursor capture: pushing it hides+locks the cursor,
    /// popping it (the release chord, or window-focus loss) restores it. The release chord is
    /// Shift+Esc, checked here and not delivered to the game.
    class InputRouter
    {
    public:
        /// @brief Constructs the router over the borrowed services.
        /// @param window  Window whose cursor capture follows gameplay focus; nullptr headless.
        /// @param input   The frame-coherent Input snapshot routed events fold into.
        /// @param imgui   The ImGui layer events forward to under UI focus; nullptr if UI-free.
        InputRouter(Window* window, Input& input, ImGuiLayer* imgui);

        /// @brief Pushes a focus layer; the top of the stack owns input.
        ///
        /// Pushing Gameplay captures the OS cursor; the cursor state is recomputed from the
        /// new top.
        /// @param focus  The layer to push.
        void PushFocus(InputFocus focus);

        /// @brief Pops the top focus layer, restoring the one beneath (UI when the stack empties).
        ///
        /// Recomputes the OS cursor capture from the new top, so leaving Gameplay frees the cursor.
        void PopFocus();

        /// @brief Returns the focus layer currently owning input (UI when the stack is empty).
        [[nodiscard]] InputFocus GetFocus() const;

        /// @brief Returns true if the running game currently owns input exclusively.
        [[nodiscard]] bool IsGameplayFocused() const { return GetFocus() == InputFocus::Gameplay; }

        /// @brief Routes one drained window event to ImGui and/or the Input snapshot by focus.
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

    private:
        /// @brief Matches the OS cursor capture and ImGui mouse handling to the current focus top.
        ///
        /// Gameplay focus captures the cursor and disables ImGui's mouse (so its NewFrame cursor
        /// poll cannot drift hover); any other focus releases the cursor and re-enables it.
        void SyncFocusState();

        /// @brief Borrowed window; nullptr headless. Its cursor capture follows gameplay focus.
        Window* m_Window;
        /// @brief The Input snapshot routed key/mouse events fold into.
        Input& m_Input;
        /// @brief Borrowed ImGui layer; nullptr when the app is UI-free.
        ImGuiLayer* m_ImGui;
        /// @brief Focus layers above the implicit UI base; the back is the current owner.
        vector<InputFocus> m_Stack;

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
