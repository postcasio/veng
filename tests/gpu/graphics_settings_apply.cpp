// ApplyGraphicsSettings against a live headless Application: the no-world-reload apply path. Each
// viewport re-resolves its scene's RenderLook at the render after the apply, so the outcomes are read
// a frame later.
//
//  - a resolve output differing only in a per-frame view knob reconfigures nothing (the viewport's
//    output generation does not move) and the knob reaches the view — recompile only on a topology
//    change;
//  - a resolve output changing a topology field reconfigures the viewport (generation moves, and the
//    viewport's settings carry the new value), and the next render produces a valid output — the
//    "no world reload" property, applied to a running, presented viewport;
//  - a two-viewport set both reconfigure on one apply;
//  - an empty managed set (the editor) is a clean no-op.
//
// It drives a real Context through Run(), so it rides the gpu band.

#include <doctest/doctest.h>

#include <Veng/Application.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsResolve.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // A headless application driven by closures, with a swappable resolve seam.
    class GsApp final : public Application
    {
    public:
        using Application::Application;

        function<void(GsApp&)> InitFn;
        function<void(GsApp&, int)> StepFn;
        function<void(const GraphicsResolveInput&, GraphicsResolveOutput&)> Resolver;
        int Frames = 6;
        int Current = 0;

        // Opens a world with a camera at `eye`, a seat viewing through it, and a default RenderLook
        // for its viewports to resolve the settings over; returns the world.
        WorldInstanceId OpenCameraWorld(vec3 eye)
        {
            const WorldInstanceId world =
                GetWorldRunner().OpenWorld(WorldOpenInfo{.StartSimulation = false});
            Scene& scene = GetWorldRunner().ResolveWorld(world)->GetScene();
            const Entity camera = scene.CreateEntity();
            scene.Add<Transform>(camera).Position = eye;
            scene.Add<Camera>(camera);
            const Entity seat = scene.CreateEntity();
            scene.Add<Viewer>(seat).Camera = camera;
            scene.Add<RenderLook>(scene.CreateEntity());
            return world;
        }

    protected:
        void OnInitialize() override
        {
            if (InitFn)
            {
                InitFn(*this);
            }
        }

        void OnUpdate(f32) override
        {
            if (StepFn)
            {
                StepFn(*this, Current);
            }
            if (++Current >= Frames)
            {
                RequestExit();
            }
        }

        void OnResolveGraphics(const GraphicsResolveInput& input,
                               GraphicsResolveOutput& output) override
        {
            if (Resolver)
            {
                Resolver(input, output);
            }
        }
    };

    ApplicationInfo HeadlessInfo(vector<ManagedViewportInfo> managed)
    {
        ApplicationInfo info;
        info.Name = "veng-graphics-settings-apply-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        info.HeadlessExtent = {128, 96};
        info.ManagedViewports = std::move(managed);
        return info;
    }
}

TEST_CASE("A per-frame-only resolve reconfigures nothing; a topology change reconfigures once")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    GsApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    u64 genBeforePerFrame = 0;
    u64 genAfterPerFrame = 0;
    f32 appliedExposure = 0.0f;
    u64 genBeforeTopology = 0;
    u64 genAfterTopology = 0;
    u32 appliedShadowResolution = 0;

    app.InitFn = [&](GsApp& app)
    {
        const WorldInstanceId world = app.OpenCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, world);
        // Settle the viewport to the authored baseline first, so a later identity-topology apply
        // has nothing to reconfigure.
        app.ApplyGraphicsSettings();
    };

    app.StepFn = [&](GsApp& app, int frame)
    {
        Renderer::Viewport* vp = app.GetManagedViewports().Get(0);
        REQUIRE(vp != nullptr);

        if (frame == 2)
        {
            // A resolver that touches only a per-frame view knob leaves the topology surface at the
            // baseline, so no Configure fires.
            app.Resolver = [](const GraphicsResolveInput&, GraphicsResolveOutput& out)
            { out.View.Exposure = 4.0f; };
            genBeforePerFrame = vp->GetOutputGeneration();
            app.ApplyGraphicsSettings();
        }
        else if (frame == 3)
        {
            genAfterPerFrame = vp->GetOutputGeneration();
            appliedExposure = vp->GetViewState().Exposure;
        }
        else if (frame == 4)
        {
            // A resolver that changes a topology field forces exactly one reconfigure.
            app.Resolver = [](const GraphicsResolveInput&, GraphicsResolveOutput& out)
            { out.Settings.ShadowResolution = 2048; };
            genBeforeTopology = vp->GetOutputGeneration();
            app.ApplyGraphicsSettings();
        }
        else if (frame == 5)
        {
            genAfterTopology = vp->GetOutputGeneration();
            appliedShadowResolution = vp->GetSettings().ShadowResolution;
        }
    };

    app.Frames = 6;
    app.Run({});

    // Per-frame-only apply: no recompile, and the knob is live.
    CHECK(genAfterPerFrame == genBeforePerFrame);
    CHECK(appliedExposure == doctest::Approx(4.0f));
    // Topology apply: exactly one recompile, and the new setting is live.
    CHECK(genAfterTopology == genBeforeTopology + 1);
    CHECK(appliedShadowResolution == 2048);
}

TEST_CASE("A topology apply reconfigures every viewport in the managed set")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    GsApp app(HeadlessInfo({ManagedViewportInfo{}, ManagedViewportInfo{}}), types, systems);

    bool bothReconfigured = false;
    bool bothOutputsValid = false;
    u64 g0 = 0;
    u64 g1 = 0;

    app.InitFn = [&](GsApp& app)
    {
        const WorldInstanceId world = app.OpenCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, world);
        app.GetManagedViewports().SetViewportWorld(1, world);
        app.ApplyGraphicsSettings();
    };

    app.StepFn = [&](GsApp& app, int frame)
    {
        const ManagedViewportSet& set = app.GetManagedViewports();
        if (frame == 3)
        {
            REQUIRE(set.GetCount() == 2);
            g0 = set.Get(0)->GetOutputGeneration();
            g1 = set.Get(1)->GetOutputGeneration();
            app.Resolver = [](const GraphicsResolveInput&, GraphicsResolveOutput& out)
            { out.Settings.Bloom = !out.Settings.Bloom; };
            app.ApplyGraphicsSettings();
        }
        else if (frame == 4)
        {
            bothReconfigured =
                set.Get(0)->GetOutputGeneration() > g0 && set.Get(1)->GetOutputGeneration() > g1;
            bothOutputsValid =
                set.Get(0)->GetOutputHandle().IsValid() && set.Get(1)->GetOutputHandle().IsValid();
        }
    };

    app.Frames = 6;
    app.Run({});

    CHECK(bothReconfigured);
    CHECK(bothOutputsValid);
}

TEST_CASE("ApplyGraphicsSettings on an empty managed set is a clean no-op")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    // No managed viewport: the editor's posture.
    GsApp app(HeadlessInfo({}), types, systems);

    bool reached = false;
    app.InitFn = [&](GsApp& app)
    {
        REQUIRE(app.GetManagedViewports().Empty());
        app.Resolver = [](const GraphicsResolveInput&, GraphicsResolveOutput& out)
        { out.Settings.ShadowResolution = 4096; };
        // Must not crash and must not invoke the resolver (there is nothing to apply to).
        app.ApplyGraphicsSettings();
        reached = true;
    };

    app.Frames = 2;
    app.Run({});

    CHECK(reached);
}
