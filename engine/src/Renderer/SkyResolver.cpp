#include "SkyResolver.h"

#include "EnvironmentIbl.h"
#include "SkyCubemapBake.h"
#include "AtmospherePrecompute.h"

#include <algorithm>
#include <span>
#include <vector>

#include <glm/geometric.hpp>

#include <Veng/Log.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Types.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/MaterialInstance.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng::Renderer
{
    namespace
    {
        // Linear float HDR format for the baked sky cube: the scene-color format, so its radiance
        // round-trips the skybox sampler.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;

        // Cube face edge length for the baked material sky. Mip 0 suffices for display; the
        // roughness chain the IBL tier needs is convolved from this cube, not baked here. Sized so
        // a point feature a sky material bakes (a star) subtends only a few display pixels — at 512
        // a single face texel already covers ~7 pixels of a 1440p view, reading as a blob.
        constexpr u32 SkyBakeFaceSize = 1024;

        // Field-wise equality of two Atmosphere parameter sets; gates the once-per-change LUT
        // regeneration. An exact compare is right — the LUTs are a pure function of these fields,
        // so any bit change must regenerate and an unchanged set must not.
        bool AtmosphereEquals(const Atmosphere& a, const Atmosphere& b)
        {
            return a.RayleighScattering == b.RayleighScattering &&
                   a.RayleighHeight == b.RayleighHeight && a.MieScattering == b.MieScattering &&
                   a.MieExtinction == b.MieExtinction && a.MieHeight == b.MieHeight &&
                   a.MieAnisotropy == b.MieAnisotropy && a.OzoneAbsorption == b.OzoneAbsorption &&
                   a.OzoneCenter == b.OzoneCenter && a.OzoneWidth == b.OzoneWidth &&
                   a.PlanetRadius == b.PlanetRadius && a.AtmosphereRadius == b.AtmosphereRadius &&
                   a.SunAngularRadius == b.SunAngularRadius && a.SunIrradiance == b.SunIrradiance;
        }
    }

    Unique<SkyResolver> SkyResolver::Create(Context& context, AssetManager& assets)
    {
        return Unique<SkyResolver>(new SkyResolver(context, assets));
    }

    SkyResolver::SkyResolver(Context& context, AssetManager& assets) : m_Context(context)
    {
        // The IBL maps + their consumer set layout exist before the atmosphere and bake so the
        // bake reuses the IBL set layout, and before the renderer's pipelines so the lighting
        // layout can reserve the set (set 2).
        m_Ibl = EnvironmentIbl::Create(m_Context, assets);
        // The atmosphere LUTs + their consumer set layout exist before the renderer's sky pipeline
        // so the sky layout can reserve the set (set 1).
        m_Atmosphere = AtmospherePrecompute::Create(m_Context, assets);
        // The sky-material bake owns its radiance cube + a consumer set matching the IBL radiance
        // binding, so a baked material sky samples through the same skybox pipeline. The cube renders
        // at HdrFormat (the scene-color format) so its radiance round-trips the skybox sampler.
        m_SkyBake =
            SkyCubemapBake::Create(m_Context, m_Ibl->GetSetLayout(), HdrFormat, SkyBakeFaceSize);
    }

    SkyResolver::~SkyResolver()
    {
        // Cancel any deferred SH readback still in flight so its completion — which writes members
        // of this resolver — never fires after it is gone.
        AsyncReadback& readback = m_Context.GetAsyncReadback();
        for (const AsyncReadbackHandle handle : m_ShReadback.Handles)
        {
            readback.Cancel(handle);
        }
    }

    void SkyResolver::Resolve(SceneView& view)
    {
        // Start from the no-sky fallback; the resolved source overrides what it drives.
        view.Environment = {};
        view.EnvironmentIntensity = 1.0f;
        view.AtmosphereEnabled = false;
        view.AtmosphereIntensity = 1.0f;
        view.Atmosphere = Atmosphere{};
        view.SkyMaterial = {};
        view.SkylightIntensity = 1.0f;

        // The toward-sun direction is derived from the scene's first directional light (a sun
        // overhead travels down), so the sky and the direct lighting share one sun. No directional
        // light leaves the default world-up sun.
        view.SunDirection = vec3(0.0f, 1.0f, 0.0f);
        for (auto [entity, light] : view.World.View<Light>())
        {
            if (light.Type == LightType::Directional)
            {
                const f32 length = glm::length(light.Direction);
                if (length > 1e-4f)
                {
                    view.SunDirection = -light.Direction / length;
                }
                break;
            }
        }

        // Resolve the scene's one Sky component; warn once if several exist (the first walked wins).
        const Sky* sky = view.World.TryGetFirst<Sky>();
        u32 skyCount = 0;
        for ([[maybe_unused]] auto [entity, component] : view.World.View<Sky>())
        {
            ++skyCount;
        }
        if (skyCount > 1 && !m_MultipleSkyWarned)
        {
            Log::Warn(
                "SceneRenderer: {} Sky components in the scene; resolving the first, ignoring "
                "the rest.",
                skyCount);
            m_MultipleSkyWarned = true;
        }

        SkySourceKind kind = SkySourceKind::None;
        SkyLighting lighting = SkyLighting::None;
        bool baked = false;

        if (sky != nullptr && sky->Source.HasValue())
        {
            lighting = sky->Lighting;
            const TypeId active = sky->Source.ActiveType();
            const void* source = sky->Source.ActivePtr();
            if (active == TypeIdOf<EnvironmentSky>())
            {
                const auto* environment = static_cast<const EnvironmentSky*>(source);
                view.Environment = environment->Map;
                view.EnvironmentIntensity = sky->Intensity;
                kind = SkySourceKind::Environment;
            }
            else if (active == TypeIdOf<AtmosphereSky>())
            {
                const auto* atmosphere = static_cast<const AtmosphereSky*>(source);
                view.Atmosphere = atmosphere->Params;
                view.AtmosphereEnabled = true;
                view.AtmosphereIntensity = sky->Intensity;
                view.SkylightIntensity = sky->Intensity;
                kind = SkySourceKind::Atmosphere;
                // Baked renders the atmosphere into a radiance cube the skybox path samples; direct
                // runs it per pixel every frame. The two render the same sky; the author picks per
                // the sky's dynamics and the renderer honors it (no silent switch).
                baked = atmosphere->Mode == SkyMode::Baked;
            }
            else if (active == TypeIdOf<MaterialSky>())
            {
                const auto* material = static_cast<const MaterialSky*>(source);
                view.SkyMaterial = material->Material;
                kind = SkySourceKind::Material;
                // Baked runs the material into a radiance cube the skybox path samples; direct runs
                // it per pixel every frame. The two render the same sky; the author picks per the
                // sky's dynamics and the renderer honors it (no silent switch).
                baked = material->Mode == SkyMode::Baked;
                view.EnvironmentIntensity = sky->Intensity;
            }
        }

        // The source × mode × tier resolution table, now in its final unified shape: every source is
        // a radiance-cube producer, so a lighting tier is active exactly when the source fills a
        // cube. An environment always does (its own radiance cube); a material or atmosphere does in
        // Baked mode (the bake cube), and does not in Direct mode (it composites per pixel, no cube
        // to light from). A direct source with a lighting tier therefore degrades to background-only
        // — bake to light — logged once. None is always display-only.
        const bool cubeBacked =
            kind == SkySourceKind::Environment ||
            ((kind == SkySourceKind::Material || kind == SkySourceKind::Atmosphere) && baked);
        const bool tierActive = lighting == SkyLighting::None || cubeBacked;
        if (!tierActive && !m_UnsupportedTierWarned)
        {
            Log::Warn("SceneRenderer: a direct sky cannot light the scene; displaying the sky "
                      "without lighting it — bake the sky (SkyMode::Baked) to light.");
            m_UnsupportedTierWarned = true;
        }

        // A degraded tier resolves to no lighting (display-only); the source still shows. The
        // lighting pass's iblAllowed/skylight flags — set from the resolved tier in Rebuild — gate
        // whether the scene is actually lit.
        const SkyLighting resolvedLighting = tierActive ? lighting : SkyLighting::None;

        // Signal the internal recompile on a resolved source-kind, tier, or bake-mode change — the
        // frame boundary the pass set flips at, reusing the imported output (identity preserved). A
        // direct↔baked flip is a resolved-source change: the same recompile a kind change drives.
        // The renderer consults NeedsRecompile() and does the Rebuild itself.
        m_NeedsRecompile = kind != m_ResolvedSkyKind || resolvedLighting != m_ResolvedSkyLighting ||
                           baked != m_ResolvedSkyBaked;
        m_ResolvedSkyKind = kind;
        m_ResolvedSkyLighting = resolvedLighting;
        m_ResolvedSkyBaked = baked;
    }

    void SkyResolver::RecordPreBeginView(CommandBuffer& cmd, const SceneView& view,
                                         const Ref<GraphicsPipeline>& skyPipeline)
    {
        // Procedural atmosphere: transition the LUTs to a sampled layout once, then regenerate them
        // only when this frame's Atmosphere differs from the last generated set — the once-per-change
        // contract (the sun direction is a runtime push, not a precompute input). Gated on the
        // resolved sky being an atmosphere so the cost is absent on the shipping path. Recorded here,
        // before the atmosphere bake below and before the graph the direct pass samples them
        // through, so the LUTs are resident for either display mode. A baked atmosphere generates
        // them through a self-contained immediate submit, so they are device-resident before the
        // bake's own immediate-submit readback samples them (the frame command buffer has not been
        // submitted at that point); a direct atmosphere records into the frame command buffer, which
        // the direct sky pass samples through in-order later this frame.
        m_AtmosphereRegeneratedLastFrame = false;
        if (m_ResolvedSkyKind == SkySourceKind::Atmosphere)
        {
            const bool regenerate =
                !m_AtmosphereGenerated || !AtmosphereEquals(view.Atmosphere, m_LastAtmosphere);
            if (m_ResolvedSkyBaked)
            {
                m_Context.ImmediateCommands(
                    [&](CommandBuffer& lutCmd)
                    {
                        m_Atmosphere->EnsureInitialized(lutCmd);
                        if (regenerate)
                        {
                            m_Atmosphere->Generate(lutCmd, view.Atmosphere);
                        }
                    });
            }
            else
            {
                m_Atmosphere->EnsureInitialized(cmd);
                if (regenerate)
                {
                    m_Atmosphere->Generate(cmd, view.Atmosphere);
                }
            }
            if (regenerate)
            {
                m_LastAtmosphere = view.Atmosphere;
                m_AtmosphereGenerated = true;
                m_AtmosphereRegeneratedLastFrame = true;
            }
        }

        // Bake a baked-mode sky into its radiance cube on the sky dirty signal, so a static sky
        // bakes once and the skybox pass samples the cube this frame. Every baked source — an
        // authored material or the procedural atmosphere — fills the same cube; display and any
        // ambient tier then read that one cube, so they agree by construction. Recorded into cmd
        // before the graph the skybox pass samples the cube through, and before the scene claims its
        // own view slot below — the bake writes six face view-constants regions into distinct view
        // slots, so it must run ahead of the frame's own view claim. A direct or no-sky source bakes
        // nothing.
        const bool bakedMaterial = m_ResolvedSkyKind == SkySourceKind::Material &&
                                   m_ResolvedSkyBaked && view.SkyMaterial.IsLoaded();
        const bool bakedAtmosphere =
            m_ResolvedSkyKind == SkySourceKind::Atmosphere && m_ResolvedSkyBaked;
        const bool baking = bakedMaterial || bakedAtmosphere;

        // A bake is all-or-nothing against the frame's remaining view budget: a half-filled cube
        // marked clean would show a permanently wrong sky, so a frame without room for every face
        // records no bake at all and leaves the dirty signal standing for the next frame with room.
        // This frame's skybox then samples the cube as the last bake left it. The SH tier's first
        // bake needs twice as many, because its cold-start readback bakes a second cube through an
        // immediate submit that still draws from this frame's view budget; every later SH re-bake
        // reads the display bake back deferred and claims only the display bake's own six.
        const bool shColdSeed = m_ResolvedSkyLighting == SkyLighting::SH && !m_SkyShValid;
        const u32 bakeSlots =
            shColdSeed ? 2 * SkyCubemapBake::CubeFaces : SkyCubemapBake::CubeFaces;
        const bool bakeAffordable =
            m_Context.GetBindlessRegistry().GetRemainingViews() >= bakeSlots;

        if (baking && bakeAffordable)
        {

            // The dirty signal: for a material, its resolved instance changing; for the atmosphere,
            // its params or the sun direction changing (both feed the baked sky radiance + disc).
            const MaterialInstance* material = bakedMaterial ? view.SkyMaterial.Get() : nullptr;
            bool bakeDirty = false;
            if (bakedMaterial)
            {
                // The instance may be reused in place — its params/star buffer rewritten while the
                // pointer stays the same — so a revision change re-bakes as a swap does.
                bakeDirty = material != m_LastBakedSkyMaterial ||
                            material->GetRevision() != m_LastBakedSkyMaterialRevision;
            }
            else
            {
                bakeDirty = !m_BakedAtmosphereValid ||
                            !AtmosphereEquals(view.Atmosphere, m_LastBakedAtmosphere) ||
                            view.SunDirection != m_LastBakedAtmosphereSun;
            }

            // The SH tier's cold start runs first, before the frame's display bake records into cmd:
            // while no coefficients have been projected yet — a sky's first bake, or a tier just
            // switched to SH — there is nothing to defer to, so a single synchronous readback of the
            // reduced cube seeds them. Its immediate submit (bake + reduce + copy + download) records
            // barriers off the persistent image-layout tracker, so it must see the tracker as the
            // display bake left it last frame, not as this frame's not-yet-submitted display bake
            // would. It bakes its own cube, so it seeds even on a frame the display bake is clean.
            if (shColdSeed)
            {
                const vector<u8> faces =
                    bakedMaterial
                        ? m_SkyBake->BakeAndDownload(*material)
                        : m_SkyBake->BakeAtmosphereAndDownload(skyPipeline, m_Atmosphere->GetSet(),
                                                               view.Atmosphere, view.SunDirection,
                                                               view.AtmosphereIntensity);
                m_SkySh = EnvironmentIbl::ProjectCubeToIrradianceSh(
                    faces, m_SkyBake->GetShReadbackFaceSize());
                m_SkyShValid = true;
            }

            // The display bake into the frame command buffer, so the skybox pass samples the cube
            // this frame. Recorded before the deferred SH readback and the IBL convolution (below),
            // both of which read the cube this bake fills.
            if (bakeDirty)
            {
                if (bakedMaterial)
                {
                    m_SkyBake->Bake(cmd, *material);
                }
                else
                {
                    m_SkyBake->BakeAtmosphere(cmd, skyPipeline, m_Atmosphere->GetSet(),
                                              view.Atmosphere, view.SunDirection,
                                              view.AtmosphereIntensity);
                    m_LastBakedAtmosphere = view.Atmosphere;
                    m_LastBakedAtmosphereSun = view.SunDirection;
                    m_BakedAtmosphereValid = true;
                }
                m_LastBakedSkyMaterial = material;
                m_LastBakedSkyMaterialRevision = material != nullptr ? material->GetRevision() : 0;
            }

            // The steady-state SH readback: every re-bake after the first reads the display bake's
            // own cube back without blocking, from a reduced level, with the projection deferred a
            // frame or two — so a static or occasionally-rebaked SH sky costs one bake. Skipped on
            // the cold bake, whose coefficients were seeded synchronously above.
            if (bakeDirty && m_ResolvedSkyLighting == SkyLighting::SH && !shColdSeed)
            {
                BeginDeferredShReadback(cmd);
            }

            // IBL convolves the freshly-filled bake cube into the split-sum maps in this command
            // buffer, on the same bake dirty signal, so a static sky pays it once.
            if (m_ResolvedSkyLighting == SkyLighting::IBL)
            {
                if (bakeDirty || !m_SkyCubeConvolved)
                {
                    m_Ibl->EnsureInitialized(cmd);
                    m_Ibl->GenerateFromCube(cmd, m_SkyBake->GetCubeView(),
                                            m_SkyBake->GetFaceSize());
                    m_SkyCubeConvolved = true;
                }
            }
            else
            {
                m_SkyCubeConvolved = false;
            }
        }
        else if (!baking)
        {
            m_LastBakedSkyMaterial = nullptr;
            m_SkyCubeConvolved = false;
            m_BakedAtmosphereValid = false;
        }

        // An environment sky on the SH tier lights the diffuse term from its radiance cube — the
        // same cube the skybox samples. Project it to the skylight coefficients on the
        // environment-change signal, before the view-constants write below folds m_SkySh in; a
        // static environment projects once. The environment IBL tier convolves in the main command
        // buffer below (its skybox radiance is already produced there).
        if (m_ResolvedSkyKind == SkySourceKind::Environment &&
            m_ResolvedSkyLighting == SkyLighting::SH && view.Environment.IsLoaded())
        {
            const EnvironmentMap* environment = view.Environment.Get();
            if (environment != m_LastSkyShEnvironment)
            {
                m_SkySh = m_Ibl->ProjectEnvironmentToIrradianceSh(*environment);
                m_SkyShValid = true;
                m_LastSkyShEnvironment = environment;
            }
        }
        else
        {
            m_LastSkyShEnvironment = nullptr;
        }
    }

    void SkyResolver::BeginDeferredShReadback(CommandBuffer& cmd)
    {
        AsyncReadback& readback = m_Context.GetAsyncReadback();

        // Supersede any still-in-flight readback: its completion would write coefficients this bake
        // has already replaced, and its staging buffers are no longer wanted.
        for (const AsyncReadbackHandle handle : m_ShReadback.Handles)
        {
            readback.Cancel(handle);
        }
        m_ShReadback.Handles.clear();

        // Reduce the just-baked display cube to the readback level in this frame's command buffer.
        m_SkyBake->RecordReductionMips(cmd);

        const u32 faceSize = m_SkyBake->GetShReadbackFaceSize();
        const usize faceBytes = static_cast<usize>(faceSize) * faceSize * 8; // RGBA16F
        m_ShReadback.FaceSize = faceSize;
        m_ShReadback.Faces.assign(faceBytes * SkyCubemapBake::CubeFaces, 0);
        m_ShReadback.Remaining = SkyCubemapBake::CubeFaces;

        // One non-blocking readback per face of the reduced level; the completions land together a
        // few frames on, accumulate the faces layer-major, and reproject once the last arrives.
        for (u32 face = 0; face < SkyCubemapBake::CubeFaces; ++face)
        {
            const AsyncReadbackHandle handle = readback.Request({
                .Name = "Sky SH Readback",
                .Image = m_SkyBake->GetCubeImage(),
                .MipLevel = m_SkyBake->GetShReadbackMipLevel(),
                .ArrayLayer = face,
                .RestoreTo = AccessKind::Sample,
                .OnComplete =
                    [this, face, faceBytes](const std::span<const u8> bytes)
                {
                    if (bytes.size() == faceBytes)
                    {
                        std::ranges::copy(
                            bytes, m_ShReadback.Faces.begin() +
                                       static_cast<vector<u8>::difference_type>(face * faceBytes));
                    }
                    if (m_ShReadback.Remaining > 0 && --m_ShReadback.Remaining == 0)
                    {
                        m_SkySh = EnvironmentIbl::ProjectCubeToIrradianceSh(m_ShReadback.Faces,
                                                                            m_ShReadback.FaceSize);
                        m_SkyShValid = true;
                    }
                },
            });
            m_ShReadback.Handles.push_back(handle);
        }
    }

    void SkyResolver::RecordPreReplay(CommandBuffer& cmd, const SceneView& view)
    {
        // Image-based lighting: initialize the BRDF LUT + leave the maps in a sampled layout
        // once, then (re)generate the radiance/irradiance/prefilter maps when the bound
        // environment changes — a one-time cost recorded before the graph the lighting pass
        // samples them through. Recorded into cmd before the graph so the cubes are resident
        // and in their sampled layout when the lighting pass runs.
        m_Ibl->EnsureInitialized(cmd);
        const EnvironmentMap* environment =
            view.Environment.IsLoaded() ? view.Environment.Get() : nullptr;
        if (environment != nullptr && environment != m_LastEnvironment)
        {
            m_Ibl->Generate(cmd, *environment);
        }
        m_LastEnvironment = environment;
    }
}
