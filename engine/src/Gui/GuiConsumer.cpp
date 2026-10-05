#include <Veng/Gui/GuiConsumer.h>

#include <Veng/Event.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
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

        // Maps a GLFW modifier bitfield to the Gui modifier vocabulary. The bits are GLFW's own
        // (SHIFT/CONTROL/ALT/SUPER), the same field the ImGui sink reads.
        InputModifiers ToInputModifiers(i32 mods)
        {
            InputModifiers result = InputModifiers::None;
            if ((mods & 0x0001) != 0)
            {
                result = result | InputModifiers::Shift;
            }
            if ((mods & 0x0002) != 0)
            {
                result = result | InputModifiers::Control;
            }
            if ((mods & 0x0004) != 0)
            {
                result = result | InputModifiers::Alt;
            }
            if ((mods & 0x0008) != 0)
            {
                result = result | InputModifiers::Meta;
            }
            return result;
        }

        // One wheel notch, in document points. A fixed distance rather than a fraction of the box
        // under the pointer: a short list and a tall one should answer the same flick the same way,
        // where a fraction makes the tall one race. A trackpad reports fractional notches, so a
        // precise gesture scales down through the same constant.
        constexpr f32 WheelNotchPoints = 56.0f;

        // Maps a navigation role to its NavAction, or nullopt for a role that navigates nothing.
        optional<NavAction> ToNavAction(const ActionRole role)
        {
            switch (role)
            {
            case ActionRole::NavigateUp:
                return NavAction::MoveUp;
            case ActionRole::NavigateDown:
                return NavAction::MoveDown;
            case ActionRole::NavigateLeft:
                return NavAction::MoveLeft;
            case ActionRole::NavigateRight:
                return NavAction::MoveRight;
            case ActionRole::NavigateNext:
                return NavAction::Next;
            case ActionRole::NavigatePrevious:
                return NavAction::Previous;
            case ActionRole::Confirm:
                return NavAction::Confirm;
            case ActionRole::Cancel:
                return NavAction::Cancel;
            case ActionRole::None:
                return std::nullopt;
            }
            return std::nullopt;
        }

        // Maps a seat's held modifier keys to the Gui modifier vocabulary.
        InputModifiers ToInputModifiers(const ModifierKeys& keys)
        {
            InputModifiers result = InputModifiers::None;
            if (keys.Shift)
            {
                result = result | InputModifiers::Shift;
            }
            if (keys.Control)
            {
                result = result | InputModifiers::Control;
            }
            if (keys.Alt)
            {
                result = result | InputModifiers::Alt;
            }
            if (keys.Super)
            {
                result = result | InputModifiers::Meta;
            }
            return result;
        }

        // Maps an editing key to its TextEditAction, or nullopt when the key edits no text. These
        // are the keys that carry no character, so they never reach a field through the typed-text
        // route and need this key-press mapping to reach it at all.
        optional<TextEditAction> ToTextEditAction(Key key)
        {
            switch (key)
            {
            case Key::Backspace:
                return TextEditAction::DeleteBackward;
            case Key::Delete:
                return TextEditAction::DeleteForward;
            case Key::Left:
                return TextEditAction::CaretLeft;
            case Key::Right:
                return TextEditAction::CaretRight;
            case Key::Home:
                return TextEditAction::CaretHome;
            case Key::End:
                return TextEditAction::CaretEnd;
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

        // Pointer events route to the viewport under the pointer, into its routable documents
        // top-first; the first document that consumes stops the fall-through. The drive list is
        // walked in reverse because registration order is composite order — when regions overlap
        // (a fullscreen game screen registered over the primary viewport), the last-registered
        // viewport is drawn on top, so it owns the pointer.
        if (type == EventType::MouseMoved || type == EventType::MouseButtonPressed ||
            type == EventType::MouseButtonReleased || type == EventType::MouseScrolled)
        {
            const ivec2 pixels = PointerPixels();

            for (auto viewportIt = m_Viewports.rbegin(); viewportIt != m_Viewports.rend();
                 ++viewportIt)
            {
                Renderer::Viewport* const viewport = *viewportIt;
                // An offscreen viewport renders into a texture something else displays, so it is
                // never what a window pointer is over: its region is a render extent, not a place
                // on screen, and a region left at the window origin would otherwise swallow every
                // event over that corner.
                if (viewport->GetRole() == Renderer::ViewportRole::Offscreen)
                {
                    continue;
                }
                const optional<vec2> normalized = viewport->WindowToViewport(pixels);
                if (!normalized)
                {
                    continue;
                }

                // A viewport bound to a seat routes pointer only for that seat, and only while its
                // seat's focus top is UI; the all-devices seat (Entity::Null) always routes.
                const SeatRef seat = viewport->GetSeat();
                if (!seat.IsImplicit() && m_Router.GetFocus(seat) != InputFocus::UI)
                {
                    continue;
                }

                // Document space is logical points under the viewport's UI scale (the document
                // solved at extent / scale), so the physical-pixel point divides by it.
                const vec2 docPoint =
                    *normalized * vec2(viewport->GetRegion().Extent) / viewport->GetUiScale();

                // A wheel turn is not a pointer event: it names a scrollable box rather than an
                // element, so it takes the same viewport and document walk and its own dispatch.
                // The wheel's y is positive away from the user, where a scroll offset grows
                // downward, so the sign flips here — once, at the seam — rather than in every
                // scrollable.
                if (type == EventType::MouseScrolled)
                {
                    const vec2 turn = static_cast<const MouseScrolledEvent&>(event).GetOffset();
                    const vec2 delta = vec2(turn.x, -turn.y) * WheelNotchPoints;
                    const std::span<Gui::Document* const> scrolled = viewport->GetInputDocuments();
                    for (auto it = scrolled.rbegin(); it != scrolled.rend(); ++it)
                    {
                        Gui::Document* document = *it;
                        if (document->IsInteractive() && document->DispatchScroll(docPoint, delta))
                        {
                            return true;
                        }
                    }
                    break;
                }

                PointerEvent pointer;
                pointer.Position = docPoint;
                if (type == EventType::MouseMoved)
                {
                    pointer.Kind = PointerEventKind::Move;
                }
                else if (type == EventType::MouseButtonPressed)
                {
                    const auto& pressed = static_cast<const MouseButtonPressedEvent&>(event);
                    pointer.Kind = PointerEventKind::Down;
                    pointer.Button = ToPointerButton(pressed.GetButton());
                    pointer.Modifiers = ToInputModifiers(pressed.GetMods());
                }
                else
                {
                    const auto& released = static_cast<const MouseButtonReleasedEvent&>(event);
                    pointer.Kind = PointerEventKind::Up;
                    pointer.Button = ToPointerButton(released.GetButton());
                    pointer.Modifiers = ToInputModifiers(released.GetMods());
                }

                const std::span<Gui::Document* const> documents = viewport->GetInputDocuments();
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

        // A key reaches the documents only as text editing; navigation arrives as roles
        // (ForwardRole). A focused text field owns the editing keys — Backspace and Delete edit
        // around its caret, Left/Right/Home/End move it — and accepting one claims the key, so the
        // role resolution reads it as up and the field keeps it until release. A platform auto-repeat
        // takes the same route, so a held key walks the caret or erases the way any text field does.
        if (type == EventType::KeyPressed || type == EventType::KeyRepeat)
        {
            const Key code = type == EventType::KeyRepeat
                                 ? static_cast<const KeyRepeatEvent&>(event).GetKey()
                                 : static_cast<const KeyPressedEvent&>(event).GetKey();
            const optional<TextEditAction> edit = ToTextEditAction(code);
            if (!edit)
            {
                return false;
            }
            for (Renderer::Viewport* viewport : m_Viewports)
            {
                const std::span<Gui::Document* const> documents = viewport->GetInputDocuments();
                for (auto it = documents.rbegin(); it != documents.rend(); ++it)
                {
                    Gui::Document* document = *it;
                    if (document->IsInteractive() && document->DispatchTextEdit(*edit))
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
                const std::span<Gui::Document* const> documents = viewport->GetInputDocuments();
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

    bool GuiConsumer::ForwardRole(const RoleEvent& event)
    {
        const optional<NavAction> action = ToNavAction(event.Role);
        if (!action)
        {
            return false;
        }
        const InputModifiers modifiers = ToInputModifiers(event.Modifiers);

        // The seat's own viewports, topmost first, and in each its routable documents topmost first:
        // the press stops at the first document that takes it. The implicit seat reaches only the
        // viewports bound to no seat, so a seated document is driven by its own seat alone.
        for (auto viewportIt = m_Viewports.rbegin(); viewportIt != m_Viewports.rend(); ++viewportIt)
        {
            Renderer::Viewport* const viewport = *viewportIt;
            const SeatRef seat = viewport->GetSeat();
            const bool owns = event.Seat.IsImplicit() ? seat.IsImplicit() : seat == event.Seat;
            if (!owns)
            {
                continue;
            }
            const std::span<Gui::Document* const> documents = viewport->GetInputDocuments();
            for (auto it = documents.rbegin(); it != documents.rend(); ++it)
            {
                Gui::Document* document = *it;
                if (document->IsInteractive() && document->Navigate(*action, modifiers))
                {
                    return true;
                }
            }
        }
        return false;
    }
}
