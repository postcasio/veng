#include "FrameTopology.h"

namespace Veng::Renderer
{
    FrameTopology ResolveFrameTopology(const SceneRendererSettings& settings,
                                       const SkyTopologyInput& sky)
    {
        FrameTopology topology;

        topology.DebugBloom = settings.Mode == DebugView::Bloom;
        topology.BloomActive =
            (settings.Mode == DebugView::Final && settings.Bloom) || topology.DebugBloom;

        // Auto-exposure meters the lit HDR in the Final path only (a debug arm has no tonemap tail
        // to drive).
        topology.AutoExposureActive = settings.Mode == DebugView::Final && settings.AutoExposure;

        // TAA is a Final-only resolve: it inserts the resolve + history-copy passes between
        // lighting and the tonemap tail and routes lighting into a separate lit target.
        topology.TaaActive = settings.Mode == DebugView::Final && settings.TAA;

        topology.DebugShadow = settings.Mode == DebugView::Shadows;
        topology.DebugAo = settings.Mode == DebugView::AO;
        // Cascades debug needs the shadow pass wired so cascade constants are written.
        topology.DebugCascades = settings.Mode == DebugView::Cascades;
        topology.DebugPunctual = settings.Mode == DebugView::PunctualShadows;

        // The Final view and the Bloom debug arm both composite the full scene before the bloom
        // tail, so both fold in the same contributors (shadows, SSAO, sky — plus the g-buffer
        // emissive channel the lighting pass adds) — the Bloom pyramid then blooms the same HDR the
        // Final view would.
        topology.SceneComposited = settings.Mode == DebugView::Final || topology.DebugBloom;

        topology.ShadowActive = (topology.SceneComposited && settings.Shadows) ||
                                topology.DebugShadow || topology.DebugCascades;
        topology.PunctualShadowActive =
            (topology.SceneComposited && settings.PunctualShadows) || topology.DebugPunctual;

        topology.SsaoFold = topology.SceneComposited && settings.AO;
        topology.SsaoActive = topology.SsaoFold || topology.DebugAo;

        // The sky pass topology is driven by the resolved Sky component, not a consumer toggle, and
        // reduces to one rule: a source fills the radiance cube (static) or composites direct
        // (dynamic), and the cube-backed sources share one display path. Every cube-backed source —
        // an environment (its own radiance cube), a baked material/atmosphere (the bake cube), or a
        // CubeSky (a caller-owned baked cube) — displays through the cubemap skybox pass; the two
        // direct per-pixel passes (SkyMaterialScenePass for a direct material, SkyScenePass for a
        // direct atmosphere) survive only as the authored dynamic modes. The SH skylight arm folds
        // into the lighting pass for any cube-backed source on the SH tier; the resolver's own
        // skylight-active flag then gates the per-frame upload.
        topology.BakedSkyWanted =
            topology.SceneComposited &&
            (((sky.Kind == SkySourceKind::Material || sky.Kind == SkySourceKind::Atmosphere) &&
              sky.IsBaked) ||
             sky.Kind == SkySourceKind::Cube);
        topology.CubeBacked =
            (topology.SceneComposited && sky.Kind == SkySourceKind::Environment) ||
            topology.BakedSkyWanted;
        topology.SkyboxWanted = topology.CubeBacked;
        topology.AtmosphereWanted =
            topology.SceneComposited && sky.Kind == SkySourceKind::Atmosphere && !sky.IsBaked;
        topology.SkyMaterialWanted =
            topology.SceneComposited && sky.Kind == SkySourceKind::Material && !sky.IsBaked;
        topology.SkylightWanted = topology.CubeBacked && sky.Lighting == SkyLighting::SH;

        // IBL lights the scene when the resolved sky is a cube-backed source on the IBL tier — an
        // environment (convolved from its equirect cube) or a baked material/atmosphere (convolved
        // from its bake cube). Either fills the IBL consumer set the lighting pass binds; a
        // display-only source (any other tier) shows its sky without lighting from it.
        topology.IblAllowed = topology.CubeBacked && sky.Lighting == SkyLighting::IBL;

        // SSR is a Final-only effect plus its own debug arm; the debug arm force-wires the
        // trace so the raw reflection target is visible regardless of the Settings.SSR toggle.
        topology.DebugReflections = settings.Mode == DebugView::Reflections;
        topology.SsrActive =
            (settings.Mode == DebugView::Final && settings.SSR) || topology.DebugReflections;

        // Depth of field takes the same gate shape, but the debug arm wires only the chain's first
        // two stages: the gather, fill, and composite stay off, so inspecting the circle of
        // confusion never alters the HDR tail.
        if (settings.Mode == DebugView::CoC)
        {
            topology.Dof = DofStages::CocOnly;
        }
        else if (settings.Mode == DebugView::Final && settings.DepthOfField)
        {
            topology.Dof = DofStages::Full;
        }

        // The scene-color copy runs wherever the translucent composite does (the Final view and
        // the Bloom debug arm), so a refractive material behaves identically in both.
        topology.RefractionActive = topology.SceneComposited && settings.Refraction;

        return topology;
    }
}
