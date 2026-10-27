// Heightfield collision geometry: a grid of heights built at run time into a CollisionShape, a
// body resting on the surface it describes (near the world's origin and far from it), rays reading
// its heights, cell tags and holes, the off-thread Prepare seam, and the validation that refuses a
// malformed grid. Pure CPU — a headless Scene or a bare PhysicsWorld, no Context, no GPU.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/Queries.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A tilted bowl: a plane rising along +x and +z with a paraboloid on it, so its lowest point
    // sits off the grid's centre and a sphere dropped there has a unique place to come to rest.
    f32 Bowl(const f32 x, const f32 z)
    {
        return 0.1f * x + 0.05f * z + 0.02f * (x * x + z * z);
    }

    // The bowl's lowest point: where its gradient (0.1 + 0.04 x, 0.05 + 0.04 z) vanishes.
    constexpr f32 BowlLowX = -2.5f;
    constexpr f32 BowlLowZ = -1.25f;

    // A grid of `count` samples a side, `spacing` apart and centred on the shape's origin, sampling
    // `height` at each sample's local (x, z).
    template <typename Height>
    Ref<CollisionShape> Grid(const u32 count, const f32 spacing, Height height)
    {
        const Ref<CollisionShape> shape = CreateRef<CollisionShape>();
        shape->Geometry = CollisionGeometry::HeightField;
        CollisionHeightField& field = shape->HeightField;
        const f32 half = static_cast<f32>(count - 1) * spacing * 0.5f;
        field.SampleCount = count;
        field.Spacing = spacing;
        field.Origin = vec3(-half, 0.0f, -half);
        field.Heights.resize(usize{count} * count);
        for (u32 z = 0; z < count; ++z)
        {
            for (u32 x = 0; x < count; ++x)
            {
                field.Heights[usize{z} * count + x] = height(-half + static_cast<f32>(x) * spacing,
                                                             -half + static_cast<f32>(z) * spacing);
            }
        }
        return shape;
    }

    Collider GridCollider(const Ref<CollisionShape>& shape)
    {
        Collider collider{.Shape = ColliderShape::Mesh};
        collider.Geometry = AssetManager::Adopt<CollisionShape>(shape);
        return collider;
    }

    const RigidBody StaticBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static};

    // Drops a 0.5 m sphere just above the bowl's lowest point on a 33-sample grid whose body stands
    // at `offset`, and returns where the sphere's centre settles, relative to `offset`.
    dvec3 RestingCentre(const dvec3 offset, const f32 radius)
    {
        const Unique<PhysicsWorld> world = PhysicsWorld::Create(PhysicsWorldInfo{});
        const Ref<CollisionShape> ground = Grid(33, 0.5f, Bowl);
        world->CreateBody(Entity{.Index = 1}, StaticBody, GridCollider(ground),
                          PhysicsPose{.Position = offset});

        const Entity ball{.Index = 2};
        world->CreateBody(
            ball,
            RigidBody{.Motion = MotionType::Dynamic,
                      .Mass = 1.0f,
                      .LinearDamping = 2.0f,
                      .AngularDamping = 2.0f},
            Collider{.Shape = ColliderShape::Sphere, .Extents = vec3(radius)},
            PhysicsPose{.Position =
                            offset +
                            dvec3(BowlLowX, Bowl(BowlLowX, BowlLowZ) + radius + 0.1f, BowlLowZ)});
        for (u32 step = 0; step < 300; ++step)
        {
            world->Step(FixedStep);
        }
        return world->GetBodyPose(ball)->Position - offset;
    }
}

TEST_CASE("a sphere rests on a heightfield at its radius from the surface the heights describe")
{
    constexpr f32 Radius = 0.5f;
    const dvec3 centre = RestingCentre(dvec3(0.0), Radius);

    // The nearest point of the analytic surface, by a 1 cm search around the rest point: the
    // triangulated grid departs from the bowl by under 2 mm at this spacing and curvature.
    f32 nearest = std::numeric_limits<f32>::max();
    const vec3 c = vec3(centre);
    for (f32 x = c.x - 1.0f; x <= c.x + 1.0f; x += 0.01f)
    {
        for (f32 z = c.z - 1.0f; z <= c.z + 1.0f; z += 0.01f)
        {
            nearest = std::min(nearest, glm::length(c - vec3(x, Bowl(x, z), z)));
        }
    }
    const f32 maxError = CollisionHeightField{}.MaxError;
    CHECK(std::abs(nearest - Radius) <= maxError + 0.02f);
    // It came to rest in the bowl, not somewhere it could have rolled to.
    CHECK(glm::length(vec2(c.x - BowlLowX, c.z - BowlLowZ)) < 0.1f);
}

TEST_CASE("a heightfield far from the world's origin holds a body exactly where it does at it")
{
    constexpr f32 Radius = 0.5f;
    const dvec3 near = RestingCentre(dvec3(0.0), Radius);
    const dvec3 far = RestingCentre(dvec3(7.0e6, -3.0e6, 5.0e6), Radius);
    CHECK(glm::length(far - near) < 1.0e-3);
}

TEST_CASE("a ray reads a heightfield's heights, cell tags and holes")
{
    constexpr u32 Count = 17;
    const auto waves = [](const f32 x, const f32 z)
    { return 0.3f * std::sin(0.4f * x) + 0.2f * std::cos(0.3f * z); };
    const Ref<CollisionShape> ground = Grid(Count, 1.0f, waves);
    CollisionHeightField& field = ground->HeightField;
    const auto tagOf = [](const u32 x, const u32 z)
    { return static_cast<u8>((x * 7 + z * 3) % 5 + 1); };
    field.CellTags.resize(usize{Count - 1} * (Count - 1));
    for (u32 z = 0; z + 1 < Count; ++z)
    {
        for (u32 x = 0; x + 1 < Count; ++x)
        {
            field.CellTags[usize{z} * (Count - 1) + x] = tagOf(x, z);
        }
    }
    // A hole at sample (5, 5) takes both triangles of the cell it is the low corner of.
    constexpr u32 HoleX = 5;
    constexpr u32 HoleZ = 5;
    field.Heights[usize{HoleZ} * Count + HoleX] = CollisionHeightField::Hole;

    const Unique<PhysicsWorld> world = PhysicsWorld::Create(PhysicsWorldInfo{});
    const Entity body{.Index = 1};
    world->CreateBody(body, StaticBody, GridCollider(ground), PhysicsPose{});

    const f32 origin = field.Origin.x;
    const auto down = [&](const f32 x, const f32 z)
    { return Raycast(world.get(), dvec3(x, 10.0, z), vec3(0.0f, -1.0f, 0.0f), 20.0f); };

    u32 heightsRead = 0;
    u32 tagsRead = 0;
    for (const u32 sx : {1U, 8U, 14U})
    {
        for (const u32 sz : {2U, 9U, 13U})
        {
            const f32 x = origin + static_cast<f32>(sx);
            const f32 z = origin + static_cast<f32>(sz);
            // A hair inside the sample's cell, so the ray meets a face rather than a vertex.
            const optional<RayHit> atSample = down(x + 1.0e-4f, z + 1.0e-4f);
            if (atSample && atSample->Body == body &&
                std::abs(static_cast<f32>(atSample->Position.y) - waves(x, z)) <= field.MaxError)
            {
                ++heightsRead;
            }
            const optional<RayHit> atCell = down(x + 0.5f, z + 0.5f);
            if (atCell && atCell->SurfaceTag == tagOf(sx, sz))
            {
                ++tagsRead;
            }
        }
    }
    CHECK(heightsRead == 9);
    CHECK(tagsRead == 9);

    const f32 holeX = origin + static_cast<f32>(HoleX) + 0.5f;
    const f32 holeZ = origin + static_cast<f32>(HoleZ) + 0.5f;
    CHECK_FALSE(down(holeX, holeZ).has_value());
    // A box resting elsewhere reports no tag: only a heightfield cell carries one.
    const Entity box{.Index = 2};
    world->CreateBody(box, StaticBody, Collider{.Shape = ColliderShape::Box, .Extents = vec3(0.5f)},
                      PhysicsPose{.Position = dvec3(30.0, 0.0, 0.0)});
    const optional<RayHit> onBox = down(30.0f, 0.0f);
    REQUIRE(onBox.has_value());
    CHECK(onBox->Body == box);
    CHECK(onBox->SurfaceTag == 0);
}

TEST_CASE("a heightfield whose sample count is no multiple of the solver's block builds and reads "
          "to its last sample")
{
    // None of these counts is a multiple of its block (2 for 3 samples, 4 above), so each leaves a
    // partial block the build pads. Heights rising along x alone leave a block's column flat and
    // its last row sloped, which is the case where the solver's quantization would otherwise read
    // whole blocks past the grid's last row.
    const auto slope = [](const f32 x, const f32 /*z*/) { return 0.25f * x; };
    for (const u32 count : {3U, 5U, 17U, 63U, 65U})
    {
        CAPTURE(count);
        const Ref<CollisionShape> ground = Grid(count, 1.0f, slope);
        CollisionHeightField& field = ground->HeightField;
        field.CellTags.assign(usize{count - 1} * (count - 1), 1);
        // The last cell's tag, which the padding must leave in place.
        field.CellTags.back() = 3;

        const Unique<PhysicsWorld> world = PhysicsWorld::Create(PhysicsWorldInfo{});
        const Entity body{.Index = 1};
        world->CreateBody(body, StaticBody, GridCollider(ground), PhysicsPose{});

        // Inside the far corner cell, and past the grid's edge, where the padding stands.
        const f32 last = field.Origin.x + static_cast<f32>(count - 1);
        const f32 x = last - 0.25f;
        const f32 z = last - 0.25f;
        const optional<RayHit> corner =
            Raycast(world.get(), dvec3(x, 10.0, z), vec3(0.0f, -1.0f, 0.0f), 40.0f);
        REQUIRE(corner.has_value());
        CHECK(std::abs(static_cast<f32>(corner->Position.y) - slope(x, z)) <= field.MaxError);
        CHECK(corner->SurfaceTag == 3);
        CHECK_FALSE(Raycast(world.get(), dvec3(last + 0.5, 10.0, last + 0.5),
                            vec3(0.0f, -1.0f, 0.0f), 40.0f)
                        .has_value());
    }
}

TEST_CASE("a heightfield prepared on a worker is adopted without a rebuild, and its entry still "
          "drops once nothing holds it")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);
    scene->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));
    const PhysicsWorld& world = *scene->GetPhysicsWorld();

    const Ref<CollisionShape> ground = Grid(65, 1.0f, Bowl);
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 1});
    auto prepared = tasks.Submit([ground] { ground->Prepare(); });
    REQUIRE(prepared.Get().has_value());
    REQUIRE(ground->IsPrepared());

    // The continuation's half: adopt, and add the body's components.
    const Collider collider = GridCollider(ground);
    const auto spawn = [&](const vec3 position)
    {
        const Entity entity = scene->CreateEntity();
        scene->Add<Transform>(entity, Transform{.Position = position});
        scene->Add<RigidBody>(entity, StaticBody);
        scene->Add<Collider>(entity, collider);
        return entity;
    };
    const Entity first = spawn(vec3(0.0f));
    const Entity second = spawn(vec3(100.0f, 0.0f, 0.0f));
    StepPhysics(*scene, FixedStep);

    CHECK(world.HasBody(first));
    CHECK(world.HasBody(second));
    CHECK(world.GetBuiltShapeCount() == 1);
    CHECK(world.FindBuiltShape(collider) == ground->GetPreparedShape());
    CHECK(world.GetBodyShape(first) == ground->GetPreparedShape());
    CHECK(world.GetBodyShape(second) == ground->GetPreparedShape());

    scene->DestroyEntity(first);
    scene->DestroyEntity(second);
    StepPhysics(*scene, FixedStep);
    StepPhysics(*scene, FixedStep);
    CHECK(world.GetBodyCount() == 0);
    CHECK(world.GetBuiltShapeCount() == 0);
}

TEST_CASE("ValidateHeightField names each malformed grid and passes a sound one")
{
    const Ref<CollisionShape> sound = Grid(9, 0.5f, Bowl);
    CHECK(CollisionShape::ValidateHeightField(sound->HeightField).has_value());

    const auto refused = [&](auto&& damage)
    {
        CollisionHeightField field = sound->HeightField;
        damage(field);
        return !CollisionShape::ValidateHeightField(field).has_value();
    };
    CHECK(refused(
        [](CollisionHeightField& f)
        {
            f.SampleCount = 2;
            f.Heights.resize(4);
        }));
    CHECK(refused([](CollisionHeightField& f)
                  { f.SampleCount = CollisionHeightField::MaxSampleCount + 1; }));
    CHECK(refused([](CollisionHeightField& f) { f.Heights.pop_back(); }));
    CHECK(refused([](CollisionHeightField& f)
                  { f.Heights[3] = std::numeric_limits<f32>::quiet_NaN(); }));
    CHECK(refused([](CollisionHeightField& f)
                  { f.Heights[3] = -std::numeric_limits<f32>::infinity(); }));
    CHECK(refused([](CollisionHeightField& f) { f.Spacing = 0.0f; }));
    CHECK(refused([](CollisionHeightField& f) { f.Spacing = -1.0f; }));
    CHECK(refused([](CollisionHeightField& f) { f.CellTags.resize(7); }));
    CHECK(refused([](CollisionHeightField& f) { f.MaxError = 0.0f; }));

    // A hole is a legal height, and a tag per cell is the legal tag count.
    CHECK_FALSE(
        refused([](CollisionHeightField& f) { f.Heights[3] = CollisionHeightField::Hole; }));
    CHECK_FALSE(refused([](CollisionHeightField& f) { f.CellTags.resize(64); }));
}
