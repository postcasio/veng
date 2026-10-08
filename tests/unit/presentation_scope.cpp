// Presentation scopes: the per-scene lease that decides what a scene's owned sound and rumble does each
// frame. Device-free — a scope is pure bookkeeping, and the runner cases drive empty-scene worlds over
// a TestServices bundle, whose contexts present nothing (an empty View), so a ticking world reads Muted.

#include <doctest/doctest.h>

#include <set>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/PresentationScope.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include "support/TestServices.h"

using namespace Veng;

namespace
{
    // A Sim-phase system that closes the world it runs in from its own update, so the runner defers
    // the close to the end of its walk.
    struct SelfClosingProbe final : SceneSystem
    {
        static inline WorldRunner* Runner = nullptr;

        void OnUpdate(Scene&, f32, const SystemContext& context) override
        {
            if (Runner != nullptr)
            {
                Runner->CloseWorld(context.World);
            }
        }
    };

    constexpr f32 Step = 1.0f / 60.0f;

    PresentationScopeId ScopeOf(const WorldRunner& runner, const WorldInstanceId world)
    {
        const World* resolved = runner.ResolveWorld(world);
        REQUIRE(resolved != nullptr);
        const PresentationScope* scope = resolved->GetScene().GetPresentationScope();
        REQUIRE(scope != nullptr);
        return scope->GetId();
    }
}

VE_SYSTEM(SelfClosingProbe, 0x4B3B2BD8FC20A06AULL, "Self Closing Probe");

TEST_CASE("A scope's lease latches Live, Muted or Held once per frame, and starts Held")
{
    PresentationScopes scopes;
    const Unique<PresentationScope> scope = scopes.Open();
    const PresentationScopeId id = scope->GetId();
    CHECK(scopes.GetState(id) == PresentationState::Held);

    scope->Renew(true);
    CHECK(scopes.GetState(id) == PresentationState::Held);
    scopes.Resolve();
    CHECK(scopes.GetState(id) == PresentationState::Live);

    scope->Renew(false);
    scopes.Resolve();
    CHECK(scopes.GetState(id) == PresentationState::Muted);

    scopes.Resolve();
    CHECK(scopes.GetState(id) == PresentationState::Held);

    // Two renewals in one frame are one renewal, audible when either was.
    scope->Renew(false);
    scope->Renew(true);
    scopes.Resolve();
    CHECK(scopes.GetState(id) == PresentationState::Live);
}

TEST_CASE("A scope reads Closed once dropped, detached or replaced, and an unknown id reads Closed")
{
    PresentationScopes scopes;
    PresentationScopeId dropped;
    {
        const Unique<PresentationScope> scope = scopes.Open();
        dropped = scope->GetId();
        scope->Renew(true);
        scopes.Resolve();
    }
    CHECK(scopes.GetState(dropped) == PresentationState::Closed);

    TypeRegistry types;
    Unique<Scene> scene = Scene::Create(types);
    scene->SetPresentationScope(scopes.Open());
    const PresentationScopeId detached = scene->GetPresentationScope()->GetId();
    CHECK(scene->Clone()->GetPresentationScope() == nullptr);
    scene->SetPresentationScope(nullptr);
    CHECK(scopes.GetState(detached) == PresentationState::Closed);
    CHECK(scene->GetPresentationScope() == nullptr);

    scene->SetPresentationScope(scopes.Open());
    const PresentationScopeId replaced = scene->GetPresentationScope()->GetId();
    scene->SetPresentationScope(scopes.Open());
    const PresentationScopeId current = scene->GetPresentationScope()->GetId();
    CHECK(scopes.GetState(replaced) == PresentationState::Closed);
    CHECK(scopes.GetState(current) == PresentationState::Held);

    scene.reset();
    CHECK(scopes.GetState(current) == PresentationState::Closed);
    CHECK(scopes.GetState(PresentationScopeId{}) == PresentationState::Closed);
    CHECK(scopes.GetState(PresentationScopeId{.Value = 1ULL << 40}) == PresentationState::Closed);
    CHECK(scopes.GetScopes().size() == 1);
}

TEST_CASE("Scope ids are never reused across closures")
{
    PresentationScopes scopes;
    std::set<u64> seen{scopes.GetApplicationScope().Value};
    constexpr usize Cycles = 300;
    for (usize i = 0; i < Cycles; ++i)
    {
        seen.insert(scopes.Open()->GetId().Value);
    }
    CHECK(seen.size() == Cycles + 1);
}

TEST_CASE("The application scope reads Live in every frame though nothing renews it")
{
    PresentationScopes scopes;
    const PresentationScopeId application = scopes.GetApplicationScope();
    REQUIRE(application.IsValid());
    usize live = 0;
    constexpr usize Frames = 4;
    for (usize i = 0; i < Frames; ++i)
    {
        scopes.Resolve();
        live += scopes.GetState(application) == PresentationState::Live ? 1 : 0;
    }
    CHECK(live == Frames);
    REQUIRE(scopes.GetScopes().size() == 1);
    CHECK(scopes.GetScopes().front().Id == application);
}

TEST_CASE("A scene's View phase renews its scope, audible only when the scene is presented")
{
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    TypeRegistry types;
    const Unique<Scene> scene = Scene::Create(types);
    scene->SetPresentationScope(scopes.Open());
    const PresentationScopeId id = scene->GetPresentationScope()->GetId();
    SystemContext context = services.Make();

    // A Sim step is not the lease: only the frame's View pass renews it.
    scene->TickSimulationPhase(SceneSystem::Phase::Sim, Step, context);
    scopes.Resolve();
    CHECK(scopes.GetState(id) == PresentationState::Held);

    scene->TickSimulationPhase(SceneSystem::Phase::View, Step, context);
    scopes.Resolve();
    CHECK(scopes.GetState(id) == PresentationState::Muted);

    context.View = SystemViewInfo{};
    scene->TickSimulationPhase(SceneSystem::Phase::View, Step, context);
    scopes.Resolve();
    CHECK(scopes.GetState(id) == PresentationState::Live);
}

TEST_CASE("A runner world's scope follows its View phase through pause, close and replacement")
{
    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<SelfClosingProbe>();
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    WorldRunner runner(
        WorldRunnerInfo{.Types = &types, .Systems = &systems, .Presentation = &scopes});
    runner.SetContextFactory(services.Factory());
    const auto frame = [&]
    {
        runner.Tick(WorldTickInfo{.Delta = Step});
        scopes.Resolve();
    };

    // A simulation running no systems: the phase alone holds the lease.
    const WorldInstanceId world = runner.OpenWorld(WorldOpenInfo{.Systems = vector<SystemId>{}});
    const PresentationScopeId scope = ScopeOf(runner, world);
    CHECK(scopes.GetState(scope) == PresentationState::Held);

    SUBCASE("An unpresented world is Muted, Held while paused, and Muted again on resume")
    {
        frame();
        CHECK(scopes.GetState(scope) == PresentationState::Muted);
        runner.SetWorldPaused(world, true);
        frame();
        CHECK(scopes.GetState(scope) == PresentationState::Held);
        runner.SetWorldPaused(world, false);
        frame();
        CHECK(scopes.GetState(scope) == PresentationState::Muted);
    }

    SUBCASE("A world its own system closes mid-tick has a Closed scope by that frame's Resolve")
    {
        SelfClosingProbe::Runner = &runner;
        const WorldInstanceId closing = runner.OpenWorld(
            WorldOpenInfo{.Systems = vector<SystemId>{SystemIdOf<SelfClosingProbe>()}});
        const PresentationScopeId closingScope = ScopeOf(runner, closing);
        frame();
        SelfClosingProbe::Runner = nullptr;
        CHECK(runner.ResolveWorld(closing) == nullptr);
        CHECK(scopes.GetState(closingScope) == PresentationState::Closed);
        CHECK(scopes.GetState(scope) == PresentationState::Muted);
    }

    SUBCASE("An installed scene closes the replaced scene's scope and carries a fresh one")
    {
        runner.InstallScene(world, Scene::Create(types));
        const PresentationScopeId installed = ScopeOf(runner, world);
        CHECK(scopes.GetState(scope) == PresentationState::Closed);
        CHECK_FALSE(installed == scope);
        CHECK(scopes.GetState(installed) == PresentationState::Held);
    }
}
