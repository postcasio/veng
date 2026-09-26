// A preview lights a scene with what it lacks: the look's environment as its sky when the scene
// has none, and a sun only when the scene would otherwise be unlit.

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "EditorOnly.h"
#include "PreviewLook.h"

using namespace Veng;
using namespace VengEditor;

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        registry.Register<EditorOnly>();
        return registry;
    }
}

TEST_CASE("PreviewLighting: an environment lights an unlit scene without a sun")
{
    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);

    const PreviewLighting lit = PlanPreviewLighting(*scene, true);
    CHECK(lit.Sky);
    CHECK_FALSE(lit.Sun);

    const PreviewLighting dark = PlanPreviewLighting(*scene, false);
    CHECK_FALSE(dark.Sky);
    CHECK(dark.Sun);
}

TEST_CASE("PreviewLighting: what the scene carries is kept")
{
    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);
    scene->Add<Sky>(scene->CreateEntity());

    // The scene's own sky stands; with no light and no environment sky added, the sun still
    // comes.
    const PreviewLighting ownSky = PlanPreviewLighting(*scene, true);
    CHECK_FALSE(ownSky.Sky);
    CHECK(ownSky.Sun);

    scene->Add<Light>(scene->CreateEntity());
    const PreviewLighting ownBoth = PlanPreviewLighting(*scene, true);
    CHECK_FALSE(ownBoth.Sky);
    CHECK_FALSE(ownBoth.Sun);
}

TEST_CASE("PreviewLighting: the added lighting is the editor's")
{
    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);

    AddPreviewLighting(*scene, {});
    usize lights = 0;
    usize marked = 0;
    scene->Each<Light>(
        [&](Entity entity, Light&)
        {
            ++lights;
            marked += scene->Has<EditorOnly>(entity) ? 1 : 0;
        });
    CHECK(lights == 1);
    CHECK(marked == 1);

    // A second pass finds the scene lit and adds nothing.
    AddPreviewLighting(*scene, {});
    CHECK(scene->EntityCount() == 1);
}
