// The device-free half of instanced mesh sets: the instance store's write tracking and bound, and
// the per-instance cull the host fallback runs (which the GPU cull mirrors).

#include <array>
#include <limits>

#include <doctest/doctest.h>

#include <Veng/Renderer/InstancedMesh.h>

#include <Renderer/InstancedCull.h>

// After the Veng headers, so Veng.h's GLM_FORCE_DEPTH_ZERO_TO_ONE is set before glm is first
// included.
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A unit cube's bound, the mesh every case places.
    const AABB UnitCube{.Min = vec3(-0.5f), .Max = vec3(0.5f)};

    // A 60-degree camera at the origin looking down -Z, square, near 0.1 / far 1000.
    mat4 CameraViewProj()
    {
        return glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f) *
               glm::lookAt(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
    }

    InstancedCullView MainView(const f32 hysteresis = 0.0f)
    {
        const mat4 viewProj = CameraViewProj();
        return InstancedCullView{
            .Planes = Frustum::FromViewProjection(viewProj),
            .FrustumCull = true,
            .Main = true,
            .Lod = LodView::FromViewProjection(viewProj, 0.0f),
            .Hysteresis = hysteresis,
        };
    }

    // The size band's open upper end.
    constexpr f32 Unbounded = std::numeric_limits<f32>::max();

    InstancedCullReference Reference()
    {
        return {.ViewProj = CameraViewProj(), .Eye = vec3(0.0f), .HeightPixels = 1000.0f};
    }
}

TEST_CASE("instance set: writes are uploaded by the ranges they touched")
{
    const Ref<InstanceSet> set = InstanceSet::Create(100);
    REQUIRE(set->GetCount() == 100);
    const u64 synced = set->GetWriteSerial();
    CHECK(set->GetWrittenSince(synced).End == 0);

    const std::array<MeshInstance, 3> moved{};
    set->Write(10, moved);
    set->Write(40, std::span(moved).first(2));
    const InstanceSet::Range written = set->GetWrittenSince(synced);
    CHECK(written.First == 10);
    CHECK(written.End == 42);

    // A reader caught up after the first write sees only the second.
    const u64 afterBoth = set->GetWriteSerial();
    CHECK(set->GetWrittenSince(afterBoth - 1).First == 40);

    // A reader further behind than the tracked writes, or behind a capacity change, uploads all.
    for (u32 i = 0; i < InstanceSet::MaxTrackedWrites; ++i)
    {
        set->Write(i, std::span(moved).first(1));
    }
    CHECK(set->GetWrittenSince(synced).End == 100);
    const u64 beforeResize = set->GetWriteSerial();
    set->Resize(120);
    CHECK(set->GetWrittenSince(beforeResize).End == 120);
    CHECK(set->GetCount() == 100);
}

TEST_CASE("instance set: the bound holds every drawn instance's sphere")
{
    const Ref<InstanceSet> set = InstanceSet::Create(0);
    set->Resize(4);
    const std::array<MeshInstance, 4> instances{
        MeshInstance::Make(vec3(10.0f, 0.0f, 0.0f), quat(1.0f, 0.0f, 0.0f, 0.0f), 2.0f),
        MeshInstance::Make(vec3(-5.0f, 3.0f, 0.0f), glm::angleAxis(1.0f, vec3(0.0f, 1.0f, 0.0f)),
                           1.0f),
        MeshInstance::Make(vec3(0.0f, 0.0f, 7.0f), quat(1.0f, 0.0f, 0.0f, 0.0f), 0.5f),
        MeshInstance::Make(vec3(0.0f, -9.0f, 0.0f), quat(1.0f, 0.0f, 0.0f, 0.0f), 3.0f),
    };
    set->Write(0, instances);
    set->SetCount(4);

    const AABB offset{.Min = vec3(0.0f), .Max = vec3(1.0f)};
    const AABB bound = set->GetLocalBounds(offset);
    bool contained = true;
    for (const MeshInstance& instance : instances)
    {
        vec3 center;
        f32 radius = 0.0f;
        InstanceBoundingSphere(instance.GetMatrix(), offset, center, radius);
        contained = contained && glm::all(glm::lessThanEqual(bound.Min, center - vec3(radius))) &&
                    glm::all(glm::greaterThanEqual(bound.Max, center + vec3(radius)));
    }
    CHECK(contained);

    set->SetCount(0);
    CHECK(set->GetLocalBounds(offset).IsEmpty());
}

TEST_CASE("instanced cull: the frustum keeps what is ahead and culls what is behind")
{
    const InstancedCullSet set{.MeshBounds = UnitCube, .MaxPixels = Unbounded};
    const InstancedCullView view = MainView();
    const auto Drawn = [&](const vec3& at)
    {
        return CullInstance(MeshInstance::Make(at, quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f), set, view,
                            Reference(), InstanceNoPreviousLod)
            .Drawn;
    };
    CHECK(Drawn(vec3(0.0f, 0.0f, -10.0f)));
    CHECK_FALSE(Drawn(vec3(0.0f, 0.0f, 10.0f)));
    CHECK_FALSE(Drawn(vec3(100.0f, 0.0f, -10.0f)));

    // The set's frame carries its instances: the same instance behind a set turned half round
    // stands ahead.
    InstancedCullSet turned = set;
    turned.World = glm::rotate(mat4(1.0f), glm::pi<f32>(), vec3(0.0f, 1.0f, 0.0f));
    CHECK(CullInstance(
              MeshInstance::Make(vec3(0.0f, 0.0f, 10.0f), quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f),
              turned, view, Reference(), InstanceNoPreviousLod)
              .Drawn);
}

TEST_CASE("instanced cull: the size band partitions by the reference projected size")
{
    const InstancedCullSet set{.MeshBounds = UnitCube, .MinPixels = 20.0f, .MaxPixels = Unbounded};
    const InstancedCullReference reference = Reference();
    // A shadow view never measures the band itself: its own frustum keeps every instance here.
    InstancedCullView shadow = MainView();
    shadow.Main = false;
    shadow.FrustumCull = false;

    u32 mismatched = 0;
    u32 kept = 0;
    for (u32 i = 1; i <= 64; ++i)
    {
        const vec3 at(0.0f, 0.0f, -2.0f * static_cast<f32>(i));
        const MeshInstance instance = MeshInstance::Make(at, quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f);
        vec3 center;
        f32 radius = 0.0f;
        InstanceBoundingSphere(instance.GetMatrix(), UnitCube, center, radius);
        const bool inBand =
            ReferenceProjectedPixels(reference.ViewProj, reference.Eye, reference.HeightPixels,
                                     center, radius) >= set.MinPixels;
        const bool main =
            CullInstance(instance, set, MainView(), reference, InstanceNoPreviousLod).Drawn;
        const bool cast =
            CullInstance(instance, set, shadow, reference, InstanceNoPreviousLod).Drawn;
        mismatched += (main != inBand ? 1u : 0u) + (cast != inBand ? 1u : 0u);
        kept += inBand ? 1u : 0u;
    }
    CHECK(mismatched == 0);
    // The band splits the row rather than keeping or dropping all of it.
    CHECK(kept > 0);
    CHECK(kept < 64);
}

TEST_CASE("instanced cull: levels coarsen with distance, and hold inside the hysteresis band")
{
    const std::array<f32, 2> thresholds{0.5f, 0.1f};
    const InstancedCullSet set{
        .MeshBounds = UnitCube,
        .LodThresholds = thresholds,
        .LodCount = 3,
        .MaxPixels = Unbounded,
    };
    const InstancedCullReference reference = Reference();
    const InstancedCullView view = MainView(0.2f);
    const auto Level = [&](const f32 distance, const u8 previous)
    {
        return CullInstance(MeshInstance::Make(vec3(0.0f, 0.0f, -distance),
                                               quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f),
                            set, view, reference, previous)
            .Lod;
    };

    u32 last = 0;
    bool monotone = true;
    for (f32 distance = 1.0f; distance < 100.0f; distance *= 1.25f)
    {
        const u32 level = Level(distance, InstanceNoPreviousLod);
        monotone = monotone && level >= last;
        last = level;
    }
    CHECK(monotone);
    CHECK(last == 2);

    // Where the size crosses the first threshold: just past it either way, the level drawn last
    // frame is kept.
    const f32 radius = glm::length(UnitCube.Size()) * 0.5f;
    const f32 scale = LodView::FromViewProjection(reference.ViewProj, 0.0f).ProjectionScale;
    const f32 crossing = radius * scale / thresholds[0];
    CHECK(Level(crossing * 0.97f, 1) == 1);
    CHECK(Level(crossing * 1.03f, 0) == 0);
    CHECK(Level(crossing * 0.97f, InstanceNoPreviousLod) == 0);
    CHECK(Level(crossing * 1.03f, InstanceNoPreviousLod) == 1);
}

TEST_CASE("instanced cull: a shadow view skips casters smaller than its minimum texels")
{
    const mat4 light = glm::ortho(-50.0f, 50.0f, -50.0f, 50.0f, -100.0f, 100.0f) *
                       glm::lookAt(vec3(0.0f), vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, 0.0f, -1.0f));
    InstancedCullSet set{.MeshBounds = UnitCube, .MaxPixels = Unbounded, .MinShadowTexels = 4.0f};
    const InstancedCullView shadow{
        .Planes = Frustum::FromViewProjection(light),
        .FrustumCull = true,
        .Main = false,
        .Lod = LodView::FromViewProjection(light, 0.0f),
        .Resolution = 256.0f,
    };
    // A cube's diameter of ~1.7 units spans ~4.4 of 256 texels over 100 units.
    const auto Cast = [&](const f32 scale)
    {
        return CullInstance(MeshInstance::Make(vec3(0.0f, 0.0f, -10.0f),
                                               quat(1.0f, 0.0f, 0.0f, 0.0f), scale),
                            set, shadow, Reference(), InstanceNoPreviousLod)
            .Drawn;
    };
    CHECK(Cast(1.0f));
    CHECK_FALSE(Cast(0.5f));
    set.MinShadowTexels = 0.0f;
    CHECK(CullInstance(
              MeshInstance::Make(vec3(0.0f, 0.0f, -10.0f), quat(1.0f, 0.0f, 0.0f, 0.0f), 0.5f), set,
              shadow, Reference(), InstanceNoPreviousLod)
              .Drawn);
}
