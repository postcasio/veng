// Level-of-detail selection: the screen-size metric a view measures a mesh by, the threshold walk
// with its hysteresis band, and the forced-level precedence. Device-free — the meshes carry bounds
// and level thresholds only, never a buffer.

#include <doctest/doctest.h>

#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Visibility.h>

#include "Renderer/LodSelection.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr f32 Thresholds[] = {0.5f, 0.25f, 0.1f};

    // A mesh with a unit-radius bounding sphere and three coarser levels, one submesh each.
    Ref<Mesh> LeveledMesh()
    {
        const f32 half = 1.0f / std::sqrt(3.0f);
        const AABB box{.Min = vec3(-half), .Max = vec3(half)};
        vector<MeshLod> lods;
        for (const f32 threshold : Thresholds)
        {
            lods.push_back(MeshLod{.ScreenSize = threshold, .Ranges = {MeshLodRange{}}});
        }
        return Mesh::Create(MeshInfo{
            .Name = "leveled", .SubMeshes = {SubMesh{.Bounds = box}}, .Bounds = box, .Lods = lods});
    }

    VisibleMesh Placed(const Mesh& mesh, const vec3& position)
    {
        return VisibleMesh{
            .World = glm::translate(mat4(1.0f), position),
            .Mesh = &mesh,
            .CastsShadows = true,
        };
    }

    mat4 PerspectiveLookingAt(const vec3& target)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
        camera.SetView(vec3(0.0f), target, vec3(0.0f, 1.0f, 0.0f));
        return camera.ViewProjection();
    }
}

TEST_CASE("lod selection: the level coarsens monotonically as the screen size shrinks")
{
    // Every previous level, and none: a smaller size never selects a finer level.
    const u32 previousLevels[] = {NoPreviousLod, 0, 1, 2, 3};
    u32 reversals = 0;
    u32 reached = 0;
    for (const u32 previous : previousLevels)
    {
        u32 last = 0;
        for (f32 size = 2.0f; size > 0.01f; size *= 0.97f)
        {
            const u32 level = SelectLod(Thresholds, size, previous, 0.1f);
            reversals += level < last ? 1 : 0;
            reached |= 1u << level;
            last = level;
        }
    }
    CHECK(reversals == 0);
    CHECK(reached == 0b1111u);

    // With no history the thresholds themselves decide.
    CHECK(SelectLod(Thresholds, 0.6f, NoPreviousLod, 0.1f) == 0);
    CHECK(SelectLod(Thresholds, 0.3f, NoPreviousLod, 0.1f) == 1);
    CHECK(SelectLod(Thresholds, 0.2f, NoPreviousLod, 0.1f) == 2);
    CHECK(SelectLod(Thresholds, 0.05f, NoPreviousLod, 0.1f) == 3);
}

TEST_CASE("lod selection: a size inside a threshold's band keeps the level it had")
{
    constexpr f32 hysteresis = 0.1f;
    // The band around the 1 → 2 threshold (0.25) is [0.225, 0.275]: either level holds inside it.
    for (const f32 size : {0.226f, 0.24f, 0.25f, 0.26f, 0.274f})
    {
        CHECK(SelectLod(Thresholds, size, 1, hysteresis) == 1);
        CHECK(SelectLod(Thresholds, size, 2, hysteresis) == 2);
    }
    // Outside it the level moves, by as many levels as the size crossed.
    CHECK(SelectLod(Thresholds, 0.22f, 1, hysteresis) == 2);
    CHECK(SelectLod(Thresholds, 0.28f, 2, hysteresis) == 1);
    CHECK(SelectLod(Thresholds, 0.05f, 0, hysteresis) == 3);
    CHECK(SelectLod(Thresholds, 0.9f, 3, hysteresis) == 0);

    // A size oscillating across a threshold by less than the band does not flicker.
    u32 level = SelectLod(Thresholds, 0.255f, NoPreviousLod, hysteresis);
    u32 flips = 0;
    for (u32 frame = 0; frame < 32; ++frame)
    {
        const f32 size = frame % 2 == 0 ? 0.24f : 0.26f;
        const u32 next = SelectLod(Thresholds, size, level, hysteresis);
        flips += next != level ? 1 : 0;
        level = next;
    }
    CHECK(flips == 0);
}

TEST_CASE("lod selection: screen size follows distance and projection, not the view's direction")
{
    // Perspective: the same distance in any direction measures the same; twice as far, half.
    const vec3 ahead(0.0f, 0.0f, -10.0f);
    const LodView forward = LodView::FromViewProjection(PerspectiveLookingAt(ahead), 0.0f);
    const LodView aside = LodView::FromViewProjection(PerspectiveLookingAt(vec3(1, 0, 0)), 0.0f);
    const f32 straight = ProjectedScreenSize(forward, ahead, 1.0f);
    CHECK(ProjectedScreenSize(aside, ahead, 1.0f) == doctest::Approx(straight));
    CHECK(ProjectedScreenSize(forward, ahead * 2.0f, 1.0f) == doctest::Approx(straight * 0.5f));
    // A unit sphere at 10 under a 60-degree view is 2 / (2 · 10 · tan 30°) of the view height.
    CHECK(straight == doctest::Approx(1.0f / (10.0f * std::tan(glm::radians(30.0f)))));
    // A sphere around the eye is as large as a size gets.
    CHECK(ProjectedScreenSize(forward, vec3(0.5f, 0.0f, 0.0f), 1.0f) ==
          std::numeric_limits<f32>::max());

    // Orthographic: the view height alone decides, at any distance.
    CameraView ortho;
    ortho.SetOrthographic(5.0f, 5.0f, 0.1f, 100.0f);
    ortho.SetView(vec3(0.0f), ahead, vec3(0.0f, 1.0f, 0.0f));
    const LodView flat = LodView::FromViewProjection(ortho.ViewProjection(), 0.0f);
    CHECK(ProjectedScreenSize(flat, ahead, 1.0f) == doctest::Approx(0.2f));
    CHECK(ProjectedScreenSize(flat, ahead * 5.0f, 1.0f) == doctest::Approx(0.2f));

    // Each level of bias halves the size.
    const LodView biased = LodView::FromViewProjection(PerspectiveLookingAt(ahead), 1.0f);
    CHECK(ProjectedScreenSize(biased, ahead, 1.0f) == doctest::Approx(straight * 0.5f));
}

TEST_CASE("lod selection: a forced level wins, the entity's over the view's, clamped to the mesh")
{
    const Ref<Mesh> mesh = LeveledMesh();
    const LodView view = LodView::FromViewProjection(PerspectiveLookingAt(vec3(0, 0, -1)), 0.0f);

    // Near enough that selection picks level 0.
    VisibleMesh near = Placed(*mesh, vec3(0.0f, 0.0f, -2.0f));
    CHECK(ResolveMeshLod(near, view, LodPolicy{}, NoPreviousLod) == 0);

    CHECK(ResolveMeshLod(near, view, LodPolicy{.Forced = 2}, NoPreviousLod) == 2);
    CHECK(ResolveMeshLod(near, view, LodPolicy{.Forced = CoarsestLod}, NoPreviousLod) == 3);

    near.ForcedLod = 1;
    CHECK(ResolveMeshLod(near, view, LodPolicy{.Forced = 2}, NoPreviousLod) == 1);
    near.ForcedLod = 99;
    CHECK(ResolveMeshLod(near, view, LodPolicy{}, NoPreviousLod) == 3);

    // Far away the automatic selection reaches the coarsest, and a forced 0 still draws full detail.
    const VisibleMesh far = Placed(*mesh, vec3(0.0f, 0.0f, -500.0f));
    CHECK(ResolveMeshLod(far, view, LodPolicy{}, NoPreviousLod) == 3);
    CHECK(ResolveMeshLod(far, view, LodPolicy{.Forced = 0}, NoPreviousLod) == 0);

    // A single-level mesh is level 0 whatever is forced.
    const AABB box{.Min = vec3(-1.0f), .Max = vec3(1.0f)};
    const Ref<Mesh> single = Mesh::Create(
        MeshInfo{.Name = "single", .SubMeshes = {SubMesh{.Bounds = box}}, .Bounds = box});
    CHECK(ResolveMeshLod(Placed(*single, vec3(0, 0, -500)), view, LodPolicy{.Forced = 3},
                         NoPreviousLod) == 0);
}
