#include "EditorGizmo.h"

#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <limits>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // Linear-RGBA per-axis colors (X red, Y green, Z blue) and the highlight tint.
        constexpr vec4 AxisColor[3]{
            {0.9f, 0.2f, 0.2f, 1.0f}, {0.2f, 0.85f, 0.2f, 1.0f}, {0.3f, 0.45f, 1.0f, 1.0f}};
        constexpr vec4 HighlightColor{1.0f, 0.85f, 0.1f, 1.0f};
        constexpr vec4 UniformColor{0.85f, 0.85f, 0.85f, 1.0f};

        // The gizmo's on-screen size: this fraction of the distance from the camera to the pivot
        // is one world-unit of handle, so the gizmo stays a roughly constant size as it recedes.
        constexpr f32 ScaleFactor = 0.18f;
        // Pick tolerance in gizmo-scale units (a handle is grabbed within this of its ideal line).
        constexpr f32 AxisPickTolerance = 0.12f;
        // The plane handle's quad spans this fraction of the axis length, offset from the origin.
        constexpr f32 PlaneOffset = 0.35f;
        constexpr f32 PlaneSize = 0.25f;
        // The rotate ring radius as a fraction of the handle scale, and its pick band half-width.
        constexpr f32 RingRadius = 0.9f;
        constexpr f32 RingPickTolerance = 0.1f;
        // Ribbon thickness of the gizmo's debug-draw lines/boxes, in screen pixels.
        constexpr f32 GizmoLineWidth = 3.0f;

        /// @brief Returns the squared distance between a ray and a finite segment, with the ray parameter on the ray.
        ///
        /// Closest points between the semi-infinite cursor ray (parameter s ≥ 0) and the
        /// segment a→b (parameter t in [0,1]); used to grab an axis handle when the cursor ray
        /// passes near the axis line. The ray parameter is not upper-bounded: ray.Direction is
        /// normalized, so clamping s to a segment would treat the ray as one unit long and the
        /// gizmo — many units ahead of the camera — would never be reached.
        f32 RaySegmentDistanceSq(const Ray& ray, vec3 a, vec3 b, f32& outRayT, vec3& outSegPoint)
        {
            const vec3 d1 = ray.Direction;
            const vec3 d2 = b - a;
            const vec3 r = ray.Origin - a;
            const f32 aa = glm::dot(d1, d1);
            const f32 e = glm::dot(d2, d2);
            const f32 f = glm::dot(d2, r);
            const f32 c = glm::dot(d1, r);
            const f32 bdot = glm::dot(d1, d2);
            const f32 denom = aa * e - bdot * bdot;

            // Unconstrained ray parameter, then lower-bounded to the ray (s ≥ 0); parallel
            // lines (denom ≈ 0) pin s to the ray origin.
            f32 s = denom > 1e-6f ? glm::max((bdot * f - c * e) / denom, 0.0f) : 0.0f;
            f32 t = (bdot * s + f) / glm::max(e, 1e-6f);
            // Clamp t to the segment and re-solve s for that endpoint (still ray-bounded).
            if (t < 0.0f)
            {
                t = 0.0f;
                s = glm::max(-c / glm::max(aa, 1e-6f), 0.0f);
            }
            else if (t > 1.0f)
            {
                t = 1.0f;
                s = glm::max((bdot - c) / glm::max(aa, 1e-6f), 0.0f);
            }

            const vec3 rayPoint = ray.Origin + d1 * s;
            const vec3 segPoint = a + d2 * t;
            outRayT = s;
            outSegPoint = segPoint;
            return glm::dot(rayPoint - segPoint, rayPoint - segPoint);
        }

        /// @brief Intersects a ray with a plane through @p point with normal @p normal; false when parallel.
        bool RayPlane(const Ray& ray, vec3 point, vec3 normal, vec3& outHit)
        {
            const f32 denom = glm::dot(normal, ray.Direction);
            if (std::abs(denom) < 1e-6f)
            {
                return false;
            }
            const f32 t = glm::dot(point - ray.Origin, normal) / denom;
            if (t < 0.0f)
            {
                return false;
            }
            outHit = ray.At(t);
            return true;
        }

        /// @brief Intersects a ray with an axis-aligned box (min/max); writes the near hit distance.
        bool RayBox(const Ray& ray, vec3 boxMin, vec3 boxMax, f32& outT)
        {
            f32 tMin = 0.0f;
            f32 tMax = std::numeric_limits<f32>::max();
            for (i32 i = 0; i < 3; ++i)
            {
                if (std::abs(ray.Direction[i]) < 1e-8f)
                {
                    if (ray.Origin[i] < boxMin[i] || ray.Origin[i] > boxMax[i])
                    {
                        return false;
                    }
                    continue;
                }
                const f32 inv = 1.0f / ray.Direction[i];
                f32 t1 = (boxMin[i] - ray.Origin[i]) * inv;
                f32 t2 = (boxMax[i] - ray.Origin[i]) * inv;
                if (t1 > t2)
                {
                    std::swap(t1, t2);
                }
                tMin = glm::max(tMin, t1);
                tMax = glm::min(tMax, t2);
                if (tMin > tMax)
                {
                    return false;
                }
            }
            outT = tMin;
            return true;
        }

        /// @brief The axis index (0/1/2) for an axis handle, or -1 for a non-axis handle.
        i32 AxisIndex(GizmoHandle handle)
        {
            switch (handle)
            {
            case GizmoHandle::AxisX:
            case GizmoHandle::PlaneX:
                return 0;
            case GizmoHandle::AxisY:
            case GizmoHandle::PlaneY:
                return 1;
            case GizmoHandle::AxisZ:
            case GizmoHandle::PlaneZ:
                return 2;
            default:
                return -1;
            }
        }
    }

    EditorGizmo::Placement EditorGizmo::ComputePlacement(const Scene& scene, const Entity entity,
                                                         const vec3 cameraPosition,
                                                         const GizmoMode mode)
    {
        Placement placement;
        const mat4 world = WorldMatrix(scene, entity);
        placement.Origin = vec3(world[3]);

        if (mode == GizmoMode::Scale)
        {
            // Scale writes the entity's local Transform::Scale[axis], so the handles must follow
            // the entity's own frame in world space (its world-matrix basis vectors) — a
            // world-aligned handle would scale along the object's local diagonal once it is
            // rotated. A degenerate (zero-scale) basis vector falls back to the world axis.
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const vec3 basis = vec3(world[axis]);
                vec3 fallback{0.0f};
                fallback[axis] = 1.0f;
                placement.Axes[axis] =
                    glm::length(basis) > 1e-6f ? glm::normalize(basis) : fallback;
            }
        }
        else
        {
            // Translate and rotate manipulate in world space — their drag maps the world delta
            // back into the parent frame — so the handles are the world basis regardless of the
            // entity's parent frame.
            placement.Axes[0] = vec3(1.0f, 0.0f, 0.0f);
            placement.Axes[1] = vec3(0.0f, 1.0f, 0.0f);
            placement.Axes[2] = vec3(0.0f, 0.0f, 1.0f);
        }

        const f32 distance = glm::length(cameraPosition - placement.Origin);
        placement.Scale = glm::max(distance * ScaleFactor, 0.05f);
        return placement;
    }

    GizmoHandle EditorGizmo::Pick(const Placement& placement, const Ray& ray,
                                  const GizmoMode mode) const
    {
        const f32 length = placement.Scale;
        const f32 tolerance = AxisPickTolerance * length;

        if (mode == GizmoMode::Rotate)
        {
            // Ray vs. each axis ring: intersect the ring's plane, then test the hit's distance
            // from the pivot against the ring radius within a band.
            const f32 radius = RingRadius * length;
            const f32 band = RingPickTolerance * length;
            GizmoHandle best = GizmoHandle::None;
            f32 bestDistance = std::numeric_limits<f32>::max();
            for (i32 axis = 0; axis < 3; ++axis)
            {
                vec3 hit;
                if (!RayPlane(ray, placement.Origin, placement.Axes[axis], hit))
                {
                    continue;
                }
                const f32 radial = glm::length(hit - placement.Origin);
                if (std::abs(radial - radius) <= band)
                {
                    const f32 cameraDistance = glm::length(hit - ray.Origin);
                    if (cameraDistance < bestDistance)
                    {
                        bestDistance = cameraDistance;
                        best = static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::AxisX) + axis);
                    }
                }
            }
            return best;
        }

        // Translate and Scale share the axis-line handles; Translate adds plane quads.
        GizmoHandle best = GizmoHandle::None;
        f32 bestRayT = std::numeric_limits<f32>::max();

        for (i32 axis = 0; axis < 3; ++axis)
        {
            const vec3 tip = placement.Origin + placement.Axes[axis] * length;
            f32 rayT = 0.0f;
            vec3 segPoint;
            const f32 distSq = RaySegmentDistanceSq(ray, placement.Origin, tip, rayT, segPoint);
            if (distSq <= tolerance * tolerance && rayT < bestRayT)
            {
                bestRayT = rayT;
                best = static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::AxisX) + axis);
            }
        }

        if (mode == GizmoMode::Translate)
        {
            // Plane handles: a small quad in each axis-pair plane, offset from the origin.
            const f32 lo = PlaneOffset * length;
            const f32 hi = (PlaneOffset + PlaneSize) * length;
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const i32 u = (axis + 1) % 3;
                const i32 v = (axis + 2) % 3;
                vec3 hit;
                if (!RayPlane(ray, placement.Origin, placement.Axes[axis], hit))
                {
                    continue;
                }
                const vec3 local = hit - placement.Origin;
                const f32 du = glm::dot(local, placement.Axes[u]);
                const f32 dv = glm::dot(local, placement.Axes[v]);
                if (du >= lo && du <= hi && dv >= lo && dv <= hi)
                {
                    const f32 rayT = glm::length(hit - ray.Origin);
                    if (rayT < bestRayT)
                    {
                        bestRayT = rayT;
                        best =
                            static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::PlaneX) + axis);
                    }
                }
            }
        }

        if (mode == GizmoMode::Scale)
        {
            // The scale handles are boxes, so pick their volume (not only the axis line) — the
            // tip box extends past the line's grab cylinder, so a click on the box itself would
            // otherwise miss.
            const f32 axisHalf = AxisPickTolerance * length * 0.9f;
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const vec3 tip = placement.Origin + placement.Axes[axis] * length;
                f32 tMin = 0.0f;
                if (RayBox(ray, tip - vec3(axisHalf), tip + vec3(axisHalf), tMin) &&
                    tMin < bestRayT)
                {
                    bestRayT = tMin;
                    best = static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::AxisX) + axis);
                }
            }

            // Uniform-scale center box: a small box at the pivot.
            const f32 half = AxisPickTolerance * length * 1.5f;
            f32 tMin = 0.0f;
            if (RayBox(ray, placement.Origin - vec3(half), placement.Origin + vec3(half), tMin) &&
                tMin < bestRayT)
            {
                bestRayT = tMin;
                best = GizmoHandle::Uniform;
            }
        }

        return best;
    }

    void EditorGizmo::Draw(Renderer::DebugDraw& debug, const Scene& scene, const Entity entity,
                           const vec3 cameraPosition, const GizmoMode mode) const
    {
        if (entity.IsNull() || !scene.IsAlive(entity))
        {
            return;
        }

        // A drag draws in the mode it was captured in, so a shared-mode change mid-drag does not
        // redraw the handles out from under the dragged one.
        const GizmoMode drawMode = m_Dragging ? m_DragMode : mode;
        const Placement placement = ComputePlacement(scene, entity, cameraPosition, drawMode);
        const f32 length = placement.Scale;
        const GizmoHandle active = m_Dragging ? m_DragHandle : m_Hovered;

        auto axisColor = [&](const i32 axis, const GizmoHandle handle)
        { return active == handle ? HighlightColor : AxisColor[axis]; };

        if (drawMode == GizmoMode::Rotate)
        {
            constexpr u32 segments = 48;
            const f32 radius = RingRadius * length;
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const vec3 u = placement.Axes[(axis + 1) % 3];
                const vec3 v = placement.Axes[(axis + 2) % 3];
                const GizmoHandle handle =
                    static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::AxisX) + axis);
                const vec4 color = axisColor(axis, handle);
                vec3 prev;
                for (u32 i = 0; i <= segments; ++i)
                {
                    const f32 a =
                        glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(segments);
                    const vec3 point =
                        placement.Origin + radius * (std::cos(a) * u + std::sin(a) * v);
                    if (i > 0)
                    {
                        debug.DrawLine(prev, point, color, GizmoLineWidth);
                    }
                    prev = point;
                }
            }
            return;
        }

        // Translate / Scale: an arm per axis.
        for (i32 axis = 0; axis < 3; ++axis)
        {
            const vec3 tip = placement.Origin + placement.Axes[axis] * length;
            const GizmoHandle handle =
                static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::AxisX) + axis);
            const vec4 color = axisColor(axis, handle);
            debug.DrawLine(placement.Origin, tip, color, GizmoLineWidth);

            if (drawMode == GizmoMode::Scale)
            {
                // A small box at the tip marks a scale grab.
                const f32 half = AxisPickTolerance * length * 0.9f;
                debug.DrawBox({tip - vec3(half), tip + vec3(half)}, color, GizmoLineWidth);
            }
        }

        if (drawMode == GizmoMode::Translate)
        {
            const f32 lo = PlaneOffset * length;
            const f32 hi = (PlaneOffset + PlaneSize) * length;
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const vec3 u = placement.Axes[(axis + 1) % 3];
                const vec3 v = placement.Axes[(axis + 2) % 3];
                const GizmoHandle handle =
                    static_cast<GizmoHandle>(static_cast<u8>(GizmoHandle::PlaneX) + axis);
                const vec4 color = active == handle ? HighlightColor : AxisColor[axis];
                const vec3 a = placement.Origin + u * lo + v * lo;
                const vec3 b = placement.Origin + u * hi + v * lo;
                const vec3 c = placement.Origin + u * hi + v * hi;
                const vec3 d = placement.Origin + u * lo + v * hi;
                debug.DrawLine(a, b, color, GizmoLineWidth);
                debug.DrawLine(b, c, color, GizmoLineWidth);
                debug.DrawLine(c, d, color, GizmoLineWidth);
                debug.DrawLine(d, a, color, GizmoLineWidth);
            }
        }

        if (drawMode == GizmoMode::Scale)
        {
            const f32 half = AxisPickTolerance * length * 1.5f;
            const vec4 color = active == GizmoHandle::Uniform ? HighlightColor : UniformColor;
            debug.DrawBox({placement.Origin - vec3(half), placement.Origin + vec3(half)}, color,
                          GizmoLineWidth);
        }
    }

    bool EditorGizmo::Hover(const Scene& scene, const Entity entity, const Ray& ray,
                            const vec3 cameraPosition, const GizmoMode mode)
    {
        if (m_Dragging)
        {
            return true;
        }
        if (entity.IsNull() || !scene.IsAlive(entity))
        {
            m_Hovered = GizmoHandle::None;
            return false;
        }
        const Placement placement = ComputePlacement(scene, entity, cameraPosition, mode);
        m_Hovered = Pick(placement, ray, mode);
        return m_Hovered != GizmoHandle::None;
    }

    bool EditorGizmo::BeginDrag(const Scene& scene, const Entity entity, const Ray& ray,
                                const vec3 cameraPosition, const GizmoMode mode)
    {
        if (m_Dragging || entity.IsNull() || !scene.IsAlive(entity))
        {
            return false;
        }
        const Transform* transform = scene.TryGet<Transform>(entity);
        if (transform == nullptr)
        {
            return false;
        }

        const Placement placement = ComputePlacement(scene, entity, cameraPosition, mode);
        const GizmoHandle handle = Pick(placement, ray, mode);
        if (handle == GizmoHandle::None)
        {
            return false;
        }

        m_Dragging = true;
        m_DragMode = mode;
        m_DragHandle = handle;
        m_Hovered = handle;
        m_StartTransform = *transform;
        m_DragPlacement = placement;
        m_StartWorldPosition = placement.Origin;

        const i32 axis = AxisIndex(handle);
        if (mode == GizmoMode::Translate)
        {
            if (handle >= GizmoHandle::PlaneX && handle <= GizmoHandle::PlaneZ)
            {
                // Plane translate: the grab point is the ray ∩ the handle plane.
                vec3 hit = placement.Origin;
                RayPlane(ray, placement.Origin, placement.Axes[axis], hit);
                m_GrabAnchor = hit;
            }
            else
            {
                // Axis translate: the grab point is the closest point on the axis line to the ray.
                f32 rayT = 0.0f;
                vec3 segPoint = placement.Origin;
                RaySegmentDistanceSq(ray, placement.Origin,
                                     placement.Origin + placement.Axes[axis] * placement.Scale,
                                     rayT, segPoint);
                // Unclamped axis projection so the entity can be dragged past the arm tip.
                const vec3 onLine = ray.At(rayT);
                m_GrabAnchor =
                    placement.Origin + placement.Axes[axis] * glm::dot(onLine - placement.Origin,
                                                                       placement.Axes[axis]);
            }
        }
        else if (mode == GizmoMode::Rotate)
        {
            // The grab anchor's x stores the initial angle in the ring plane.
            vec3 hit = placement.Origin;
            RayPlane(ray, placement.Origin, placement.Axes[axis], hit);
            const vec3 u = placement.Axes[(axis + 1) % 3];
            const vec3 v = placement.Axes[(axis + 2) % 3];
            const vec3 radial = hit - placement.Origin;
            m_GrabAnchor = vec3(std::atan2(glm::dot(radial, v), glm::dot(radial, u)), 0.0f, 0.0f);
        }
        else // Scale
        {
            if (handle == GizmoHandle::Uniform)
            {
                // Uniform scale tracks the cursor on the camera-facing plane through the pivot;
                // the ray origin is the fixed camera, so it cannot measure cursor motion.
                m_DragViewNormal = glm::normalize(cameraPosition - placement.Origin);
                vec3 hit = placement.Origin;
                RayPlane(ray, placement.Origin, m_DragViewNormal, hit);
                m_GrabAnchor = hit;
            }
            else
            {
                f32 rayT = 0.0f;
                vec3 segPoint = placement.Origin;
                RaySegmentDistanceSq(ray, placement.Origin,
                                     placement.Origin + placement.Axes[axis] * placement.Scale,
                                     rayT, segPoint);
                const vec3 onLine = ray.At(rayT);
                m_GrabAnchor =
                    vec3(glm::dot(onLine - placement.Origin, placement.Axes[axis]), 0.0f, 0.0f);
            }
        }
        return true;
    }

    void EditorGizmo::Drag(Scene& scene, const Entity entity, const Ray& ray)
    {
        if (!m_Dragging || entity.IsNull() || !scene.IsAlive(entity) ||
            scene.TryGet<Transform>(entity) == nullptr)
        {
            return;
        }

        const Placement& placement = m_DragPlacement;
        const i32 axis = AxisIndex(m_DragHandle);

        // The parent world matrix maps the world-space delta back into the entity's local frame
        // (identity for an unparented entity, where local == world).
        const mat4 entityWorld = WorldMatrix(scene, entity);
        const mat4 localToParent = LocalMatrix(m_StartTransform);
        const mat4 parentWorld = entityWorld * glm::inverse(localToParent);
        const mat4 worldToParent = glm::inverse(parentWorld);

        Transform next = m_StartTransform;

        if (m_DragMode == GizmoMode::Translate)
        {
            vec3 worldDelta{0.0f};
            if (m_DragHandle >= GizmoHandle::PlaneX && m_DragHandle <= GizmoHandle::PlaneZ)
            {
                vec3 hit = placement.Origin;
                if (RayPlane(ray, placement.Origin, placement.Axes[axis], hit))
                {
                    worldDelta = hit - m_GrabAnchor;
                }
            }
            else
            {
                f32 rayT = 0.0f;
                vec3 segPoint = placement.Origin;
                RaySegmentDistanceSq(ray, placement.Origin,
                                     placement.Origin + placement.Axes[axis] * placement.Scale,
                                     rayT, segPoint);
                const vec3 onLine = ray.At(rayT);
                const vec3 current =
                    placement.Origin + placement.Axes[axis] * glm::dot(onLine - placement.Origin,
                                                                       placement.Axes[axis]);
                worldDelta = current - m_GrabAnchor;
            }
            // Map the world translation delta into the parent frame and add it to the local position.
            const vec3 parentDelta = vec3(worldToParent * vec4(worldDelta, 0.0f));
            next.Position = m_StartTransform.Position + parentDelta;
        }
        else if (m_DragMode == GizmoMode::Rotate)
        {
            vec3 hit = placement.Origin;
            if (RayPlane(ray, placement.Origin, placement.Axes[axis], hit))
            {
                const vec3 u = placement.Axes[(axis + 1) % 3];
                const vec3 v = placement.Axes[(axis + 2) % 3];
                const vec3 radial = hit - placement.Origin;
                const f32 angle =
                    std::atan2(glm::dot(radial, v), glm::dot(radial, u)) - m_GrabAnchor.x;
                // The world-space rotation about the handle axis, pulled into the parent frame.
                const vec3 worldAxis = placement.Axes[axis];
                const vec3 parentAxis = glm::normalize(vec3(worldToParent * vec4(worldAxis, 0.0f)));
                const quat delta = glm::angleAxis(angle, parentAxis);
                next.Rotation = glm::normalize(delta * m_StartTransform.Rotation);
            }
        }
        else // Scale
        {
            if (m_DragHandle == GizmoHandle::Uniform)
            {
                // The cursor's signed radial motion on the camera-facing plane, scaled by the
                // gizmo size: dragging out one gizmo-radius grows by 1.0x, dragging in shrinks.
                vec3 hit = placement.Origin;
                RayPlane(ray, placement.Origin, m_DragViewNormal, hit);
                const f32 startDistance = glm::length(m_GrabAnchor - placement.Origin);
                const f32 currentDistance = glm::length(hit - placement.Origin);
                const f32 reference = glm::max(placement.Scale, 1e-4f);
                const f32 ratio =
                    glm::max(1.0f + (currentDistance - startDistance) / reference, 0.01f);
                next.Scale = m_StartTransform.Scale * ratio;
            }
            else
            {
                f32 rayT = 0.0f;
                vec3 segPoint = placement.Origin;
                RaySegmentDistanceSq(ray, placement.Origin,
                                     placement.Origin + placement.Axes[axis] * placement.Scale,
                                     rayT, segPoint);
                const vec3 onLine = ray.At(rayT);
                const f32 current = glm::dot(onLine - placement.Origin, placement.Axes[axis]);
                const f32 start = m_GrabAnchor.x;
                const f32 ratio = std::abs(start) > 1e-4f ? glm::max(current / start, 0.01f) : 1.0f;
                next.Scale[axis] = m_StartTransform.Scale[axis] * ratio;
            }
        }

        // Write back through the scene accessor (mutable Get bumps the spatial version).
        scene.Get<Transform>(entity) = next;
    }

    optional<std::pair<Transform, Transform>> EditorGizmo::EndDrag(const Scene& scene,
                                                                   const Entity entity)
    {
        if (!m_Dragging)
        {
            return std::nullopt;
        }
        m_Dragging = false;
        m_DragHandle = GizmoHandle::None;

        // The live value already holds the final Transform; the commit spans start → final.
        Transform final = m_StartTransform;
        if (!entity.IsNull() && scene.IsAlive(entity))
        {
            if (const Transform* current = scene.TryGet<Transform>(entity))
            {
                final = *current;
            }
        }
        return std::make_pair(m_StartTransform, final);
    }
}
