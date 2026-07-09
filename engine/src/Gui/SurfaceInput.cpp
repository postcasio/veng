#include <Veng/Gui/SurfaceInput.h>

#include <Veng/Event.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Surface.h>
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

    bool SurfaceInputConsumer::ForwardEvent(const Event& event)
    {
        const EventType type = event.GetEventType();
        if (type != EventType::MouseMoved && type != EventType::MouseButtonPressed &&
            type != EventType::MouseButtonReleased)
        {
            return false;
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
            button =
                ToPointerButton(static_cast<const MouseButtonReleasedEvent&>(event).GetButton());
        }

        // Across every participating panel whose seat holds UI focus, the nearest ray hit wins — the
        // only world-space arbitration when overlapping panels lie under one pointer ray.
        Entry* nearest = nullptr;
        SurfaceRayHit nearestHit{};
        for (Entry& entry : m_Entries)
        {
            // The seat gate: an unseated panel is display-only, so the adapter is never consulted.
            // Route only while the seat's focus top is UI (the SeatFocusScope's takeover) and the
            // document has been opened interactive — the same gate the screen-space consumer applies.
            const Entity seat = entry.Surface->Seat;
            if (seat == Entity::Null || m_Router.GetFocus(seat) != InputFocus::UI)
            {
                continue;
            }
            const Document* const document = entry.Surface->GetDocument();
            if (document == nullptr || !document->IsInteractive())
            {
                continue;
            }
            const optional<Ray> ray = entry.Ray();
            if (!ray)
            {
                continue;
            }
            const optional<SurfaceRayHit> hit =
                IntersectSurface(*ray, entry.Placement(), vec2(entry.Surface->Resolution));
            if (!hit)
            {
                continue;
            }
            if (nearest == nullptr || hit->Distance < nearestHit.Distance)
            {
                nearest = &entry;
                nearestHit = *hit;
            }
        }

        if (nearest == nullptr)
        {
            return false;
        }
        return DispatchDocumentPointer(*nearest->Surface, nearestHit.DocumentPoint, kind, button);
    }
}
