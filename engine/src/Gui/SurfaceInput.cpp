#include <Veng/Gui/SurfaceInput.h>

#include <Veng/Event.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <limits>

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
        // (SHIFT/CONTROL/ALT/SUPER), the same field the pointer and ImGui sinks read.
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

        // One wheel notch, in document points — the screen-space consumer's constant, so a panel
        // answers a flick the same whether it is composited as an overlay or mapped onto a mesh.
        constexpr f32 WheelNotchPoints = 56.0f;

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

        // Maps an editing key to its TextEditAction, or nullopt when the key edits no text. These
        // carry no character, so they never reach a field through the typed-text route and need this
        // key-press mapping to reach it at all.
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

        // Builds a document-local pointer event of the given kind and dispatches it into the
        // document. A display-only document (SetInteractive not opened) consumes nothing.
        bool DispatchDocumentPointer(GuiSurface& surface, vec2 documentPoint, PointerEventKind kind,
                                     PointerButton button)
        {
            Document* const document = surface.GetDocument();
            if (document == nullptr)
            {
                return false;
            }
            PointerEvent event;
            event.Kind = kind;
            event.Button = button;
            event.Position = documentPoint;
            return document->DispatchPointer(event);
        }
    }

    optional<SurfaceRayHit> IntersectSurface(const Ray& ray, const SurfacePlacement& placement,
                                             vec2 documentExtent)
    {
        // The panel is a rectangle in the placement's local XY plane. Its world center and its two
        // world-space half-extent vectors span the rectangle; their cross product is the front normal.
        const vec3 center = vec3(placement.Transform * vec4(0.0f, 0.0f, 0.0f, 1.0f));
        const vec3 halfX =
            vec3(placement.Transform * vec4(placement.Size.x * 0.5f, 0.0f, 0.0f, 0.0f));
        const vec3 halfY =
            vec3(placement.Transform * vec4(0.0f, placement.Size.y * 0.5f, 0.0f, 0.0f));
        const vec3 normal = glm::cross(halfX, halfY);

        // A ray parallel to the panel never resolves a point; guard the division.
        const f32 denom = glm::dot(ray.Direction, normal);
        if (std::abs(denom) < std::numeric_limits<f32>::epsilon())
        {
            return std::nullopt;
        }

        // Ray-plane intersection; a hit behind the ray origin is not under the pointer.
        const f32 t = glm::dot(center - ray.Origin, normal) / denom;
        if (t < 0.0f)
        {
            return std::nullopt;
        }

        // Project the hit onto the rectangle's axes; |proj| > 1 is outside the panel. Each half-extent
        // vector already carries half the panel width/height, so dividing by its squared length maps a
        // corner to ±1.
        const vec3 hit = ray.At(t);
        const vec3 delta = hit - center;
        const f32 projX = glm::dot(delta, halfX) / glm::dot(halfX, halfX);
        const f32 projY = glm::dot(delta, halfY) / glm::dot(halfY, halfY);
        if (std::abs(projX) > 1.0f || std::abs(projY) > 1.0f)
        {
            return std::nullopt;
        }

        // Map [-1, 1] to the document's coordinate space: local +X is document +X (right), local +Y is
        // document -Y, since the document lays out y-down from its top-left origin.
        const f32 u = (projX + 1.0f) * 0.5f;
        const f32 v = (1.0f - projY) * 0.5f;
        return SurfaceRayHit{.DocumentPoint = vec2(u * documentExtent.x, v * documentExtent.y),
                             .Distance = t};
    }

    optional<SurfaceRayHit> IntersectSurface(const Ray& ray, const SurfacePlacement& placement,
                                             const GuiSurface& surface)
    {
        return IntersectSurface(ray, placement, vec2(surface.Resolution));
    }

    optional<vec2> RayToDocumentPoint(const Ray& ray, const SurfacePlacement& placement,
                                      vec2 documentExtent)
    {
        const optional<SurfaceRayHit> hit = IntersectSurface(ray, placement, documentExtent);
        if (!hit)
        {
            return std::nullopt;
        }
        return hit->DocumentPoint;
    }

    optional<vec2> RouteSurfacePointer(GuiSurface& surface, const SurfacePlacement& placement,
                                       const Ray& ray, PointerEventKind kind, PointerButton button)
    {
        // The seat gate: with no seat the surface is display-only and the adapter is never consulted.
        if (surface.Seat == Entity::Null)
        {
            return std::nullopt;
        }

        const optional<SurfaceRayHit> hit =
            IntersectSurface(ray, placement, vec2(surface.Resolution));
        if (!hit)
        {
            return std::nullopt;
        }

        DispatchDocumentPointer(surface, hit->DocumentPoint, kind, button);
        return hit->DocumentPoint;
    }

    SurfaceInputConsumer::Registration::Registration(Registration&& other) noexcept
        : m_Consumer(other.m_Consumer), m_Id(other.m_Id)
    {
        other.m_Consumer = nullptr;
        other.m_Id = 0;
    }

    SurfaceInputConsumer::Registration&
    SurfaceInputConsumer::Registration::operator=(Registration&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_Consumer = other.m_Consumer;
            m_Id = other.m_Id;
            other.m_Consumer = nullptr;
            other.m_Id = 0;
        }
        return *this;
    }

    SurfaceInputConsumer::Registration::~Registration()
    {
        Reset();
    }

    void SurfaceInputConsumer::Registration::Reset()
    {
        if (m_Consumer != nullptr)
        {
            m_Consumer->Unregister(m_Id);
            m_Consumer = nullptr;
            m_Id = 0;
        }
    }

    SurfaceInputConsumer::SurfaceInputConsumer(InputRouter& router) : m_Router(router) {}

    SurfaceInputConsumer::Registration
    SurfaceInputConsumer::Register(GuiSurface& surface, function<SurfacePlacement()> placement,
                                   function<optional<Ray>()> ray)
    {
        const RegistrationId id = m_NextId++;
        m_Entries.emplace_back(Entry{.Id = id,
                                     .Surface = &surface,
                                     .Placement = std::move(placement),
                                     .Ray = std::move(ray)});
        return Registration(*this, id);
    }

    void SurfaceInputConsumer::Unregister(RegistrationId id)
    {
        const auto it = std::ranges::find(m_Entries, id, &Entry::Id);
        if (it != m_Entries.end())
        {
            m_Entries.erase(it);
        }
    }

    bool SurfaceInputConsumer::IsParticipating(const Entry& entry) const
    {
        // The seat gate: an unseated panel is display-only, so the adapter is never consulted. Route
        // only while the seat's focus top is UI (the SeatFocusScope's takeover) and the document has
        // been opened interactive — the same gate the screen-space consumer applies.
        const Entity seat = entry.Surface->Seat;
        if (seat == Entity::Null || m_Router.GetFocus(seat) != InputFocus::UI)
        {
            return false;
        }
        const Document* const document = entry.Surface->GetDocument();
        return document != nullptr && document->IsInteractive();
    }

    SurfaceInputConsumer::Entry* SurfaceInputConsumer::HitTestNearest(SurfaceRayHit& hit)
    {
        Entry* nearest = nullptr;
        SurfaceRayHit nearestHit{};
        for (Entry& entry : m_Entries)
        {
            if (!IsParticipating(entry))
            {
                continue;
            }
            const optional<Ray> ray = entry.Ray();
            if (!ray)
            {
                continue;
            }
            const optional<SurfaceRayHit> entryHit =
                IntersectSurface(*ray, entry.Placement(), vec2(entry.Surface->Resolution));
            if (!entryHit)
            {
                continue;
            }
            if (nearest == nullptr || entryHit->Distance < nearestHit.Distance)
            {
                nearest = &entry;
                nearestHit = *entryHit;
            }
        }
        if (nearest != nullptr)
        {
            hit = nearestHit;
        }
        return nearest;
    }

    bool SurfaceInputConsumer::ForwardEvent(const Event& event)
    {
        const EventType type = event.GetEventType();

        // A pointer or wheel event names a point in the world, so it routes by the nearest ray hit.
        if (type == EventType::MouseMoved || type == EventType::MouseButtonPressed ||
            type == EventType::MouseButtonReleased || type == EventType::MouseScrolled)
        {
            SurfaceRayHit hit{};
            Entry* const nearest = HitTestNearest(hit);
            if (nearest == nullptr)
            {
                return false;
            }
            Document* const document = nearest->Surface->GetDocument();

            // The wheel is not a pointer transition: it names a scrollable box at the hit point and
            // takes its own dispatch. The wheel's y is positive away from the user where a scroll
            // offset grows downward, so the sign flips here — once, at the seam.
            if (type == EventType::MouseScrolled)
            {
                const vec2 turn = static_cast<const MouseScrolledEvent&>(event).GetOffset();
                const vec2 delta = vec2(turn.x, -turn.y) * WheelNotchPoints;
                return document->DispatchScroll(hit.DocumentPoint, delta);
            }

            PointerEventKind kind = PointerEventKind::Move;
            PointerButton button = PointerButton::Primary;
            if (type == EventType::MouseButtonPressed)
            {
                kind = PointerEventKind::Down;
                button =
                    ToPointerButton(static_cast<const MouseButtonPressedEvent&>(event).GetButton());
            }
            else if (type == EventType::MouseButtonReleased)
            {
                kind = PointerEventKind::Up;
                button = ToPointerButton(
                    static_cast<const MouseButtonReleasedEvent&>(event).GetButton());
            }
            return DispatchDocumentPointer(*nearest->Surface, hit.DocumentPoint, kind, button);
        }

        // A key carries no world point, so it routes to the participating panels in registration
        // order, stopping at the first that consumes — the focused text field or focus navigation.
        // An editing key is offered to the field first (a caret step or a codepoint deletion); a
        // navigation key then drives focus. A platform auto-repeat takes the editing route and only
        // it — focus is a discrete choice of element and must not skate through the focus order.
        if (type == EventType::KeyPressed || type == EventType::KeyRepeat)
        {
            const bool repeat = type == EventType::KeyRepeat;
            const Key code = repeat ? static_cast<const KeyRepeatEvent&>(event).GetKey()
                                    : static_cast<const KeyPressedEvent&>(event).GetKey();
            if (const optional<TextEditAction> edit = ToTextEditAction(code))
            {
                for (const Entry& entry : m_Entries)
                {
                    if (IsParticipating(entry) &&
                        entry.Surface->GetDocument()->DispatchTextEdit(*edit))
                    {
                        return true;
                    }
                }
            }
            if (repeat)
            {
                return false;
            }
            const InputModifiers modifiers =
                ToInputModifiers(static_cast<const KeyPressedEvent&>(event).GetMods());
            const optional<NavAction> action =
                ToNavAction(code, HasModifier(modifiers, InputModifiers::Shift));
            if (!action)
            {
                return false;
            }
            for (const Entry& entry : m_Entries)
            {
                if (IsParticipating(entry) &&
                    entry.Surface->GetDocument()->Navigate(*action, modifiers))
                {
                    return true;
                }
            }
            return false;
        }

        // A typed character reaches the focused element's text input.
        if (type == EventType::KeyTyped)
        {
            const u32 codepoint = static_cast<const KeyTypedEvent&>(event).GetCodepoint();
            for (const Entry& entry : m_Entries)
            {
                if (IsParticipating(entry) && entry.Surface->GetDocument()->DispatchText(codepoint))
                {
                    return true;
                }
            }
            return false;
        }

        return false;
    }
}
