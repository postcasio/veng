// The scene-gizmo layer: the stand-ins drawn for content that renders nothing.
//
// What makes this layer worth trusting is that it draws the *same* quantities the renderer
// consumes, rather than a second derivation that looks like them. The case that matters is the
// rect area light: its gizmo outline is asserted to be, point for point, the four world-space
// vertices `PackSceneLights` puts in the area-vertex buffer for the shader to integrate. So a
// light whose emitter is not where it appears to be is a disagreement the layer cannot hide, and
// a reader who sees the outline has seen the emitter.
//
// The rest is the layer's contract as a selectable set: a family draws only when it is selected,
// the group table an interface builds itself from covers every family exactly once, and nothing
// selected draws nothing at all.

#include <doctest/doctest.h>

#include <algorithm>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneGizmos.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    void RegisterBuiltins(TypeRegistry& types)
    {
        types.Register<Name>("Name");
        types.Register<Transform>("Transform");
        types.Register<Hierarchy>("Hierarchy");
        types.Register<Light>("Light");
        types.Register<Camera>("Camera");
    }

    // A rect light on a child of a rotated, translated parent — the shape a display's own light
    // takes, and the case where a gizmo recomposing the chain by hand would diverge from the packer.
    Entity AddParentedRect(Scene& scene, const quat parentRotation, const vec3 parentPosition,
                           const quat localRotation)
    {
        const Entity parent = scene.CreateEntity();
        scene.Add<Transform>(parent,
                             Transform{.Position = parentPosition, .Rotation = parentRotation});

        const Entity light = scene.CreateEntity();
        scene.Add<Transform>(light, Transform{.Rotation = localRotation});
        scene.Add<Light>(light, Light{.Type = LightType::Rect,
                                      .Range = 4.0f,
                                      .Width = 1.5f,
                                      .Height = 0.33f,
                                      .TwoSided = true});
        scene.SetParent(light, parent);
        return light;
    }

    // Whether some drawn line has both of these endpoints, in either direction.
    bool HasSegment(const DebugDraw& debug, const vec3 a, const vec3 b)
    {
        const vector<DebugLineVertex>& verts = debug.GetLineVertices();
        for (usize i = 0; i + 1 < verts.size(); i += 2)
        {
            const bool forward = glm::all(glm::epsilonEqual(verts[i].Position, a, 1e-4f)) &&
                                 glm::all(glm::epsilonEqual(verts[i + 1].Position, b, 1e-4f));
            const bool backward = glm::all(glm::epsilonEqual(verts[i].Position, b, 1e-4f)) &&
                                  glm::all(glm::epsilonEqual(verts[i + 1].Position, a, 1e-4f));
            if (forward || backward)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("scene gizmos: a rect light's outline is the quad the packer hands the shader")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    // A parent turned well off every axis and a child turned again, so an outline that dropped or
    // reordered a factor of the chain cannot coincide with the right answer.
    AddParentedRect(*scene, glm::angleAxis(0.7f, glm::normalize(vec3(0.3f, 1.0f, -0.4f))),
                    vec3(3.0f, -1.5f, 12.0f),
                    glm::angleAxis(-glm::half_pi<f32>(), vec3(1.0f, 0.0f, 0.0f)));

    const PackedSceneLights packed = PackSceneLights(*scene, false, 1024);
    REQUIRE(packed.LightCount == 1);
    REQUIRE(packed.AreaVertexCount == 4);

    DebugDraw debug;
    DrawSceneGizmos(*scene, debug, SceneGizmo::Lights, SceneGizmoStyle{});

    // Every edge of the packed quad is drawn, so the outline encloses exactly the emitter.
    for (u32 i = 0; i < 4; ++i)
    {
        const vec3 a = vec3(packed.AreaVertices[i]);
        const vec3 b = vec3(packed.AreaVertices[(i + 1) % 4]);
        CAPTURE(i);
        CHECK(HasSegment(debug, a, b));
    }

    // And the normal stub runs along the packed area normal from the packed position — the axis the
    // shader's front/back test uses, drawn both ways because the light is two-sided.
    const vec3 position = vec3(packed.Lights[0].PositionRange);
    const vec3 normal = vec3(packed.Lights[0].AreaNormal);
    CHECK(HasSegment(debug, position, position + normal * 0.5f));
    CHECK(HasSegment(debug, position, position - normal * 0.5f));
}

TEST_CASE("scene gizmos: a family draws only when it is selected")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    const Entity camera = scene->CreateEntity();
    scene->Add<Transform>(camera, Transform{});
    scene->Add<Camera>(camera, Camera{});
    AddParentedRect(*scene, quat{1.0f, 0.0f, 0.0f, 0.0f}, vec3(0.0f), quat{1.0f, 0.0f, 0.0f, 0.0f});

    // Nothing selected is the default, and it must cost nothing rather than draw everything.
    DebugDraw none;
    DrawSceneGizmos(*scene, none, SceneGizmo::None, SceneGizmoStyle{});
    CHECK(none.IsEmpty());

    // One family selected draws that family and no other: the camera's frustum has twelve edges
    // and the rect's outline four plus two stubs, so the two are told apart by count.
    DebugDraw lights;
    DrawSceneGizmos(*scene, lights, SceneGizmo::Lights, SceneGizmoStyle{});
    DebugDraw cameras;
    DrawSceneGizmos(*scene, cameras, SceneGizmo::Cameras, SceneGizmoStyle{});
    DebugDraw both;
    DrawSceneGizmos(*scene, both, SceneGizmo::Lights | SceneGizmo::Cameras, SceneGizmoStyle{});

    CHECK_FALSE(lights.IsEmpty());
    CHECK_FALSE(cameras.IsEmpty());
    CHECK(both.GetLineVertices().size() ==
          lights.GetLineVertices().size() + cameras.GetLineVertices().size());
}

TEST_CASE("scene gizmos: the group table covers every family exactly once")
{
    const std::span<const SceneGizmoGroupInfo> groups = SceneGizmoGroupTable();
    REQUIRE_FALSE(groups.empty());

    // A consumer builds its selection interface from this table, so a family missing from it is a
    // family no interface can ever reach — invisible content the layer silently declines to show.
    SceneGizmo union_ = SceneGizmo::None;
    for (const SceneGizmoGroupInfo& group : groups)
    {
        CHECK(group.Bit != SceneGizmo::None);
        CHECK_FALSE(HasGizmo(union_, group.Bit)); // no family listed twice
        union_ = union_ | group.Bit;
        CHECK(group.Name[0] != '\0');
        CHECK(group.Description[0] != '\0');
    }
    CHECK(union_ == SceneGizmo::All);
}
