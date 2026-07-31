// The sibling-requirement gate: VE_REQUIRES declares the components a component resolves off its
// own entity, and Scene::RemoveComponent refuses to remove one while a requirer sits beside it.
// Pure CPU — the components are pooled and never driven, so no Context and no device is involved.

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Gui/Surface.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

TEST_CASE("a component declares its required siblings in the registry")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    types.Register<GuiSurface>();

    const vector<TypeId>& required = types.Info(TypeIdOf<GuiSurface>()).Requires;
    CHECK(std::ranges::find(required, TypeIdOf<MeshRenderer>()) != required.end());

    // An unmarked type carries none, and being required does not make MeshRenderer a requirer.
    CHECK(types.Info(TypeIdOf<Transform>()).Requires.empty());
    CHECK(types.Info(TypeIdOf<MeshRenderer>()).Requires.empty());
}

TEST_CASE("removing a required sibling is refused and the component stays")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    types.Register<GuiSurface>();
    Unique<Scene> scene = Scene::Create(types);

    const Entity panel = scene->CreateEntity();
    scene->Add<MeshRenderer>(panel);
    scene->Add<GuiSurface>(panel);

    CHECK(scene->FindRequirer(panel, TypeIdOf<MeshRenderer>()) == TypeIdOf<GuiSurface>());

    const VoidResult removed = scene->Remove<MeshRenderer>(panel);
    CHECK_FALSE(removed.has_value());
    if (!removed)
    {
        CHECK(removed.error().find("MeshRenderer") != std::string::npos);
        CHECK(removed.error().find("GuiSurface") != std::string::npos);
    }

    // Every assertion above is non-fatal so the case reaches this line either way: the surface's
    // sibling is resolved through Get, which aborts on a component the entity lacks. Ungated, the
    // removal succeeds and this kills the process rather than failing a case.
    CHECK(scene->Has<MeshRenderer>(panel));
    CHECK(scene->Get<MeshRenderer>(panel).Visible);
}

TEST_CASE("removing the requirer first frees the sibling")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    types.Register<GuiSurface>();
    Unique<Scene> scene = Scene::Create(types);

    const Entity panel = scene->CreateEntity();
    scene->Add<MeshRenderer>(panel);
    scene->Add<GuiSurface>(panel);

    CHECK(scene->Remove<GuiSurface>(panel).has_value());
    CHECK(scene->FindRequirer(panel, TypeIdOf<MeshRenderer>()) == InvalidTypeId);
    CHECK(scene->Remove<MeshRenderer>(panel).has_value());
    CHECK_FALSE(scene->Has<MeshRenderer>(panel));
}

TEST_CASE("the gate reaches only the entity carrying the requirer")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    types.Register<GuiSurface>();
    Unique<Scene> scene = Scene::Create(types);

    const Entity panel = scene->CreateEntity();
    scene->Add<MeshRenderer>(panel);
    scene->Add<GuiSurface>(panel);

    const Entity plainMesh = scene->CreateEntity();
    scene->Add<MeshRenderer>(plainMesh);

    CHECK(scene->Remove<MeshRenderer>(plainMesh).has_value());
    CHECK_FALSE(scene->Has<MeshRenderer>(plainMesh));
    CHECK(scene->Has<MeshRenderer>(panel));

    // Removing a component the entity does not carry stays the documented no-op rather than
    // reporting the requirement it would have hit.
    const Entity surfaceOnly = scene->CreateEntity();
    scene->Add<GuiSurface>(surfaceOnly);
    CHECK(scene->Remove<MeshRenderer>(surfaceOnly).has_value());
}

TEST_CASE("destroying an entity is not gated by a requirement")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    types.Register<GuiSurface>();
    Unique<Scene> scene = Scene::Create(types);

    const Entity panel = scene->CreateEntity();
    scene->Add<MeshRenderer>(panel);
    scene->Add<GuiSurface>(panel);

    scene->DestroyEntity(panel);
    CHECK_FALSE(scene->IsAlive(panel));

    usize remaining = 0;
    scene->Each<MeshRenderer>([&](Entity, MeshRenderer&) { ++remaining; });
    scene->Each<GuiSurface>([&](Entity, GuiSurface&) { ++remaining; });
    CHECK(remaining == 0);
}
