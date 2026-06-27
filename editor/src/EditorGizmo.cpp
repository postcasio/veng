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

        /// @brief Returns the squared distance between a ray and a finite segment, with the ray parameter on the ray.
        ///
        /// The closest-point-of-two-lines solve, clamped to the segment; used to grab an axis
        /// handle when the cursor ray passes near the axis line.
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

            f32 s = 0.0f;
            f32 t = 0.0f;
            if (denom > 1e-6f)
            {
                s = glm::clamp((bdot * f - c * e) / denom, 0.0f, 1.0f);
            }
            t = (bdot * s + f) / glm::max(e, 1e-6f);
            t = glm::clamp(t, 0.0f, 1.0f);
            s = (bdot * t + c) / glm::max(aa, 1e-6f);

            const vec3 rayPoint = ray.Origin + d1 * glm::max(s, 0.0f);
            const vec3 segPoint = a + d2 * t;
            outRayT = glm::max(s, 0.0f);
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

    void EditorGizmo::SetMode(const GizmoMode mode)
    {
        if (!m_Dragging)
        {
            m_Mode = mode;
        }
    }

    EditorGizmo::Placement EditorGizmo::ComputePlacement(const Scene& scene, const Entity entity,
                                                         const vec3 cameraPosition)
    {
        Placement placement;
        const mat4 world = WorldMatrix(scene, entity);
        placement.Origin = vec3(world[3]);
        // World-space axes: the gizmo manipulates in world space, so the handles are the world
        // basis regardless of the entity's parent frame.
        placement.Axes[0] = vec3(1.0f, 0.0f, 0.0f);
        placement.Axes[1] = vec3(0.0f, 1.0f, 0.0f);
        placement.Axes[2] = vec3(0.0f, 0.0f, 1.0f);
        const f32 distance = glm::length(cameraPosition - placement.Origin);
        placement.Scale = glm::max(distance * ScaleFactor, 0.05f);
        return placement;
    }

    GizmoHandle EditorGizmo::Pick(const Placement& placement, const Ray& ray) const
    {
        const f32 length = placement.Scale;
        const f32 tolerance = AxisPickTolerance * length;

        if (m_Mode == GizmoMode::Rotate)
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

        if (m_Mode == GizmoMode::Translate)
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

        if (m_Mode == GizmoMode::Scale)
        {
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
                           const vec3 cameraPosition) const
    {
        if (entity.IsNull() || !scene.IsAlive(entity))
        {
            return;
        }

        const Placement placement = ComputePlacement(scene, entity, cameraPosition);
        const f32 length = placement.Scale;
        const GizmoHandle active = m_Dragging ? m_DragHandle : m_Hovered;

        auto axisColor = [&](const i32 axis, const GizmoHandle handle)
        { return active == handle ? HighlightColor : AxisColor[axis]; };

        if (m_Mode == GizmoMode::Rotate)
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
                        debug.DrawLine(prev, point, color);
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
            debug.DrawLine(placement.Origin, tip, color);

            if (m_Mode == GizmoMode::Scale)
            {
                // A small box at the tip marks a scale grab.
                const f32 half = AxisPickTolerance * length * 0.9f;
                debug.DrawBox({tip - vec3(half), tip + vec3(half)}, color);
            }
        }

        if (m_Mode == GizmoMode::Translate)
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
                debug.DrawLine(a, b, color);
                debug.DrawLine(b, c, color);
                debug.DrawLine(c, d, color);
                debug.DrawLine(d, a, color);
            }
        }

        if (m_Mode == GizmoMode::Scale)
        {
            const f32 half = AxisPickTolerance * length * 1.5f;
            const vec4 color = active == GizmoHandle::Uniform ? HighlightColor : UniformColor;
            debug.DrawBox({placement.Origin - vec3(half), placement.Origin + vec3(half)}, color);
        }
    }

    bool EditorGizmo::Hover(const Scene& scene, const Entity entity, const Ray& ray,
                            const vec3 cameraPosition)
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
        const Placement placement = ComputePlacement(scene, entity, cameraPosition);
        m_Hovered = Pick(placement, ray);
        return m_Hovered != GizmoHandle::None;
    }

    bool EditorGizmo::BeginDrag(const Scene& scene, const Entity entity, const Ray& ray,
                                const vec3 cameraPosition)
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

        const Placement placement = ComputePlacement(scene, entity, cameraPosition);
        const GizmoHandle handle = Pick(placement, ray);
        if (handle == GizmoHandle::None)
        {
            return false;
        }

        m_Dragging = true;
        m_DragHandle = handle;
        m_Hovered = handle;
        m_StartTransform = *transform;
        m_DragPlacement = placement;
        m_StartWorldPosition = placement.Origin;

        const i32 axis = AxisIndex(handle);
        if (m_Mode == GizmoMode::Translate)
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
        else if (m_Mode == GizmoMode::Rotate)
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
                // Uniform scale measures the ray's distance from the pivot along the view.
                m_GrabAnchor = vec3(glm::length(ray.Origin - placement.Origin), 0.0f, 0.0f);
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

        if (m_Mode == GizmoMode::Translate)
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
        else if (m_Mode == GizmoMode::Rotate)
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
                const f32 current = glm::length(ray.Origin - placement.Origin);
                const f32 ratio =
                    m_GrabAnchor.x > 1e-4f ? glm::max(current / m_GrabAnchor.x, 0.01f) : 1.0f;
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
