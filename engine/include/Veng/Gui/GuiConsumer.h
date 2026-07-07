#pragma once

#include <Veng/Veng.h>
#include <Veng/Input/InputConsumer.h>

#include <vector>

namespace Veng
{
    class Input;
    class InputRouter;
    class Window;
}

namespace Veng::Renderer
{
    class Viewport;
}

namespace Veng::Gui
{
    /// @brief The router consumer that routes UI-owned input into attached documents.
    ///
    /// Registers as the **second** consumer in the router's registry, behind the dev/editor ImGui
    /// overlay: an event ImGui consumes never reaches it, and a pointer/key event ImGui passes is
    /// offered here. It converts the routed engine events into Gui pointer/key/text dispatch against
    /// the documents attached to the viewports it tracks — walking each pointer's viewport's layer
    /// stack **topmost-first**, skipping display-only documents, and stopping at the first document
    /// that consumes the event (an unconsumed event falls through to a later consumer).
    ///
    /// Documents are **display-only by default**: this consumer routes into a document only while it
    /// is interactive (Document::SetInteractive), which the game flips on while it holds the
    /// document's seat through a SeatFocusScope. So a passive HUD never captures input, and in
    /// split-screen a seat's menu owns only that seat's devices — the consumer routes a pointer to a
    /// viewport only when the viewport's inherited seat is the one the pointer belongs to this frame.
    ///
    /// The consumer borrows the router, the input snapshot, the window, and the viewport drive-list
    /// it walks (the Application-owned, registration-ordered list that self-cleans on a viewport's
    /// destruction). All four must outlive the consumer.
    class GuiConsumer final : public InputConsumer
    {
    public:
        /// @brief Constructs the consumer over the borrowed router, snapshot, window, and viewport list.
        /// @param router     The router whose focus/pointer routing scopes each event; must outlive this.
        /// @param input      The frame-coherent snapshot the pointer position and modifiers read from.
        /// @param window     The window whose content scale maps logical points to framebuffer pixels;
        ///                   nullptr headless (points are treated as pixels).
        /// @param viewports  The registration-ordered viewport list to route into; a hosted
        ///                   document is found by walking each viewport's layer stack. Must outlive
        ///                   this — the Application drive-list is the intended argument, since it
        ///                   self-cleans when a viewport is destroyed.
        GuiConsumer(InputRouter& router, Input& input, Window* window,
                    const std::vector<Renderer::Viewport*>& viewports);

        /// @brief Offers one UI-owned event to the attached documents; see the class brief.
        /// @param event  The event to route.
        /// @return True when a document consumed the event, stopping the fall-through.
        bool ForwardEvent(const Event& event) override;

    private:
        /// @brief The pointer position in window framebuffer pixels, from the snapshot × content scale.
        [[nodiscard]] ivec2 PointerPixels() const;

        /// @brief The router whose per-seat focus and pointer routing scope every routed event.
        InputRouter& m_Router;
        /// @brief The input snapshot the pointer position and button/key state read from.
        Input& m_Input;
        /// @brief The window whose content scale maps logical points to framebuffer pixels; nullable.
        Window* m_Window;
        /// @brief The registration-ordered viewport list the consumer walks for hosted documents.
        const std::vector<Renderer::Viewport*>& m_Viewports;
    };
}
