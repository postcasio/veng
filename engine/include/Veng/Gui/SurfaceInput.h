#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Input/InputConsumer.h>
#include <Veng/Math/Ray.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class InputRouter;
    struct GuiSurface;
}

namespace Veng::Gui
{
    /// @brief The world-space placement of a panel a document is mapped onto.
    ///
    /// The panel is a rectangle in the placement's local XY plane, centered at the origin, spanning
    /// Size units and facing local +Z — the front face a document renders onto. Transform maps that
    /// local rectangle into world space, so it carries the panel's world position, orientation, and
    /// scale. This is the default quad surface exactly; a non-quad mesh reduces to the front-face
    /// rectangle the document is laid out across, so the ray→coordinate mapping depends only on the
    /// rectangle and the document extent, never on how the panel is shaded.
    struct SurfacePlacement
    {
        /// @brief Panel-local to world transform (position, orientation, scale of the rectangle).
        mat4 Transform{1.0f};
        /// @brief Local-space width and height of the front-face rectangle, centered at the origin.
        vec2 Size{1.0f, 1.0f};
    };

    /// @brief A ray's hit against a panel: the document-local point and the world hit distance.
    struct SurfaceRayHit
    {
        /// @brief The hit mapped into the document's own coordinate space (the Solve extent).
        vec2 DocumentPoint{0.0f};
        /// @brief The ray parameter t (world units along the ray) at the hit — the nearest-hit key.
        f32 Distance = 0.0f;
    };

    /// @brief Intersects a world ray with a panel and maps the hit to a document-local point.
    ///
    /// Intersects @p ray with the placement's front-face rectangle; on a hit inside the rectangle,
    /// maps the hit's normalized position to the document coordinate space — the top-left corner is
    /// (0, 0), the bottom-right is @p documentExtent, matching the y-down document layout space that
    /// Element::Layout rects and PointerEvent::Position live in. A miss (the ray is parallel to the
    /// panel, points away from it, or hits outside the rectangle) yields nullopt — the panel is not
    /// under the ray. Pure geometry: no device, no material, identical for either material domain.
    /// @param ray             The world-space ray to intersect (Direction need not be unit length).
    /// @param placement       The panel's world placement.
    /// @param documentExtent  The extent the document was laid out at (its Solve size).
    /// @return The hit's document point and world distance, or nullopt on a miss.
    [[nodiscard]] VE_API optional<SurfaceRayHit>
    IntersectSurface(const Ray& ray, const SurfacePlacement& placement, vec2 documentExtent);

    /// @brief Intersects a world ray with a GuiSurface's panel, at the surface's own resolution.
    ///
    /// The convenience over IntersectSurface that reads the document extent from the surface's
    /// Resolution (the extent GuiSurface::Drive lays the document out at).
    /// @param ray        The world-space ray to intersect.
    /// @param placement  The panel's world placement.
    /// @param surface    The surface whose Resolution is the document extent.
    /// @return The hit's document point and world distance, or nullopt on a miss.
    [[nodiscard]] VE_API optional<SurfaceRayHit>
    IntersectSurface(const Ray& ray, const SurfacePlacement& placement, const GuiSurface& surface);

    /// @brief Returns only the document-local point a ray resolves to, discarding the hit distance.
    /// @param ray             The world-space ray to intersect.
    /// @param placement       The panel's world placement.
    /// @param documentExtent  The extent the document was laid out at.
    /// @return The document point, or nullopt on a miss.
    [[nodiscard]] VE_API optional<vec2>
    RayToDocumentPoint(const Ray& ray, const SurfacePlacement& placement, vec2 documentExtent);

    /// @brief Routes a world-ray pointer event into a seated surface's document.
    ///
    /// The seat gate: a surface with no seat (GuiSurface::Seat == Entity::Null) stays display-only —
    /// the adapter is never consulted and this returns nullopt, so a world of non-interactive holograms
    /// costs no input work. A seated surface's ray is intersected with its panel (IntersectSurface at
    /// the surface's Resolution); on a hit, a document-local PointerEvent of the given kind and button
    /// is dispatched through the document's capture→bubble pipeline (a Down then an Up on the same
    /// element synthesizing a Click), exactly as a screen-space pointer would. A display-only document
    /// (Document::SetInteractive not opened) consumes nothing, so the panel is inert until the game
    /// opens interactivity under a SeatFocusScope.
    /// @param surface    The surface whose document receives the event.
    /// @param placement  The panel's world placement.
    /// @param ray        The world-space pointer ray (a screen-driven cursor ray or a game-supplied one).
    /// @param kind       The pointer transition to deliver (Move / Down / Up).
    /// @param button     The button for a Down / Up; ignored for a Move.
    /// @return The resolved document point on a hit, or nullopt when the gate is closed or the ray misses.
    VE_API optional<vec2> RouteSurfacePointer(GuiSurface& surface,
                                              const SurfacePlacement& placement, const Ray& ray,
                                              PointerEventKind kind,
                                              PointerButton button = PointerButton::Primary);

    /// @brief The router consumer that routes world-ray pointers into interactive world panels.
    ///
    /// The world-space analogue of GuiConsumer: registered once with the InputRouter, it routes each
    /// UI-owned pointer event into the interactive GuiSurfaces participating this frame. A surface
    /// participates only while a Registration is held — the game creates one alongside the SeatFocusScope
    /// it opens on the surface's seat, so a panel is a router consumer exactly while it is interactive
    /// and drops out when the scope (and the registration) close. Each registration supplies the panel's
    /// live placement and pointer ray through callbacks pulled per event, so a moving panel or camera
    /// resolves correctly.
    ///
    /// Routing is scoped to the seat: a surface receives a pointer only while its seat's focus top is UI
    /// (the SeatFocusScope's takeover), so in split-screen a seat's in-world menu owns only that seat's
    /// devices. When several participating panels lie under one ray, the nearest hit wins (the only
    /// world-space arbitration; a richer policy is not modeled). The router must outlive the consumer.
    class SurfaceInputConsumer final : public InputConsumer
    {
    public:
        /// @brief An opaque per-registration id.
        using RegistrationId = u64;

        /// @brief RAII participation handle: a surface routes input while its Registration lives.
        ///
        /// Move-only; dropping it (or its owning scope) removes the surface from the consumer. A
        /// default/moved-from handle is inert.
        class Registration
        {
        public:
            /// @brief Constructs an inert, unregistered handle.
            Registration() = default;
            /// @brief Removes the surface from its consumer.
            ~Registration();

            Registration(const Registration&) = delete;
            Registration& operator=(const Registration&) = delete;

            /// @brief Move-constructs, transferring the registration.
            Registration(Registration&& other) noexcept;
            /// @brief Move-assigns, removing any current registration first.
            Registration& operator=(Registration&& other) noexcept;

        private:
            friend class SurfaceInputConsumer;
            Registration(SurfaceInputConsumer& consumer, RegistrationId id)
                : m_Consumer(&consumer), m_Id(id)
            {
            }

            void Reset();

            /// @brief The owning consumer, or nullptr when inert.
            SurfaceInputConsumer* m_Consumer = nullptr;
            /// @brief The id this handle removes on destruction.
            RegistrationId m_Id = 0;
        };

        /// @brief Constructs the consumer over the borrowed router; the router must outlive this.
        /// @param router  The router whose per-seat focus scopes each routed pointer.
        explicit SurfaceInputConsumer(InputRouter& router);

        /// @brief Registers a surface to participate while the returned handle is held.
        ///
        /// The surface routes pointer input only while its seat's focus top is UI. Placement and ray
        /// are pulled through the callbacks each event, so per-frame motion resolves live.
        /// @param surface    The surface whose document receives routed pointers; must outlive the handle.
        /// @param placement  Yields the panel's current world placement.
        /// @param ray        Yields the panel's current pointer ray, or nullopt when the panel has none.
        /// @return A handle that keeps the surface registered until it is dropped.
        [[nodiscard]] Registration Register(GuiSurface& surface,
                                            function<SurfacePlacement()> placement,
                                            function<optional<Ray>()> ray);

        /// @brief Offers one UI-owned pointer event to the participating world panels.
        /// @param event  The event to route.
        /// @return True when a panel consumed the event, stopping the fall-through.
        bool ForwardEvent(const Event& event) override;

    private:
        void Unregister(RegistrationId id);

        /// @brief One participating surface: its document sink, seat gate, and live placement/ray.
        struct Entry
        {
            /// @brief The registration id this entry is keyed by.
            RegistrationId Id = 0;
            /// @brief The surface whose document receives routed pointers.
            GuiSurface* Surface = nullptr;
            /// @brief Yields the panel's current world placement.
            function<SurfacePlacement()> Placement;
            /// @brief Yields the panel's current pointer ray, or nullopt when it has none.
            function<optional<Ray>()> Ray;
        };

        /// @brief The router whose per-seat focus scopes each routed pointer.
        InputRouter& m_Router;
        /// @brief The participating surfaces, in registration order.
        vector<Entry> m_Entries;
        /// @brief The next registration id handed out; monotonically increasing.
        RegistrationId m_NextId = 1;
    };
}
