#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Window;
    class Input;
    class ImGuiLayer;
    class Event;

    /// @brief The layer that owns input this frame.
    enum class InputFocus
    {
        /// @brief Editor/HUD UI owns input: events reach ImGui and the Input snapshot both.
        UI,
        /// @brief The running game owns input exclusively: events reach the Input snapshot only.
        Gameplay,
    };

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
    };
}
