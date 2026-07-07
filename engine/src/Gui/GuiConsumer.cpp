#include <Veng/Gui/GuiConsumer.h>

#include <Veng/Event.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Window.h>

#include <algorithm>

namespace Veng::Gui
{
    namespace
    {
        // Maps an engine mouse button to the Gui pointer button vocabulary.
        PointerButton ToPointerButton(MouseButton button)
        {
            switch (button)
            {
            case MouseButton::Left:
                return PointerButton::Primary;
            case MouseButton::Right:
                return PointerButton::Secondary;
            case MouseButton::Middle:
                return PointerButton::Middle;
            }
            return PointerButton::Primary;
        }

        // Maps a navigation key to its NavAction, or nullopt when the key is not a navigation key.
        optional<NavAction> ToNavAction(Key key, bool shift)
        {
            switch (key)
            {
            case Key::Up:
                return NavAction::MoveUp;
            case Key::Down:
                return NavAction::MoveDown;
            case Key::Left:
                return NavAction::MoveLeft;
            case Key::Right:
                return NavAction::MoveRight;
            case Key::Tab:
                return shift ? NavAction::Previous : NavAction::Next;
            case Key::Enter:
            case Key::Space:
                return NavAction::Confirm;
            case Key::Escape:
                return NavAction::Cancel;
            default:
                return std::nullopt;
            }
        }
    }

    GuiConsumer::GuiConsumer(InputRouter& router, Input& input, Window* window,
                             const std::vector<Renderer::Viewport*>& viewports)
        : m_Router(router), m_Input(input), m_Window(window), m_Viewports(viewports)
    {
    }

    ivec2 GuiConsumer::PointerPixels() const
    {
        // The snapshot reports the cursor in logical points; a viewport region is framebuffer
        // pixels, so scale by the window's content scale on a HiDPI display.
        const vec2 point = m_Input.GetMousePosition();
        const vec2 scale = m_Window != nullptr ? m_Window->GetContentScale() : vec2(1.0f);
        return ivec2(point * scale);
    }

    bool GuiConsumer::ForwardEvent(const Event& event)
    {
        const EventType type = event.GetEventType();

        // Pointer events route to the viewport under the pointer, into its interactive documents
        // top-first; the first document that consumes stops the fall-through. The drive list is
        // walked in reverse because registration order is composite order — when regions overlap
        // (a fullscreen game screen registered over the primary viewport), the last-registered
        // viewport is drawn on top, so it owns the pointer.
        if (type == EventType::MouseMoved || type == EventType::MouseButtonPressed ||
            type == EventType::MouseButtonReleased)
        {
            const ivec2 pixels = PointerPixels();

            for (auto viewportIt = m_Viewports.rbegin(); viewportIt != m_Viewports.rend();
                 ++viewportIt)
            {
                Renderer::Viewport* const viewport = *viewportIt;
                const optional<vec2> normalized = viewport->WindowToViewport(pixels);
                if (!normalized)
                {
                    continue;
                }

                // A viewport bound to a seat routes pointer only for that seat, and only while its
                // seat's focus top is UI; the all-devices seat (Entity::Null) always routes.
                const Entity seat = viewport->GetSeat();
                if (seat != Entity::Null && m_Router.GetFocus(seat) != InputFocus::UI)
                {
                    continue;
                }

                const vec2 docPoint = *normalized * vec2(viewport->GetRegion().Extent);

                PointerEvent pointer;
                pointer.Position = docPoint;
                if (type == EventType::MouseMoved)
                {
                    pointer.Kind = PointerEventKind::Move;
                }
                else if (type == EventType::MouseButtonPressed)
                {
                    pointer.Kind = PointerEventKind::Down;
                    pointer.Button = ToPointerButton(
                        static_cast<const MouseButtonPressedEvent&>(event).GetButton());
                }
                else
                {
                    pointer.Kind = PointerEventKind::Up;
                    pointer.Button = ToPointerButton(
                        static_cast<const MouseButtonReleasedEvent&>(event).GetButton());
                }

                const std::span<Gui::Document* const> documents = viewport->GetAttachedDocuments();
                for (auto it = documents.rbegin(); it != documents.rend(); ++it)
                {
                    Gui::Document* document = *it;
                    if (!document->IsInteractive())
                    {
                        continue;
                    }
                    if (document->DispatchPointer(pointer))
                    {
                        return true;
                    }
                }
                // The pointer belongs to exactly one viewport region; no need to scan the rest.
                break;
            }
            return false;
        }

        // A navigation key drives focus in the interactive documents of the cursor seat's
        // viewports; text input reaches the focused element's onText handler.
        if (type == EventType::KeyPressed)
        {
            const auto& key = static_cast<const KeyPressedEvent&>(event);
            const bool shift = (key.GetMods() & 0x0001) != 0;
            const optional<NavAction> action = ToNavAction(key.GetKey(), shift);
            if (!action)
            {
                return false;
            }
            for (Renderer::Viewport* viewport : m_Viewports)
            {
                const std::span<Gui::Document* const> documents = viewport->GetAttachedDocuments();
                for (auto it = documents.rbegin(); it != documents.rend(); ++it)
                {
                    Gui::Document* document = *it;
                    if (document->IsInteractive() && document->Navigate(*action))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        if (type == EventType::KeyTyped)
        {
            const auto& typed = static_cast<const KeyTypedEvent&>(event);
            for (Renderer::Viewport* viewport : m_Viewports)
            {
                const std::span<Gui::Document* const> documents = viewport->GetAttachedDocuments();
                for (auto it = documents.rbegin(); it != documents.rend(); ++it)
                {
                    Gui::Document* document = *it;
                    if (document->IsInteractive() && document->DispatchText(typed.GetCodepoint()))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        return false;
    }
}
