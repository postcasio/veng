#include <Veng/Renderer/SceneGizmos.h>

#include <array>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Asset/CollisionShape.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Audio/AudioComponents.h>
#include <Veng/Physics/Components.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Interaction.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // One hue per family, so a crowded scene is read by colour before it is read by shape. They
        // are deliberately far apart in hue rather than pleasant together: this layer is an
        // instrument, and two families a glance cannot separate is the only way it fails.
        constexpr vec4 LightColor{1.0f, 0.85f, 0.4f, 1.0f};
        constexpr vec4 CameraColor{0.5f, 0.8f, 1.0f, 1.0f};
        constexpr vec4 ColliderColor{0.35f, 1.0f, 0.5f, 1.0f};
        constexpr vec4 SensorColor{1.0f, 0.6f, 0.2f, 1.0f};
        constexpr vec4 SocketColor{1.0f, 1.0f, 1.0f, 1.0f};
        constexpr vec4 InteractionColor{1.0f, 0.4f, 0.8f, 1.0f};
        constexpr vec4 InteractionDisabledColor{0.5f, 0.2f, 0.4f, 1.0f};
        constexpr vec4 AudioColor{0.6f, 0.5f, 1.0f, 1.0f};
        constexpr vec4 AudioOuterColor{0.35f, 0.3f, 0.7f, 1.0f};
        constexpr vec4 ProbeColor{0.3f, 1.0f, 1.0f, 1.0f};
        constexpr vec4 EmptyColor{0.6f, 0.6f, 0.6f, 1.0f};

        // The families, in the order an interface offers them: the two an author places by hand
        // first, then the derived and the diagnostic.
        constexpr std::array<SceneGizmoGroupInfo, 8> Groups{{
            {.Bit = SceneGizmo::Lights,
             .Name = "Lights",
             .Description = "Icon and emitting volume for every light"},
            {.Bit = SceneGizmo::Cameras,
             .Name = "Cameras",
             .Description = "Icon and view frustum for every camera"},
            {.Bit = SceneGizmo::Colliders,
             .Name = "Colliders",
             .Description = "Physics shapes; sensors in their own colour"},
            {.Bit = SceneGizmo::Sockets,
             .Name = "Sockets",
             .Description = "Every named socket on every resident mesh"},
            {.Bit = SceneGizmo::Interaction,
             .Name = "Interaction",
             .Description = "Each interactable's range sphere"},
            {.Bit = SceneGizmo::Audio,
             .Name = "Audio",
             .Description = "Spatial sources' falloff, and the listener"},
            {.Bit = SceneGizmo::Probes,
             .Name = "Probes",
             .Description = "Capture surfaces and the axes they capture along"},
            {.Bit = SceneGizmo::Empties,
             .Name = "Empties",
             .Description = "Entities that draw nothing at all"},
        }};

        // A world matrix's three basis vectors, scale included — what an oriented wireframe is
        // built from, and what a triad shows.
        struct Basis
        {
            vec3 Origin{0.0f};
            vec3 X{1.0f, 0.0f, 0.0f};
            vec3 Y{0.0f, 1.0f, 0.0f};
            vec3 Z{0.0f, 0.0f, 1.0f};
        };

        [[nodiscard]] Basis BasisOf(const mat4& world)
        {
            return Basis{.Origin = vec3(world[3]),
                         .X = vec3(world[0]),
                         .Y = vec3(world[1]),
                         .Z = vec3(world[2])};
        }

        // Three axes from a world transform, at a fixed world size rather than the transform's own
        // scale: a socket's triad says which way it faces, and a parent's scale would make that
        // unreadable rather than informative.
        void DrawTriad(DebugDraw& debug, const mat4& world, const f32 size, const vec4 color)
        {
            const Basis basis = BasisOf(world);
            const auto axis = [&](const vec3 v)
            { return glm::length(v) > 1e-6f ? glm::normalize(v) * size : vec3(0.0f); };
            debug.DrawLine(basis.Origin, basis.Origin + axis(basis.X),
                           vec4(1.0f, 0.25f, 0.25f, color.a));
            debug.DrawLine(basis.Origin, basis.Origin + axis(basis.Y),
                           vec4(0.25f, 1.0f, 0.25f, color.a));
            debug.DrawLine(basis.Origin, basis.Origin + axis(basis.Z),
                           vec4(0.4f, 0.5f, 1.0f, color.a));
        }

        // The wireframe stand-in for a billboard a consumer supplied no icon for: a small
        // camera-agnostic star, which reads as a marker from any direction where a flat cross does
        // not.
        void DrawMarker(DebugDraw& debug, const vec3 position, const f32 size, const vec4 color)
        {
            for (const vec3 axis :
                 {vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f)})
            {
                debug.DrawLine(position - axis * size, position + axis * size, color);
            }
        }

        // An icon where there is one, the wireframe marker where there is not — so a family reads
        // the same way whether or not the consumer ships art for it.
        void DrawIcon(DebugDraw& debug, const vec3 position, const TextureHandle icon,
                      const vec4 color, const SceneGizmoStyle& style, const u32 pickId)
        {
            if (icon.IsValid())
            {
                debug.DrawBillboard(position, style.IconSize, icon, color, pickId);
                return;
            }
            DrawMarker(debug, position, style.MarkerSize, color);
        }

        // Twelve edges of a box in an arbitrary frame — an oriented box, which DebugDraw::DrawBox
        // cannot express: it takes a world AABB, and a rotated collider fitted to one would draw a
        // volume the solver never sees.
        void DrawOrientedBox(DebugDraw& debug, const mat4& world, const vec3 center,
                             const vec3 halfExtents, const vec4 color)
        {
            std::array<vec3, 8> corners{};
            for (u32 i = 0; i < 8; ++i)
            {
                const vec3 sign{(i & 1u) != 0u ? 1.0f : -1.0f, (i & 2u) != 0u ? 1.0f : -1.0f,
                                (i & 4u) != 0u ? 1.0f : -1.0f};
                corners[i] = vec3(world * vec4(center + sign * halfExtents, 1.0f));
            }
            // Pairs differing in exactly one bit are the box's edges.
            for (u32 a = 0; a < 8; ++a)
            {
                for (const u32 bit : {1u, 2u, 4u})
                {
                    if ((a & bit) == 0u)
                    {
                        debug.DrawLine(corners[a], corners[a | bit], color);
                    }
                }
            }
        }

        // A capsule as its two end spheres plus the four lines joining them: enough to read the
        // radius, the length and the axis, which is what a capsule collider is.
        void DrawCapsule(DebugDraw& debug, const mat4& world, const vec3 center, const f32 radius,
                         const f32 halfHeight, const vec4 color)
        {
            const Basis basis = BasisOf(world);
            const vec3 up = glm::length(basis.Y) > 1e-6f ? glm::normalize(basis.Y) : vec3(0, 1, 0);
            const vec3 right =
                glm::length(basis.X) > 1e-6f ? glm::normalize(basis.X) : vec3(1, 0, 0);
            const vec3 fwd = glm::length(basis.Z) > 1e-6f ? glm::normalize(basis.Z) : vec3(0, 0, 1);
            const vec3 mid = vec3(world * vec4(center, 1.0f));
            const vec3 top = mid + up * halfHeight;
            const vec3 bottom = mid - up * halfHeight;
            debug.DrawSphere(top, radius, color);
            debug.DrawSphere(bottom, radius, color);
            for (const vec3 side : {right, -right, fwd, -fwd})
            {
                debug.DrawLine(bottom + side * radius, top + side * radius, color);
            }
        }

        // Cooked geometry in a frame: a mesh's triangle edges, or a hull's points as crosses. The
        // only wireframe a convex hull or triangle-mesh collider has here — the render mesh's AABB
        // is not the collider.
        void DrawWireGeometry(DebugDraw& debug, const mat4& frame,
                              const std::span<const vec3> points,
                              const std::span<const u32> indices, const bool triangleMesh,
                              const vec4 color)
        {
            const auto at = [&](const vec3 p) { return vec3(frame * vec4(p, 1.0f)); };
            if (triangleMesh)
            {
                for (usize i = 0; i + 2 < indices.size(); i += 3)
                {
                    const vec3 a = at(points[indices[i]]);
                    const vec3 b = at(points[indices[i + 1]]);
                    const vec3 c = at(points[indices[i + 2]]);
                    debug.DrawLine(a, b, color);
                    debug.DrawLine(b, c, color);
                    debug.DrawLine(c, a, color);
                }
                return;
            }
            for (const vec3 point : points)
            {
                const vec3 world = at(point);
                debug.DrawLine(world - vec3(0.05f, 0.0f, 0.0f), world + vec3(0.05f, 0.0f, 0.0f),
                               color);
                debug.DrawLine(world - vec3(0.0f, 0.05f, 0.0f), world + vec3(0.0f, 0.05f, 0.0f),
                               color);
                debug.DrawLine(world - vec3(0.0f, 0.0f, 0.05f), world + vec3(0.0f, 0.0f, 0.05f),
                               color);
            }
        }

        // One compound child drawn at its composed pose within the collider frame.
        void DrawCollisionChild(DebugDraw& debug, const mat4& frame, const CollisionChild& child,
                                const vec4 color)
        {
            const mat4 childFrame =
                frame * glm::translate(mat4(1.0f), child.Offset) * glm::mat4_cast(child.Rotation);
            switch (child.Kind)
            {
            case CollisionChildKind::Box:
                DrawOrientedBox(debug, childFrame, vec3(0.0f), child.Extents, color);
                break;
            case CollisionChildKind::Sphere:
                debug.DrawSphere(vec3(childFrame[3]), child.Extents.x, color);
                break;
            case CollisionChildKind::Capsule:
                DrawCapsule(debug, childFrame, vec3(0.0f), child.Extents.x, child.Extents.y, color);
                break;
            case CollisionChildKind::Convex:
                DrawWireGeometry(debug, childFrame, child.Points, {}, false, color);
                break;
            case CollisionChildKind::Mesh:
                DrawWireGeometry(debug, childFrame, child.Points, child.Indices, true, color);
                break;
            }
        }

        void DrawLightGizmo(const Scene& scene, DebugDraw& debug, const Entity entity,
                            const Light& light, const SceneGizmoStyle& style)
        {
            const mat4 world = WorldMatrix(scene, entity);
            const vec3 position = vec3(world[3]);
            DrawIcon(debug, position, style.LightIcon, LightColor, style,
                     style.Pickable ? entity.Index + 1u : 0u);

            const vec3 axis = glm::length(light.Direction) > 0.0f ? glm::normalize(light.Direction)
                                                                  : vec3(0.0f, -1.0f, 0.0f);
            switch (light.Type)
            {
            case LightType::Point:
                debug.DrawSphere(position, light.Range, LightColor);
                break;
            case LightType::Spot:
            {
                // The cone at the falloff range: a ring plus four edges back to the apex.
                const f32 coneRadius = light.Range * std::tan(light.OuterCone);
                const vec3 center = position + axis * light.Range;
                const vec3 up =
                    std::abs(axis.y) < 0.99f ? vec3(0.0f, 1.0f, 0.0f) : vec3(1.0f, 0.0f, 0.0f);
                const vec3 right = glm::normalize(glm::cross(axis, up));
                const vec3 bitangent = glm::cross(axis, right);
                constexpr u32 segments = 24;
                vec3 prev{};
                for (u32 i = 0; i <= segments; ++i)
                {
                    const f32 a =
                        glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(segments);
                    const vec3 point =
                        center + coneRadius * (std::cos(a) * right + std::sin(a) * bitangent);
                    if (i > 0)
                    {
                        debug.DrawLine(prev, point, LightColor);
                    }
                    if (i % (segments / 4) == 0)
                    {
                        debug.DrawLine(position, point, LightColor);
                    }
                    prev = point;
                }
                break;
            }
            case LightType::Directional:
                // No position-bound volume; a short arrow is the whole of what there is to show.
                debug.DrawLine(position, position + axis * 1.5f, LightColor);
                break;
            case LightType::Rect:
            {
                // The emitting quad itself, built exactly as PackSceneLights builds it — the four
                // corners of Width × Height in local XY through the world matrix — plus a stub
                // along the emitting normal, local +Z. Drawing the light's own packed geometry is
                // the point: a rect that emits somewhere other than where it appears to is a
                // disagreement between this quad and the shaded result, and nothing else shows it.
                const f32 hw = light.Width * 0.5f;
                const f32 hh = light.Height * 0.5f;
                const vec3 c0 = vec3(world * vec4(-hw, -hh, 0.0f, 1.0f));
                const vec3 c1 = vec3(world * vec4(hw, -hh, 0.0f, 1.0f));
                const vec3 c2 = vec3(world * vec4(hw, hh, 0.0f, 1.0f));
                const vec3 c3 = vec3(world * vec4(-hw, hh, 0.0f, 1.0f));
                debug.DrawLine(c0, c1, LightColor);
                debug.DrawLine(c1, c2, LightColor);
                debug.DrawLine(c2, c3, LightColor);
                debug.DrawLine(c3, c0, LightColor);
                const vec3 n = glm::normalize(vec3(world[2]));
                debug.DrawLine(position, position + n * 0.5f, LightColor);
                // Two-sided emission is a fact about the light that its outline cannot carry, so
                // the stub is mirrored when it holds.
                if (light.TwoSided)
                {
                    debug.DrawLine(position, position - n * 0.5f, LightColor);
                }
                break;
            }
            case LightType::Sphere:
                debug.DrawSphere(position, light.Radius, LightColor);
                break;
            case LightType::Polygon:
            {
                const usize count = light.PolygonVertices.size();
                for (usize i = 0; i < count; ++i)
                {
                    const vec3 a = vec3(world * vec4(light.PolygonVertices[i], 1.0f));
                    const vec3 b = vec3(world * vec4(light.PolygonVertices[(i + 1) % count], 1.0f));
                    debug.DrawLine(a, b, LightColor);
                }
                break;
            }
            }
        }
    }

    std::span<const SceneGizmoGroupInfo> SceneGizmoGroupTable()
    {
        return Groups;
    }

    void DrawSceneGizmos(const Scene& scene, DebugDraw& debug, const SceneGizmo groups,
                         const SceneGizmoStyle& style)
    {
        if (groups == SceneGizmo::None)
        {
            return;
        }

        if (HasGizmo(groups, SceneGizmo::Lights))
        {
            for (auto [entity, transform, light] : scene.View<Transform, Light>())
            {
                DrawLightGizmo(scene, debug, entity, light, style);
            }
        }

        if (HasGizmo(groups, SceneGizmo::Cameras))
        {
            for (auto [entity, transform, camera] : scene.View<Transform, Camera>())
            {
                const mat4 world = WorldMatrix(scene, entity);
                DrawIcon(debug, vec3(world[3]), style.CameraIcon, CameraColor, style,
                         style.Pickable ? entity.Index + 1u : 0u);
                // The far plane is pulled in: a frustum drawn to a real far plane is a pair of
                // lines vanishing off the scene, which says nothing about where the camera looks.
                Camera gizmo = camera;
                gizmo.Far = glm::min(camera.Far, camera.Near + 5.0f);
                const CameraView view = MakeCameraView(gizmo, 16.0f / 9.0f, world);
                debug.DrawFrustum(glm::inverse(view.ViewProjection()), CameraColor);
            }
        }

        if (HasGizmo(groups, SceneGizmo::Colliders))
        {
            for (auto [entity, transform, collider] : scene.View<Transform, Collider>())
            {
                const mat4 world = WorldMatrix(scene, entity);
                const vec4 color = scene.Has<Sensor>(entity) ? SensorColor : ColliderColor;
                // The shape's own local pose composes under the entity transform: rotate by
                // Rotation, then offset by Offset, the frame the solver's RotatedTranslatedShape
                // builds. A primitive draws at this frame's origin so a rotated collider is drawn
                // where the solver puts it, not in its unrotated frame.
                const mat4 frame = world * glm::translate(mat4(1.0f), collider.Offset) *
                                   glm::mat4_cast(collider.Rotation);
                switch (collider.Shape)
                {
                case ColliderShape::Box:
                    DrawOrientedBox(debug, frame, vec3(0.0f), collider.Extents, color);
                    break;
                case ColliderShape::Sphere:
                    debug.DrawSphere(vec3(frame[3]), collider.Extents.x, color);
                    break;
                case ColliderShape::Capsule:
                    DrawCapsule(debug, frame, vec3(0.0f), collider.Extents.x, collider.Extents.y,
                                color);
                    break;
                case ColliderShape::Mesh:
                {
                    // Cooked geometry has its real wireframe: a compound's children each at their
                    // composed pose, a convex hull's points as crosses, a triangle mesh's edges.
                    // The render mesh's AABB is not the collider, so it is only the fallback for an
                    // unresident handle.
                    if (const CollisionShape* const geometry = collider.Geometry.Get();
                        geometry != nullptr)
                    {
                        if (geometry->Geometry == CollisionGeometry::Compound)
                        {
                            for (const CollisionChild& child : geometry->Children)
                            {
                                DrawCollisionChild(debug, frame, child, color);
                            }
                        }
                        else
                        {
                            DrawWireGeometry(debug, frame, geometry->Points, geometry->Indices,
                                             geometry->Geometry == CollisionGeometry::Mesh, color);
                        }
                    }
                    else if (const auto* const renderer = scene.TryGet<MeshRenderer>(entity);
                             renderer != nullptr && renderer->Mesh.IsLoaded())
                    {
                        // The render mesh's bounds are in the entity frame, not the collider's
                        // offset/rotation frame, so this stand-in draws through world.
                        const AABB& bounds = renderer->Mesh.Get()->GetBounds();
                        DrawOrientedBox(debug, world, bounds.Center(), bounds.Extents(), color);
                    }
                    break;
                }
                }
            }
        }

        if (HasGizmo(groups, SceneGizmo::Sockets))
        {
            for (auto [entity, transform, renderer] : scene.View<Transform, MeshRenderer>())
            {
                if (!renderer.Mesh.IsLoaded())
                {
                    continue;
                }
                const mat4 world = WorldMatrix(scene, entity);
                for (const MeshSocket& socket : renderer.Mesh.Get()->GetSockets())
                {
                    const mat4 local = glm::translate(mat4(1.0f), socket.Position) *
                                       glm::mat4_cast(socket.Rotation);
                    DrawTriad(debug, world * local, style.MarkerSize * 2.0f, SocketColor);
                }
            }
        }

        if (HasGizmo(groups, SceneGizmo::Interaction))
        {
            for (auto [entity, transform, interactable] : scene.View<Transform, Interactable>())
            {
                const mat4 world = WorldMatrix(scene, entity);
                debug.DrawSphere(vec3(world[3]), interactable.Range,
                                 interactable.Enabled ? InteractionColor
                                                      : InteractionDisabledColor);
            }
        }

        if (HasGizmo(groups, SceneGizmo::Audio))
        {
            for (auto [entity, transform, source] : scene.View<Transform, AudioSource>())
            {
                const mat4 world = WorldMatrix(scene, entity);
                const vec3 position = vec3(world[3]);
                DrawMarker(debug, position, style.MarkerSize, AudioColor);
                // A non-spatial source has no falloff to draw: it routes straight to its bus, so a
                // radius around it would describe a rule it does not obey.
                if (source.Spatial)
                {
                    debug.DrawSphere(position, source.MinDistance, AudioColor);
                    debug.DrawSphere(position, source.MaxDistance, AudioOuterColor);
                }
            }
            for (auto [entity, transform, listener] : scene.View<Transform, AudioListener>())
            {
                DrawTriad(debug, WorldMatrix(scene, entity), style.MarkerSize * 3.0f, AudioColor);
            }
        }

        if (HasGizmo(groups, SceneGizmo::Probes))
        {
            for (auto [entity, transform, capture] : scene.View<Transform, CaptureSurface>())
            {
                const mat4 world = WorldMatrix(scene, entity);
                DrawMarker(debug, vec3(world[3]), style.MarkerSize, ProbeColor);
                // The axes the faces are captured along, which is what the surface's alignment
                // decides and what a material sampling it has to agree with.
                DrawTriad(debug, world, style.MarkerSize * 3.0f, ProbeColor);
            }
        }

        if (HasGizmo(groups, SceneGizmo::Empties))
        {
            for (auto [entity, transform] : scene.View<Transform>())
            {
                // "Draws nothing" is the whole definition: an entity already shown by another
                // family is not an empty, and one carrying a mesh is not invisible.
                if (scene.Has<MeshRenderer>(entity) || scene.Has<Light>(entity) ||
                    scene.Has<Camera>(entity) || scene.Has<Collider>(entity) ||
                    scene.Has<AudioSource>(entity) || scene.Has<CaptureSurface>(entity))
                {
                    continue;
                }
                DrawTriad(debug, WorldMatrix(scene, entity), style.MarkerSize, EmptyColor);
            }
        }
    }
}
