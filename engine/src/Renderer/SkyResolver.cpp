#include "SkyResolver.h"

#include "EnvironmentIbl.h"
#include "AtmospherePrecompute.h"
#include "SkySourceResolve.h"

#include <Veng/Renderer/BakedSkyCube.h>
#include <Veng/Renderer/DescriptorSet.h>

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
            BakedSkyCube::Create(m_Context, m_Ibl->GetSetLayout(), HdrFormat, SkyBakeFaceSize);
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

    const Ref<DescriptorSet>& SkyResolver::GetSkyConsumerSet() const
    {
        // The resolved baked cube's set — its own for a baked material/atmosphere, the borrowed
        // cube's for a CubeSky. Falls back to the owned cube's set when nothing baked is resolved (the
        // renderer only binds this when the topology says the sky is baked, so the fallback is inert).
        return m_ResolvedCube != nullptr ? m_ResolvedCube->GetSet() : m_SkyBake->GetSet();
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
        view.SkyCube = nullptr;
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

        const ResolvedSkySource resolved = ResolveSkySource(sky, view);
        const SkySourceKind kind = resolved.Kind;
        const SkyLighting lighting = resolved.Lighting;
        const bool baked = resolved.Baked;

        // The source × mode × tier resolution table, now in its final unified shape: every source is
        // a radiance-cube producer, so a lighting tier is active exactly when the source fills a
        // cube. An environment always does (its own radiance cube); a material or atmosphere does in
        // Baked mode (the bake cube), and does not in Direct mode (it composites per pixel, no cube
        // to light from). A direct source with a lighting tier therefore degrades to background-only
        // — bake to light — logged once. None is always display-only.
        const bool cubeBacked =
            kind == SkySourceKind::Environment || kind == SkySourceKind::Cube ||
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
        // The baked cube this frame samples: a CubeSky borrows the caller's, a baked
        // material/atmosphere uses the resolver's own, everything else has none. The skybox consumer
        // set is bound at Rebuild from this, so a change to which cube is resolved recompiles.
        const bool ownedBaked =
            (kind == SkySourceKind::Material || kind == SkySourceKind::Atmosphere) && baked;
        m_ResolvedCube = kind == SkySourceKind::Cube ? view.SkyCube
                         : ownedBaked                ? m_SkyBake.get()
                                                     : nullptr;

        m_NeedsRecompile = kind != m_ResolvedSkyKind || resolvedLighting != m_ResolvedSkyLighting ||
                           baked != m_ResolvedSkyBaked || m_ResolvedCube != m_LastResolvedCube;
        m_ResolvedSkyKind = kind;
        m_ResolvedSkyLighting = resolvedLighting;
        m_ResolvedSkyBaked = baked;
        m_LastResolvedCube = m_ResolvedCube;
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

        // Establish this frame's baked cube. A MaterialSky/AtmosphereSky in SkyMode::Baked bakes into
        // the resolver's own cube on the sky dirty signal; a CubeSky borrows a cube the caller bakes
        // itself. The active cube is m_ResolvedCube (owned or borrowed) — the skybox samples it, the
        // lighting tiers derive from it, and its amortized fill lands the same way for either. A
        // direct or no-sky source bakes nothing. The bake is amortized through GeneratedTextureService:
        // a job fills a scratch cube one tile per tick and RecordAmortized copies it into the displayed
        // cube once complete, so a jump never blocks a frame on six fullscreen sky evaluations and the
        // previous cube keeps being sampled until the new one lands.
        const bool bakedMaterial = m_ResolvedSkyKind == SkySourceKind::Material &&
                                   m_ResolvedSkyBaked && view.SkyMaterial.IsLoaded();
        const bool bakedAtmosphere =
            m_ResolvedSkyKind == SkySourceKind::Atmosphere && m_ResolvedSkyBaked;
        const bool ownedBaked = (m_ResolvedSkyKind == SkySourceKind::Material ||
                                 m_ResolvedSkyKind == SkySourceKind::Atmosphere) &&
                                m_ResolvedSkyBaked;

        // Request a re-bake of the resolver's own cube on the dirty signal — the material's instance
        // or in-place revision changing, or the atmosphere's params/sun changing.
        if (bakedMaterial)
        {
            const MaterialInstance* material = view.SkyMaterial.Get();
            const bool bakeDirty =
                material != m_LastBakedSkyMaterial ||
                (material != nullptr && material->GetRevision() != m_LastBakedSkyMaterialRevision);
            if (bakeDirty)
            {
                m_SkyBake->RequestBake(m_Context.GetGeneratedTextures(), *material);
                m_LastBakedSkyMaterial = material;
                m_LastBakedSkyMaterialRevision = material != nullptr ? material->GetRevision() : 0;
            }
        }
        else if (bakedAtmosphere)
        {
            const bool bakeDirty = !m_BakedAtmosphereValid ||
                                   !AtmosphereEquals(view.Atmosphere, m_LastBakedAtmosphere) ||
                                   view.SunDirection != m_LastBakedAtmosphereSun;
            if (bakeDirty)
            {
                m_SkyBake->RequestBakeAtmosphere(m_Context.GetGeneratedTextures(), skyPipeline,
                                                 m_Atmosphere->GetSet(), view.Atmosphere,
                                                 view.SunDirection, view.AtmosphereIntensity);
                m_LastBakedAtmosphere = view.Atmosphere;
                m_LastBakedAtmosphereSun = view.SunDirection;
                m_BakedAtmosphereValid = true;
            }
        }

        // Not owning a bake this frame (a CubeSky borrow, an environment, a direct/no source): drop
        // any owned bake in flight. The owned cube keeps its last content, so a returning owned sky
        // re-bakes on its own dirty gate rather than losing it here.
        if (!ownedBaked)
        {
            m_SkyBake->AbandonBake();
            m_LastBakedSkyMaterial = nullptr;
            m_BakedAtmosphereValid = false;
        }

        if (m_ResolvedCube != nullptr)
        {
            // Drive the amortized copy of a completed bake into the displayed cube. Idempotent — for a
            // shared cube several renderers call this and the first with a landed bake does the copy,
            // the rest no-op — and a landed copy advances the cube's revision.
            m_ResolvedCube->RecordAmortized(cmd);

            // Re-derive the lighting tiers off the cube when its content changed — the revision moved
            // (a fresh bake landed, this resolver's own or, for a shared cube, another renderer's) or
            // this resolver switched to a different cube — and only once the cube holds a real bake.
            const bool cubeChanged = m_ResolvedCube != m_LastDerivedCube ||
                                     m_ResolvedCube->GetRevision() != m_LastSeenCubeRevision;
            m_LastDerivedCube = m_ResolvedCube;
            m_LastSeenCubeRevision = m_ResolvedCube->GetRevision();

            // The SH readback: the cube is read back reduced, without blocking the render thread, the
            // projection deferred a frame or two — so a static or occasionally re-baked SH sky costs
            // one bake, and the SH ambient arrives a few frames after the first bake lands.
            if (m_ResolvedCube->IsBaked() && m_ResolvedSkyLighting == SkyLighting::SH &&
                cubeChanged)
            {
                BeginDeferredShReadback(cmd);
            }

            // IBL convolves the cube into the split-sum maps when its content changes or on first
            // entry to the tier with a real bake — a static sky pays it once.
            if (m_ResolvedSkyLighting == SkyLighting::IBL)
            {
                if (m_ResolvedCube->IsBaked() && (cubeChanged || !m_SkyCubeConvolved))
                {
                    m_Ibl->EnsureInitialized(cmd);
                    m_Ibl->GenerateFromCube(cmd, m_ResolvedCube->GetCubeView(),
                                            m_ResolvedCube->GetFaceSize());
                    m_SkyCubeConvolved = true;
                }
            }
            else
            {
                m_SkyCubeConvolved = false;
            }
        }
        else
        {
            m_SkyCubeConvolved = false;
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

        // Reduce the resolved cube (owned or borrowed) to the readback level in this command buffer.
        m_ResolvedCube->RecordReductionMips(cmd);

        const u32 faceSize = m_ResolvedCube->GetShReadbackFaceSize();
        const usize faceBytes = static_cast<usize>(faceSize) * faceSize * 8; // RGBA16F
        m_ShReadback.FaceSize = faceSize;
        m_ShReadback.Faces.assign(faceBytes * BakedSkyCube::CubeFaces, 0);
        m_ShReadback.Remaining = BakedSkyCube::CubeFaces;

        // One non-blocking readback per face of the reduced level; the completions land together a
        // few frames on, accumulate the faces layer-major, and reproject once the last arrives.
        for (u32 face = 0; face < BakedSkyCube::CubeFaces; ++face)
        {
            const AsyncReadbackHandle handle = readback.Request({
                .Name = "Sky SH Readback",
                .Image = m_ResolvedCube->GetCubeImage(),
                .MipLevel = m_ResolvedCube->GetShReadbackMipLevel(),
                .ArrayLayer = face,
                .RestoreTo = AccessKind::SampleAny,
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
