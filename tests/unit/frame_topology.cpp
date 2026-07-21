// Frame-topology unit cases. ResolveFrameTopology decides which passes a frame
// wires from the SceneRendererSettings plus the resolved sky facts, with no
// device involved, so the branch set is pinned here rather than only through a
// rendered image. Pure data → data; no Context, no driver.

#include <doctest/doctest.h>

#include <array>

#include "Renderer/FrameTopology.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Every debug arm — everything but Final. Driven off DebugViewNames so a new arm
    // joins the invariants below instead of silently falling out of them.
    constexpr usize ArmCount = DebugViewNames.size();

    SceneRendererSettings ModeOnly(const DebugView mode)
    {
        // Every battery toggle off, so an arm that still wires a pass proves the
        // force-wire rather than inheriting it from a default-on setting.
        SceneRendererSettings settings;
        settings.Mode = mode;
        settings.Bloom = false;
        settings.Shadows = false;
        settings.PunctualShadows = false;
        settings.AO = false;
        settings.SSR = false;
        settings.DepthOfField = false;
        settings.TAA = false;
        settings.AutoExposure = false;
        settings.Refraction = false;
        return settings;
    }

    constexpr SkyTopologyInput NoSky{};

    FrameTopology Resolve(const SceneRendererSettings& settings)
    {
        return ResolveFrameTopology(settings, NoSky);
    }
}

TEST_CASE("frame topology: a debug arm force-wires its producing battery with the toggle off")
{
    // The AO arm needs the SSAO pass to have written the target it blits, but must
    // not fold the term into lighting (there is no lighting pass on a debug arm).
    const FrameTopology ao = Resolve(ModeOnly(DebugView::AO));
    CHECK(ao.SsaoActive);
    CHECK_FALSE(ao.SsaoFold);

    const FrameTopology reflections = Resolve(ModeOnly(DebugView::Reflections));
    CHECK(reflections.SsrActive);

    // Cascades blits a per-fragment tint, so the shadow pass must run to write the
    // cascade constants the tint indexes.
    const FrameTopology cascades = Resolve(ModeOnly(DebugView::Cascades));
    CHECK(cascades.ShadowActive);

    const FrameTopology shadows = Resolve(ModeOnly(DebugView::Shadows));
    CHECK(shadows.ShadowActive);

    const FrameTopology punctual = Resolve(ModeOnly(DebugView::PunctualShadows));
    CHECK(punctual.PunctualShadowActive);

    // Bloom is the one arm that composites the scene, so it wires the whole lit
    // chain ahead of the pyramid it visualizes.
    const FrameTopology bloom = Resolve(ModeOnly(DebugView::Bloom));
    CHECK(bloom.BloomActive);
    CHECK(bloom.SceneComposited);

    const FrameTopology coc = Resolve(ModeOnly(DebugView::CoC));
    CHECK(coc.Dof == DofStages::CocOnly);
}

TEST_CASE("frame topology: depth of field is a tri-state")
{
    SceneRendererSettings settings = ModeOnly(DebugView::Final);
    CHECK(Resolve(settings).Dof == DofStages::None);
    CHECK_FALSE(Resolve(settings).DofWired());
    CHECK_FALSE(Resolve(settings).DofComposited());

    settings.DepthOfField = true;
    CHECK(Resolve(settings).Dof == DofStages::Full);
    CHECK(Resolve(settings).DofWired());
    CHECK(Resolve(settings).DofComposited());

    settings.Mode = DebugView::CoC;
    settings.DepthOfField = false;
    CHECK(Resolve(settings).Dof == DofStages::CocOnly);
    CHECK(Resolve(settings).DofWired());
    CHECK_FALSE(Resolve(settings).DofComposited());

    // The debug arm never wires the composite, even with the feature on: compositing
    // would alter the HDR tail and change what the debug view is showing.
    settings.DepthOfField = true;
    CHECK(Resolve(settings).Dof == DofStages::CocOnly);
    CHECK_FALSE(Resolve(settings).DofComposited());
}

TEST_CASE("frame topology: the scene composites for Final and Bloom alone")
{
    for (usize arm = 0; arm < ArmCount; ++arm)
    {
        const auto mode = static_cast<DebugView>(arm);
        const bool expected = mode == DebugView::Final || mode == DebugView::Bloom;
        CAPTURE(DebugViewNames[arm]);
        CHECK(Resolve(ModeOnly(mode)).SceneComposited == expected);
    }
}

TEST_CASE("frame topology: the Final-only effects are inactive under every debug arm")
{
    // Every battery on, so an arm leaking a Final-only effect shows up.
    SceneRendererSettings settings;
    settings.Bloom = true;
    settings.Shadows = true;
    settings.PunctualShadows = true;
    settings.AO = true;
    settings.SSR = true;
    settings.DepthOfField = true;
    settings.TAA = true;
    settings.AutoExposure = true;
    settings.Refraction = true;

    for (usize arm = 1; arm < ArmCount; ++arm)
    {
        settings.Mode = static_cast<DebugView>(arm);
        const FrameTopology topology = ResolveFrameTopology(settings, NoSky);
        CAPTURE(DebugViewNames[arm]);
        CHECK_FALSE(topology.TaaActive);
        CHECK_FALSE(topology.AutoExposureActive);
        CHECK(topology.Dof != DofStages::Full);
        // SSR and refraction survive only on the arms that own them: the Reflections
        // blit needs its trace, and the Bloom arm composites the whole scene.
        CHECK(topology.SsrActive == (settings.Mode == DebugView::Reflections));
        CHECK(topology.RefractionActive == (settings.Mode == DebugView::Bloom));
    }
}

TEST_CASE("frame topology: every sky source selects exactly one display path")
{
    constexpr std::array kinds{SkySourceKind::None, SkySourceKind::Environment,
                               SkySourceKind::Atmosphere, SkySourceKind::Material};
    constexpr std::array tiers{SkyLighting::None, SkyLighting::SH, SkyLighting::IBL};

    const SceneRendererSettings settings;
    for (const SkySourceKind kind : kinds)
    {
        for (const SkyLighting tier : tiers)
        {
            for (const bool baked : {false, true})
            {
                CAPTURE(static_cast<int>(kind));
                CAPTURE(static_cast<int>(tier));
                CAPTURE(baked);
                const FrameTopology topology = ResolveFrameTopology(
                    settings, SkyTopologyInput{.Kind = kind, .Lighting = tier, .IsBaked = baked});

                // At most one of the three display paths runs, and the cubemap path is
                // exactly the cube-backed set.
                const int paths = static_cast<int>(topology.SkyboxWanted) +
                                  static_cast<int>(topology.AtmosphereWanted) +
                                  static_cast<int>(topology.SkyMaterialWanted);
                CHECK(paths <= 1);
                CHECK(topology.SkyboxWanted == topology.CubeBacked);
                CHECK(paths == (kind == SkySourceKind::None ? 0 : 1));

                // Both lighting arms are gated on a cube backing and are mutually
                // exclusive: a source lights through SH or through IBL, never both.
                CHECK_FALSE((topology.SkylightWanted && topology.IblAllowed));
                CHECK(topology.SkylightWanted == (topology.CubeBacked && tier == SkyLighting::SH));
                CHECK(topology.IblAllowed == (topology.CubeBacked && tier == SkyLighting::IBL));

                // A direct material or atmosphere has no cube, so it cannot light.
                if (kind != SkySourceKind::Environment && !baked)
                {
                    CHECK_FALSE(topology.CubeBacked);
                }
            }
        }
    }
}

TEST_CASE("frame topology: a debug arm wires no sky pass at all")
{
    for (usize arm = 1; arm < ArmCount; ++arm)
    {
        if (static_cast<DebugView>(arm) == DebugView::Bloom)
        {
            continue;
        }
        SceneRendererSettings settings;
        settings.Mode = static_cast<DebugView>(arm);
        const FrameTopology topology =
            ResolveFrameTopology(settings, SkyTopologyInput{.Kind = SkySourceKind::Environment,
                                                            .Lighting = SkyLighting::IBL,
                                                            .IsBaked = false});
        CAPTURE(DebugViewNames[arm]);
        CHECK_FALSE(topology.SkyboxWanted);
        CHECK_FALSE(topology.CubeBacked);
        CHECK_FALSE(topology.IblAllowed);
    }
}

TEST_CASE("frame topology: the resolve is stateless")
{
    // No enable edge, no previous-frame memory: the same inputs decide the same
    // topology however many times they are asked. The auto-exposure reset edge lives
    // at the call site precisely so this holds.
    SceneRendererSettings settings;
    settings.AutoExposure = true;
    const SkyTopologyInput sky{
        .Kind = SkySourceKind::Material, .Lighting = SkyLighting::SH, .IsBaked = true};

    const FrameTopology first = ResolveFrameTopology(settings, sky);
    const FrameTopology second = ResolveFrameTopology(settings, sky);
    CHECK(first == second);
    CHECK(first.AutoExposureActive);
    CHECK(second.AutoExposureActive);
}
