// Mesh-socket lookup + attachment unit cases: pure CPU, no Vulkan. A Mesh built through the
// MeshInfo factory carries a socket list and empty GPU buffers, which is everything
// Mesh::FindSocket and AttachToSocket read. Covers the sorted binary search, the miss paths that
// must return rather than assert, and the composed world transform an attached child ends up at —
// including the orientation contract (a socket's local -Z is forward, +Y is up).

#include <doctest/doctest.h>

#include <cmath>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Sockets.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    // A quarter turn about +Y: it aims a socket's forward (-Z) down -X and leaves up (+Y) alone.
    quat QuarterTurnY()
    {
        return glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f));
    }

    // A device-free Mesh carrying only a name and a socket list, sorted by name as the cook emits.
    Ref<Mesh> SocketMesh()
    {
        return Mesh::Create(MeshInfo{
            .Name = "socketed",
            .Sockets =
                {
                    MeshSocket{.Name = "Mount_A", .Position = vec3(1.0f, 2.0f, 3.0f)},
                    MeshSocket{.Name = "Mount_B",
                               .Position = vec3(0.0f, 0.5f, 0.0f),
                               .Rotation = QuarterTurnY()},
                    MeshSocket{.Name = "Mount_C", .Position = vec3(-4.0f, 0.0f, 0.0f)},
                },
        });
    }

    void CheckVec(const vec3& actual, const vec3& expected)
    {
        CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-4));
        CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-4));
        CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-4));
    }

    // A scene wired with exactly the component types the socket path touches.
    struct SocketScene
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Assets;
        Unique<Scene> World;

        SocketScene()
        {
            Types.Register<Transform>("Transform");
            Types.Register<Hierarchy>("Hierarchy");
            Types.Register<MeshRenderer>("MeshRenderer");
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
            World = Scene::Create(Types);
        }

        // An entity at `position` drawing a mesh carrying the fixture sockets.
        Entity SpawnSocketedMesh(const vec3& position)
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<MeshRenderer>(entity,
                                     MeshRenderer{.Mesh = Assets->Adopt<Mesh>(SocketMesh())});
            return entity;
        }
    };
}

TEST_CASE("Mesh::FindSocket resolves a name and misses cleanly")
{
    const Ref<Mesh> mesh = SocketMesh();
    REQUIRE(mesh->GetSockets().size() == 3);

    const MeshSocket* found = mesh->FindSocket("Mount_B");
    REQUIRE(found != nullptr);
    CHECK(found->Name == "Mount_B");
    CheckVec(found->Position, vec3(0.0f, 0.5f, 0.0f));

    // The binary search reaches both ends of the sorted list, and a name that is not there —
    // including one that is a prefix of a real socket — is a null, not an assert.
    CHECK(mesh->FindSocket("Mount_A") != nullptr);
    CHECK(mesh->FindSocket("Mount_C") != nullptr);
    CHECK(mesh->FindSocket("Mount") == nullptr);
    CHECK(mesh->FindSocket("Mount_D") == nullptr);
    CHECK(mesh->FindSocket("") == nullptr);

    // A mesh authored with no sockets answers every lookup with null.
    const Ref<Mesh> bare = Mesh::Create(MeshInfo{.Name = "bare"});
    CHECK(bare->GetSockets().empty());
    CHECK(bare->FindSocket("Mount_A") == nullptr);
}

TEST_CASE("AttachToSocket parents the child and places it at the socket's world transform")
{
    SocketScene fixture;
    const Entity host = fixture.SpawnSocketedMesh(vec3(10.0f, 0.0f, 0.0f));

    const Entity child = fixture.World->CreateEntity();
    REQUIRE(AttachToSocket(*fixture.World, child, host, "Mount_A"));

    CHECK(fixture.World->GetParent(child) == host);

    // The child's own Transform is the socket's mesh-space transform, so its world matrix is the
    // host's world composed with the socket — the authored place on the model.
    const mat4 world = WorldMatrix(*fixture.World, child);
    CheckVec(vec3(world[3]), vec3(11.0f, 2.0f, 3.0f));

    // Plain parenting, so moving the host carries the child with it for free.
    fixture.World->Get<Transform>(host).Position = vec3(0.0f, 100.0f, 0.0f);
    CheckVec(vec3(WorldMatrix(*fixture.World, child)[3]), vec3(1.0f, 102.0f, 3.0f));
}

TEST_CASE("AttachToSocket applies the socket's orientation, not only its position")
{
    SocketScene fixture;
    const Entity host = fixture.SpawnSocketedMesh(vec3(0.0f));

    const Entity child = fixture.World->CreateEntity();
    REQUIRE(AttachToSocket(*fixture.World, child, host, "Mount_B"));

    // A socket's local -Z is forward and +Y is up. Mount_B is a quarter turn about +Y, so the
    // attached child faces -X in world space and still points up along +Y.
    const mat4 world = WorldMatrix(*fixture.World, child);
    CheckVec(vec3(world * vec4(0.0f, 0.0f, -1.0f, 0.0f)), vec3(-1.0f, 0.0f, 0.0f));
    CheckVec(vec3(world * vec4(0.0f, 1.0f, 0.0f, 0.0f)), vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("AttachToSocket reports a miss instead of asserting")
{
    SocketScene fixture;
    const Entity host = fixture.SpawnSocketedMesh(vec3(0.0f));

    const Entity child = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(child, Transform{.Position = vec3(7.0f, 7.0f, 7.0f)});

    // A missing socket is a content error the caller reports; the child is left where it was.
    CHECK_FALSE(AttachToSocket(*fixture.World, child, host, "Mount_Missing"));
    CHECK(fixture.World->GetParent(child).IsNull());
    CheckVec(fixture.World->Get<Transform>(child).Position, vec3(7.0f, 7.0f, 7.0f));

    // So is an entity that draws nothing, and one whose mesh handle is not resident.
    const Entity bare = fixture.World->CreateEntity();
    CHECK_FALSE(AttachToSocket(*fixture.World, child, bare, "Mount_A"));
    CHECK(FindMeshSocket(*fixture.World, bare, "Mount_A") == nullptr);

    const Entity pending = fixture.World->CreateEntity();
    fixture.World->Add<MeshRenderer>(pending, MeshRenderer{});
    CHECK_FALSE(AttachToSocket(*fixture.World, child, pending, "Mount_A"));

    CHECK(FindMeshSocket(*fixture.World, host, "Mount_A") != nullptr);
    CHECK(FindMeshSocket(*fixture.World, Entity::Null, "Mount_A") == nullptr);
}
