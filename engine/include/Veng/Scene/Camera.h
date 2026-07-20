#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

#include <algorithm>
#include <cmath>

namespace Veng
{
    class Scene;

    /// @brief The thin-lens description a physically-authored camera resolves to, in metres.
    ///
    /// The pixel-independent half of the defocus parameterization: everything that depends
    /// only on the lens and the sensor, with the authored millimetre/metre mix already
    /// normalized to metres throughout (see ComputeCameraLens, the one conversion site).
    /// The sensor-to-pixel scale is deliberately absent — it needs the target's pixel
    /// height, which a lens does not know; ComputeDofParams adds it.
    struct CameraLens
    {
        /// @brief Aperture diameter in metres (focal length divided by the f-number).
        f32 Aperture = 0.0f;
        /// @brief Distance from the camera to the plane in perfect focus, in metres.
        f32 FocusDistance = 0.0f;
        /// @brief Sensor height in metres.
        f32 SensorHeight = 0.0f;
    };

    /// @brief Plain CPU value type that builds the view and projection matrices a SceneView carries.
    ///
    /// The render-ready view-projection the renderer consumes through SceneView, the
    /// resolved side of the recipe→resolved pairing (a Camera component produces a
    /// CameraView). Pure math, no backend handles.
    /// Projection follows the engine's Vulkan clip conventions: a column-major GLM
    /// perspective with Y flipped (Vulkan clip space has Y pointing down). Matrices are
    /// recomputed on demand from stored parameters.
    class CameraView
    {
    public:
        /// @brief Sets a perspective projection.
        /// @param fovYRadians  Vertical field of view in radians.
        /// @param aspect       Width divided by height.
        /// @param near         Positive near clip distance.
        /// @param far          Positive far clip distance.
        void SetPerspective(f32 fovYRadians, f32 aspect, f32 near, f32 far)
        {
            mat4 projection = glm::perspective(fovYRadians, aspect, near, far);
            projection[1][1] *= -1.0f; // Vulkan's clip space has Y pointing down.
            m_Projection = projection;
            m_Near = near;
            m_Far = far;
        }

        /// @brief Sets an orthographic projection.
        ///
        /// Parallel projection: no perspective foreshortening, so a face directly
        /// facing the camera projects at uniform scale regardless of depth. Follows
        /// the same Vulkan clip conventions as SetPerspective (Y flipped, ZO depth).
        /// @param halfWidth   Half the view volume's width in world units.
        /// @param halfHeight  Half the view volume's height in world units.
        /// @param near        Near clip distance.
        /// @param far         Far clip distance.
        void SetOrthographic(f32 halfWidth, f32 halfHeight, f32 near, f32 far)
        {
            mat4 projection = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, near, far);
            projection[1][1] *= -1.0f; // Vulkan's clip space has Y pointing down.
            m_Projection = projection;
            m_Near = near;
            m_Far = far;
        }

        /// @brief Sets the view matrix from eye, target, and up vectors.
        void SetView(vec3 eye, vec3 target, vec3 up) { m_View = glm::lookAt(eye, target, up); }

        /// @brief Sets the view matrix from a camera entity's world matrix.
        ///
        /// The view is the inverse of the world matrix: world places the camera,
        /// view brings the world into camera space.
        void SetViewFromWorld(const mat4& world) { m_View = glm::inverse(world); }

        /// @brief Returns the view matrix.
        [[nodiscard]] mat4 View() const { return m_View; }
        /// @brief Returns the projection matrix.
        [[nodiscard]] mat4 Projection() const { return m_Projection; }
        /// @brief Returns the combined view-projection matrix.
        [[nodiscard]] mat4 ViewProjection() const { return m_Projection * m_View; }

        /// @brief Returns the camera's world-space position (the translation column of the view's inverse).
        [[nodiscard]] vec3 GetPosition() const { return vec3(glm::inverse(m_View)[3]); }

        /// @brief Returns the near clip distance.
        ///
        /// Recovering near/far from the projection matrix is fiddly under the Y-flip,
        /// so the camera stores them. SetPerspective sets both; a camera whose
        /// projection was set by another path keeps the defaults (0.1 / 100.0).
        [[nodiscard]] f32 GetNear() const { return m_Near; }
        /// @brief Returns the far clip distance.
        [[nodiscard]] f32 GetFar() const { return m_Far; }

        /// @brief Sets the lens this view was resolved from, marking it physically authored.
        void SetLens(const CameraLens& lens) { m_Lens = lens; }

        /// @brief Returns the lens this view was resolved from, or nullopt when it carries none.
        ///
        /// Present only when the source Camera component resolved through
        /// CameraProjection::Physical, so a consumer reads it as "was this view authored in
        /// lens terms?". The pixel-dependent circle-of-confusion scale is not here: a consumer
        /// that knows its target's pixel height derives the full set through ComputeDofParams.
        [[nodiscard]] const optional<CameraLens>& GetLens() const { return m_Lens; }

    private:
        /// @brief View matrix (world-to-camera).
        mat4 m_View{1.0f};
        /// @brief Projection matrix (Y-flipped for Vulkan clip space).
        mat4 m_Projection{1.0f};
        /// @brief Near clip distance, mirroring the value passed to SetPerspective.
        f32 m_Near = 0.1f;
        /// @brief Far clip distance, mirroring the value passed to SetPerspective.
        f32 m_Far = 100.0f;
        /// @brief The lens this view resolved from; empty unless the source camera was physical.
        optional<CameraLens> m_Lens;
    };

    /// @brief Which projection a Camera component resolves through.
    enum class CameraProjection : u8
    {
        /// @brief Perspective projection from FovY (the default).
        Perspective,
        /// @brief Orthographic (parallel) projection from OrthoHeight — no foreshortening.
        Orthographic,
        /// @brief Perspective projection derived from lens terms — focal length over sensor height.
        ///
        /// The projection is an ordinary perspective; what differs is where its vertical field
        /// of view comes from (FocalLength and SensorHeight rather than the authored FovY) and
        /// that the resolved view carries a CameraLens a defocus consumer can read.
        Physical,
    };

    /// @brief Camera component for an entity whose view derives from its world transform.
    struct Camera
    {
        /// @brief The projection kind the camera resolves through (see CameraProjection).
        CameraProjection Projection = CameraProjection::Perspective;
        /// @brief Vertical field of view in radians (Perspective only).
        f32 FovY = glm::radians(60.0f);
        /// @brief The view volume's full vertical extent in world units (Orthographic only);
        ///        the horizontal extent follows the render target's aspect.
        f32 OrthoHeight = 10.0f;
        /// @brief Near clip distance.
        f32 Near = 0.1f;
        /// @brief Far clip distance.
        f32 Far = 100.0f;
        /// @brief Lens focal length in millimetres (Physical only).
        f32 FocalLength = 50.0f;
        /// @brief Sensor height in millimetres (Physical only).
        ///
        /// The full-frame reference is 24; the vertical field of view and the circle of
        /// confusion both scale from it.
        f32 SensorHeight = 24.0f;
        /// @brief Lens f-number (Physical only); the aperture diameter is FocalLength / FStop.
        f32 FStop = 2.8f;
        /// @brief Distance to the plane in perfect focus, in metres (Physical only).
        f32 FocusDistance = 10.0f;
    };

    /// @brief Normalizes a physical camera's authored lens fields into a metres-only CameraLens.
    ///
    /// The single unit-conversion site for the physical camera: FocalLength and SensorHeight
    /// are authored in millimetres while FocusDistance is authored in metres, so every other
    /// consumer derives from this result rather than mixing the two scales itself. Each input
    /// is floored at a small positive value, so a zeroed or partially-authored component
    /// yields a finite lens rather than a division by zero.
    /// @param camera  The component supplying FocalLength, SensorHeight, FStop, FocusDistance.
    /// @return The lens in metres throughout.
    [[nodiscard]] inline CameraLens ComputeCameraLens(const Camera& camera)
    {
        constexpr f32 MillimetresPerMetre = 1000.0f;
        const f32 focalLength = std::max(camera.FocalLength, 1.0e-3f) / MillimetresPerMetre;
        const f32 sensorHeight = std::max(camera.SensorHeight, 1.0e-3f) / MillimetresPerMetre;
        return CameraLens{
            .Aperture = focalLength / std::max(camera.FStop, 1.0e-3f),
            .FocusDistance = std::max(camera.FocusDistance, 1.0e-4f),
            .SensorHeight = sensorHeight,
        };
    }

    /// @brief The thin-lens defocus constants a depth-of-field consumer evaluates its
    ///        circle-of-confusion curve from.
    ///
    /// The complete pixel-space parameterization: the lens's metre-space values plus the
    /// sensor-to-pixel scale that depends on the target's pixel height. See
    /// ComputeCircleOfConfusion for the curve these three define.
    struct DofParams
    {
        /// @brief Aperture diameter in metres.
        f32 Aperture = 0.0f;
        /// @brief Distance to the plane in perfect focus, in metres.
        f32 FocusDistance = 0.0f;
        /// @brief Sensor-to-pixel scale in pixels per metre (target pixel height / sensor height).
        f32 CocScale = 0.0f;
    };

    /// @brief Resolves a lens and a target's pixel height into the defocus constants.
    /// @param lens               The lens, in metres throughout (from ComputeCameraLens).
    /// @param viewportPixelHeight  The target's vertical extent in pixels; the sensor maps onto it.
    /// @return The aperture, focus distance, and sensor-to-pixel scale.
    [[nodiscard]] inline DofParams ComputeDofParams(const CameraLens& lens,
                                                    const f32 viewportPixelHeight)
    {
        return DofParams{
            .Aperture = lens.Aperture,
            .FocusDistance = lens.FocusDistance,
            .CocScale = std::max(viewportPixelHeight, 1.0f) / std::max(lens.SensorHeight, 1.0e-6f),
        };
    }

    /// @brief Resolves a physical camera and a target's pixel height into the defocus constants.
    ///
    /// The whole conversion in one call, for a caller holding the component rather than an
    /// already-resolved view: it normalizes the authored millimetre/metre mix through
    /// ComputeCameraLens and adds the pixel scale. Device-free pure math.
    /// @param camera               The component supplying the lens fields.
    /// @param viewportPixelHeight  The target's vertical extent in pixels.
    /// @return The aperture and focus distance in metres, and the CoC scale in pixels per metre.
    [[nodiscard]] inline DofParams ComputeDofParams(const Camera& camera,
                                                    const f32 viewportPixelHeight)
    {
        return ComputeDofParams(ComputeCameraLens(camera), viewportPixelHeight);
    }

    /// @brief Evaluates the signed circle of confusion, in pixels, at a view-space depth.
    ///
    /// The thin-lens curve the defocus parameters define, for depths well beyond the focal
    /// length: `CocScale * Aperture * (depth - FocusDistance) / depth`. The sign carries the
    /// field — negative in front of the focus plane, positive behind it — and the magnitude is
    /// the blur diameter in target pixels. The curve is asymmetric: it grows without bound as
    /// depth approaches the camera, while behind the focus plane it converges to the
    /// aperture-scaled constant `CocScale * Aperture`. A non-positive depth has no defocus
    /// defined and returns zero.
    ///
    /// This is the reference form; a GPU consumer evaluates the same expression per pixel from
    /// the same three constants.
    /// @param params  The defocus constants from ComputeDofParams.
    /// @param depth   View-space distance from the camera, in metres.
    /// @return The signed circle of confusion in pixels.
    [[nodiscard]] inline f32 ComputeCircleOfConfusion(const DofParams& params, const f32 depth)
    {
        if (depth <= 0.0f)
        {
            return 0.0f;
        }
        return params.CocScale * params.Aperture * (depth - params.FocusDistance) / depth;
    }

    /// @brief Builds a CameraView from a Camera component, an aspect ratio, and the camera entity's world matrix.
    ///
    /// A Physical camera resolves as a perspective projection whose vertical field of view is
    /// derived from the lens, `2 * atan(SensorHeight / (2 * FocalLength))` — a ratio, so the
    /// authored millimetres cancel — and the resolved view carries the CameraLens a defocus
    /// consumer reads back through CameraView::GetLens.
    /// @param camera  The component supplying the projection (FovY, OrthoHeight, or the lens, plus Near/Far).
    /// @param aspect  Viewport width divided by height.
    /// @param world   The camera entity's world matrix (from WorldMatrix in Transforms.h).
    [[nodiscard]] inline CameraView MakeCameraView(const Camera& camera, f32 aspect,
                                                   const mat4& world)
    {
        CameraView result;
        if (camera.Projection == CameraProjection::Orthographic)
        {
            const f32 halfHeight = std::max(camera.OrthoHeight, 1.0e-4f) * 0.5f;
            result.SetOrthographic(halfHeight * aspect, halfHeight, camera.Near, camera.Far);
        }
        else if (camera.Projection == CameraProjection::Physical)
        {
            const f32 fovY = 2.0f * std::atan(std::max(camera.SensorHeight, 1.0e-3f) /
                                              (2.0f * std::max(camera.FocalLength, 1.0e-3f)));
            result.SetPerspective(fovY, aspect, camera.Near, camera.Far);
            result.SetLens(ComputeCameraLens(camera));
        }
        else
        {
            result.SetPerspective(camera.FovY, aspect, camera.Near, camera.Far);
        }
        result.SetViewFromWorld(world);
        return result;
    }

    /// @brief Projects a world-space point through a view into top-left-origin screen pixels.
    ///
    /// The inverse direction of an unproject: clip → NDC → pixels over the given extent. The
    /// engine projection bakes the Vulkan Y flip, so NDC maps to a top-left-origin pixel rect
    /// directly — (0,0) is the extent's top-left. A point behind the camera (clip w <= 0)
    /// returns nullopt; a point outside the extent still projects (the caller owns clamping or
    /// rejecting an off-screen result). What the pixels mean — a window, a viewport region, a
    /// UI document — is the caller's extent.
    /// @param camera  The view to project through.
    /// @param world   The world-space point.
    /// @param extent  The target pixel extent NDC maps onto.
    /// @return The projected pixel position, or nullopt behind the camera.
    [[nodiscard]] inline optional<vec2> ProjectToScreen(const CameraView& camera, const vec3 world,
                                                        const vec2 extent)
    {
        const vec4 clip = camera.ViewProjection() * vec4(world, 1.0f);
        if (clip.w <= 0.0f)
        {
            return std::nullopt;
        }
        const vec2 ndc = vec2(clip) / clip.w;
        return (ndc * 0.5f + 0.5f) * extent;
    }

    /// @brief Seat-to-camera selection: names the camera entity a seat renders through.
    ///
    /// A seat — a local player, a render target, the editor — carries a Viewer naming
    /// the camera entity it looks through, separating the seat from the camera: the
    /// camera is (Transform, Camera) data, the Viewer is "this seat sees through that
    /// one." The Camera field is a reflected Entity reference, so it remaps on prefab
    /// spawn like any intra-prefab reference.
    struct Viewer
    {
        /// @brief The camera entity this seat renders through.
        Entity Camera = Entity::Null;
    };

    /// @brief Resolves the CameraView a seat renders through, at the caller's aspect.
    ///
    /// Reads the Viewer on viewer, looks up its named Camera entity, and projects the
    /// camera through its world matrix (WorldMatrix walks the Parent edge, so a camera
    /// parented under a rig resolves correctly).
    /// @param scene   The scene the seat and camera live in.
    /// @param viewer  The seat entity carrying the Viewer component.
    /// @param aspect  Viewport width divided by height; the render target owns aspect.
    /// @return The resolved view, or nullopt if the seat has no Viewer, names
    ///         Entity::Null, or the named entity lacks (Transform, Camera).
    [[nodiscard]] VE_API optional<CameraView> ResolveCameraView(const Scene& scene, Entity viewer,
                                                                f32 aspect);

    /// @brief Resolves the single-output CameraView for a one-seat scene, at the caller's aspect.
    ///
    /// Resolves through the first Viewer entity; failing that, falls back to the first
    /// bare (Transform, Camera) entity, so a one-camera scene with no explicit seat
    /// still renders. More than one Viewer is fine — this convenience takes the first.
    /// @param scene   The scene to resolve a view from.
    /// @param aspect  Viewport width divided by height; the render target owns aspect.
    /// @return The resolved view, or nullopt if the scene has no resolvable camera.
    [[nodiscard]] VE_API optional<CameraView> ResolvePrimaryCameraView(const Scene& scene,
                                                                       f32 aspect);

    /// @brief Returns a default framing view for a scene that resolves no camera.
    ///
    /// A 45° perspective pulled back and elevated, looking at the origin — the safety-net view a
    /// renderer falls back to when ResolvePrimaryCameraView yields nullopt, so an unconfigured or
    /// camera-less scene still renders something rather than nothing.
    /// @param aspect  Viewport width divided by height.
    /// @return The fallback view.
    [[nodiscard]] inline CameraView DefaultCameraView(f32 aspect)
    {
        CameraView view;
        view.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        view.SetView(vec3(0.0f, 10.0f, 14.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return view;
    }
}

VE_ENUM(::Veng::CameraProjection, 0x2A283514BD366CA5ULL)
VE_ENUMERATOR(Perspective)
VE_ENUMERATOR(Orthographic)
VE_ENUMERATOR(Physical)
VE_ENUM_END();

VE_REFLECT(::Veng::Camera, 0x6598EF5F5C0A7B10ULL)
VE_FIELD(Projection, .DisplayName = "Projection")
VE_FIELD(FovY, .DisplayName = "Field of View", .Display = {.Min = 0.01})
VE_FIELD(OrthoHeight, .DisplayName = "Ortho Height", .Display = {.Min = 0.0001})
VE_FIELD(Near, .DisplayName = "Near", .Display = {.Min = 0.001})
VE_FIELD(Far, .DisplayName = "Far")
VE_FIELD(FocalLength, .DisplayName = "Focal Length",
         .Tooltip = "Lens focal length in millimetres; with the sensor height it sets the "
                    "vertical field of view.",
         .Display = {.Min = 1.0, .Max = 800.0}, .Category = "Lens")
VE_FIELD(SensorHeight, .DisplayName = "Sensor Height",
         .Tooltip = "Sensor height in millimetres (24 is the full-frame reference).",
         .Display = {.Min = 1.0, .Max = 100.0}, .Category = "Lens")
VE_FIELD(FStop, .DisplayName = "F-Stop",
         .Tooltip = "Lens f-number; the aperture diameter is the focal length divided by it, so "
                    "a smaller number defocuses more.",
         .Display = {.Min = 0.7, .Max = 32.0}, .Category = "Lens")
VE_FIELD(FocusDistance, .DisplayName = "Focus Distance",
         .Tooltip = "Distance to the plane in perfect focus, in metres.", .Display = {.Min = 0.01},
         .Category = "Lens")
VE_REFLECT_END();

VE_REFLECT(::Veng::Viewer, 0x879A9712E090AC19ULL)
VE_FIELD(Camera, .DisplayName = "Camera")
VE_REFLECT_END();
// A seat entity is server-authoritative and replicated so the client learns which seats exist. The
// Camera reference names a client-local (Local-tier) camera, so it encodes as a null wire reference
// — the client re-wires its own camera to the replicated seat, rather than receiving one.
VE_REPLICATED(::Veng::Viewer);
VE_ALWAYS_RELEVANT(::Veng::Viewer);
