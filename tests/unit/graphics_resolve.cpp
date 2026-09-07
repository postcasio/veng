// Graphics resolve/apply cases, device-free:
//
//  - SceneRendererSettings::operator== is the dirty-compare ApplyGraphicsSettings uses to decide
//    whether a resolve reconfigures a viewport. Two settings differing only in what would be a
//    per-frame (SceneView) knob are equal (no recompile); a change to any topology field is unequal
//    (exactly the recompile-only-on-topology-change property, distilled off the GPU).
//  - The default Application::OnResolveGraphics is identity: it leaves the pre-filled authored
//    baseline untouched, so an app that declares no schema resolves to the authored look unchanged.
//
// No Context is constructed for the operator== cases; the identity case builds an Application with no
// window and no Run(), touching no device, and calls the resolve seam directly.

#include <doctest/doctest.h>

#include <Veng/Application.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsResolve.h>
#include <Veng/Render/GraphicsSettings.h>
#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SystemRegistry.h>

using namespace Veng;

TEST_CASE("SceneRendererSettings equality is the topology dirty-compare")
{
    const Renderer::SceneRendererSettings base;

    SUBCASE("identical settings compare equal")
    {
        const Renderer::SceneRendererSettings same;
        CHECK(base == same);
    }

    SUBCASE("a bool topology field breaks equality")
    {
        Renderer::SceneRendererSettings changed = base;
        changed.Shadows = !changed.Shadows;
        CHECK_FALSE(base == changed);
    }

    SUBCASE("a sizing field breaks equality")
    {
        Renderer::SceneRendererSettings changed = base;
        changed.ShadowResolution = base.ShadowResolution * 2;
        CHECK_FALSE(base == changed);
    }

    SUBCASE("an enum topology field breaks equality")
    {
        Renderer::SceneRendererSettings changed = base;
        changed.Cull = Renderer::SceneRendererSettings::CullMode::GPU;
        CHECK_FALSE(base == changed);
    }
}

namespace
{
    // Exposes the protected resolve seam so a test can invoke it directly without Run().
    class ResolveApp final : public Application
    {
    public:
        using Application::Application;
        using Application::OnResolveGraphics;
    };
}

TEST_CASE("the default OnResolveGraphics is identity")
{
    TypeRegistry types;
    SystemRegistry systems;
    ResolveApp app{ApplicationInfo{.Name = "graphics-resolve-test"}, types, systems};

    const GraphicsSettings store{GraphicsSettingsInfo{.Types = &types}};
    const LevelRenderSettings authored;

    // A baseline the resolver must not touch: a non-default topology field and a non-default view knob.
    GraphicsResolveOutput output;
    output.Settings.Shadows = false;
    output.Settings.ShadowResolution = 2048;
    output.View.Exposure = 3.5f;
    output.DynamicResolutionEnabled = true;
    output.DynamicResolution.TargetFrameTimeMs = 8.0f;

    const GraphicsResolveOutput before = output;
    const GraphicsResolveInput input{
        .Settings = store, .Display = store.GetDisplay(), .AuthoredLook = authored};
    app.OnResolveGraphics(input, output);

    CHECK(before.Settings == output.Settings);
    CHECK(output.View.Exposure == doctest::Approx(before.View.Exposure));
    CHECK(output.DynamicResolutionEnabled == before.DynamicResolutionEnabled);
    CHECK(output.DynamicResolution.TargetFrameTimeMs ==
          doctest::Approx(before.DynamicResolution.TargetFrameTimeMs));
}
