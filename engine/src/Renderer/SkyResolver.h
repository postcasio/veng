#pragma once

#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Veng.h>

#include "SkySourceKind.h"

#include <Veng/Math/SphericalHarmonics.h>
#include <Veng/Renderer/AsyncReadback.h>

namespace Veng
{
    class AssetManager;
    class EnvironmentMap;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class GraphicsPipeline;
    class EnvironmentIbl;
    class AtmospherePrecompute;
    class BakedSkyCube;
    class DescriptorSet;

    /// @brief Owns the sky-resolve state machine and the three sky radiance-cube helpers.
    ///
    /// The renderer resolves the scene's one Sky component itself each Execute (the lights model);
    /// this subsystem is that resolve plus the generation work it drives before the graph replays.
    /// It owns the three sky helpers the sky sources produce their radiance cube through — the
    /// image-based-lighting maps (EnvironmentIbl), the procedural-atmosphere LUTs
    /// (AtmospherePrecompute), and the baked-sky cube (BakedSkyCube) — plus the whole state
    /// machine: the resolved source-kind/tier/bake-mode trio the frame-boundary recompile compares
    /// against, every once-per-change dirty gate (the last environment, the last baked
    /// material/atmosphere, the SH environment, the atmosphere LUTs), the projected skylight SH, and
    /// the one-shot ambiguity/degrade warnings.
    ///
    /// Its set layouts must exist before the renderer's CreatePipelines reserves sets (the lighting
    /// layout reserves the IBL set, the sky layout the atmosphere set, the skybox layout the IBL
    /// set), so it is constructed at the same position the three helpers were created, and the
    /// layouts reach the pipeline builds through the owned-helper accessors below. The skybox, sky,
    /// and sky-material passes do not move — Rebuild still creates them, reading which to insert from
    /// the resolved kind/tier here.
    class SkyResolver
    {
    public:
        /// @brief The kind of the sky source resolved from the scene's Sky component.
        ///
        /// An alias for the namespace-scope enum in SkySourceKind.h, which the device-free
        /// frame-topology resolve also names.
        using SkySourceKind = Veng::Renderer::SkySourceKind;

        /// @brief Creates the three sky helpers in dependency order (the bake reuses the IBL layout).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the IBL/atmosphere compute shaders.
        /// @return A new SkyResolver.
        static Unique<SkyResolver> Create(Context& context, AssetManager& assets);

        /// @brief Destroys the three owned sky helpers through the deferred-destruction retire path.
        ~SkyResolver();

        SkyResolver(const SkyResolver&) = delete;
        SkyResolver& operator=(const SkyResolver&) = delete;

        /// @brief Resolves the scene's Sky component into @p view's sky fields for this Execute.
        ///
        /// Reads the first Sky component off view.World (warning once if several exist), fills the
        /// sky fields (environment/atmosphere/material handles, intensity, and — from the scene's
        /// first directional light — the toward-sun direction), and updates the resolved
        /// source-kind/tier/bake-mode. An absent or empty Sky resolves to None and clears the fields
        /// (the flat fallback). A direct source with a lighting tier degrades to background-only,
        /// warned once. Records whether the resolved trio changed for NeedsRecompile() to report; the
        /// recompile itself is the renderer's.
        /// @param view The internal SceneView whose sky fields this fills in place.
        void Resolve(SceneView& view);

        /// @brief Whether the most recent Resolve changed the resolved source-kind/tier/bake-mode.
        ///
        /// The renderer consults this at the frame boundary and calls Rebuild() when true, reusing
        /// the imported output so GetOutput() stays valid.
        [[nodiscard]] bool NeedsRecompile() const { return m_NeedsRecompile; }

        /// @brief Records the atmosphere LUT gate, the baked-sky cube request/copy, and the SH readback.
        ///
        /// The pre-graph sky generation, recorded ahead of the graph the sky and lighting passes
        /// replay: the procedural-atmosphere LUTs (regenerated only on a param change, a baked
        /// atmosphere on its own immediate-submit path), the amortized material/atmosphere cube bake
        /// (requested on the dirty signal, its completed scratch cube copied into the displayed cube),
        /// the SH-tier deferred readback projection, the IBL-tier convolution, and the environment-sky
        /// SH projection.
        ///
        /// The amortized bake's six face renders run one per tick in the GeneratedTextureService pump
        /// (BeginFrame), so they claim their view slots there; this records only the bake request, the
        /// completion-copy, and the non-blocking SH readback, and claims no view slots of its own.
        /// @param cmd         The frame command buffer the direct-path generation records into.
        /// @param view        The resolved SceneView (atmosphere params, sky material, sun direction).
        /// @param skyPipeline The renderer-owned atmosphere sky pipeline the atmosphere bake renders through.
        void RecordPreBeginView(CommandBuffer& cmd, const SceneView& view,
                                const Ref<GraphicsPipeline>& skyPipeline);

        /// @brief Records the environment-sky IBL generation immediately before graph replay.
        ///
        /// Initializes the BRDF LUT and leaves the maps in a sampled layout once, then regenerates
        /// the radiance/irradiance/prefilter maps when the bound environment changes — recorded into
        /// @p cmd after the import bindings are built, before the graph the lighting pass samples them
        /// through.
        /// @param cmd  The frame command buffer the generation records into.
        /// @param view The resolved SceneView whose Environment gates regeneration.
        void RecordPreReplay(CommandBuffer& cmd, const SceneView& view);

        /// @brief Whether the atmosphere LUTs regenerated during the most recent RecordPreBeginView.
        [[nodiscard]] bool DidRegenerateAtmosphereLastFrame() const
        {
            return m_AtmosphereRegeneratedLastFrame;
        }

        /// @brief The resolved sky source-kind the current pass set was built for.
        [[nodiscard]] SkySourceKind GetResolvedKind() const { return m_ResolvedSkyKind; }

        /// @brief The resolved lighting tier the current pass set was built for.
        [[nodiscard]] SkyLighting GetResolvedLighting() const { return m_ResolvedSkyLighting; }

        /// @brief Whether the resolved sky bakes to a radiance cube the skybox path samples.
        [[nodiscard]] bool IsResolvedBaked() const { return m_ResolvedSkyBaked; }

        /// @brief Records whether the last Rebuild wired the SH skylight ambient arm.
        ///
        /// Gates the per-frame SH upload into the view-constants block: true only when the resolved
        /// sky is a cube-backed source on the SH tier.
        /// @param active Whether the SH skylight arm is wired this pass set.
        void SetSkylightActive(bool active) { m_SkylightActive = active; }

        /// @brief Whether the SH skylight ambient arm is wired (gates the per-frame SH upload).
        [[nodiscard]] bool IsSkylightActive() const { return m_SkylightActive; }

        /// @brief The cosine-convolved sky irradiance SH read into the view-constants block each Execute.
        [[nodiscard]] const Sh9& GetSkySh() const { return m_SkySh; }

        /// @brief The image-based-lighting maps (radiance/irradiance/prefilter cubes + BRDF LUT).
        [[nodiscard]] EnvironmentIbl& GetIbl() const { return *m_Ibl; }

        /// @brief The procedural-atmosphere precompute LUTs the sky pass samples.
        [[nodiscard]] AtmospherePrecompute& GetAtmosphere() const { return *m_Atmosphere; }

        /// @brief The consumer set the skybox pass binds for the resolved baked cube (owned or borrowed).
        ///
        /// For a MaterialSky/AtmosphereSky in SkyMode::Baked this is the resolver's own bake cube's
        /// set; for a CubeSky it is the caller-owned cube's set. The renderer binds it into the skybox
        /// pipeline at Rebuild, so a change to which cube is resolved trips NeedsRecompile.
        [[nodiscard]] const Ref<DescriptorSet>& GetSkyConsumerSet() const;

    private:
        SkyResolver(Context& context, AssetManager& assets);

        /// @brief Reduces the just-baked cube and reads it back deferred to reproject the skylight SH.
        ///
        /// The steady-state SH path (every re-bake after the first): records the reduction blit
        /// chain into @p cmd and requests a non-blocking per-face readback of the reduced level.
        /// The completions, delivered a frame or two later, accumulate the six faces and reproject
        /// m_SkySh — so a dirty SH sky costs one bake and is lit by the previous bake's coefficients
        /// for the frame in between. Supersedes any still-in-flight readback so a stale completion
        /// never overwrites fresher coefficients.
        /// @param cmd The frame command buffer the reduction blits are recorded into.
        void BeginDeferredShReadback(CommandBuffer& cmd);

        Context& m_Context;

        /// @brief Image-based-lighting maps + their generation pipelines.
        ///
        /// Owns the radiance/irradiance/prefilter cubemaps, the BRDF LUT, and the consumer set
        /// (set 2) the lighting pass binds. The lighting layout reserves its set layout, so it
        /// exists before the pipelines.
        Unique<EnvironmentIbl> m_Ibl;

        /// @brief Procedural-atmosphere LUTs (transmittance/scattering/irradiance).
        ///
        /// Owns the precompute pipelines + the consumer set (set 1) the sky pass binds. Created
        /// before the sky pipeline so the sky layout can reserve its set layout.
        Unique<AtmospherePrecompute> m_Atmosphere;

        /// @brief Bakes a baked-mode material/atmosphere sky into a radiance cube the skybox samples.
        ///
        /// Owns one radiance cube (at a fixed face resolution), the 1×1 far-plane stand-in depth,
        /// and a consumer set matching the IBL radiance binding. Bake records on the sky dirty
        /// signal; the skybox pass binds GetSet() when the resolved sky is a baked source.
        Unique<BakedSkyCube> m_SkyBake;

        /// @brief The baked cube the current resolve samples: the owned m_SkyBake, a borrowed CubeSky
        ///        cube, or null. Set each Resolve; the skybox consumer set and the recompile read it.
        ///
        /// Non-owning, non-const: the resolver drives the cube's amortized copy (RecordAmortized),
        /// which for a borrowed shared cube is how its bake lands.
        BakedSkyCube* m_ResolvedCube = nullptr;

        /// @brief The resolved cube the current pass set was built against; a change trips the recompile.
        ///
        /// The skybox consumer set is bound at Rebuild, so switching to a different cube (a CubeSky
        /// pointing at a new resource, or owned↔borrowed) must recompile to rebind it.
        BakedSkyCube* m_LastResolvedCube = nullptr;

        /// @brief The cube the lighting tiers last derived from; a change forces a re-derive.
        ///
        /// Distinct from the revision below: switching to a different cube whose revision happens to
        /// match the last seen one must still re-convolve the IBL/SH from the new cube's content.
        BakedSkyCube* m_LastDerivedCube = nullptr;

        /// @brief The resolved cube's revision at the last IBL/SH derive; a change re-derives them.
        ///
        /// A cube's revision advances when a fresh bake lands in it (its owner's or, for a shared
        /// cube, another renderer's), so comparing it drives this resolver's IBL convolution / SH
        /// readback off the shared cube without a per-resolver bake signal.
        u64 m_LastSeenCubeRevision = 0;

        /// @brief The resolved sky kind the current pass set was built for; gates the recompile.
        ///
        /// Compared against each Resolve's freshly-resolved kind; a change recompiles the pass set at
        /// the frame boundary. Initialized to None so a first-frame environment/atmosphere/material
        /// sky triggers the initial wiring.
        SkySourceKind m_ResolvedSkyKind = SkySourceKind::None;

        /// @brief The resolved lighting tier the current pass set was built for; gates the recompile.
        ///
        /// A change to whether the resolved sky lights the scene via SH (the lighting pass's second
        /// ambient arm) recompiles the pass set the same way a source-kind change does.
        SkyLighting m_ResolvedSkyLighting = SkyLighting::None;

        /// @brief Whether the resolved sky bakes to a radiance cube the skybox path samples.
        ///
        /// True for a MaterialSky or AtmosphereSky source in SkyMode::Baked. A resolved change
        /// recompiles the pass set at the frame boundary, the same internal recompile a source-kind
        /// change drives — the two modes render the same sky, so output identity is preserved.
        bool m_ResolvedSkyBaked = false;

        /// @brief Whether the most recent Resolve changed the resolved trio (drives NeedsRecompile).
        bool m_NeedsRecompile = false;

        /// @brief The material a baked material sky last baked; gates the once-per-change re-bake.
        ///
        /// Non-owning — the resolved Sky component keeps the instance resident. The bake re-records
        /// when this frame's material differs, mirroring the atmosphere LUTs' dirty gate. Cleared
        /// when the resolved sky is not a baked material.
        const MaterialInstance* m_LastBakedSkyMaterial = nullptr;

        /// @brief The revision of m_LastBakedSkyMaterial at the last bake; a change re-bakes.
        ///
        /// A material sky reused in place (its params/handles rewritten, the instance pointer
        /// unchanged) advances its revision, so comparing it detects an in-place content change the
        /// pointer compare misses — the material analogue of the atmosphere's param dirty gate.
        u32 m_LastBakedSkyMaterialRevision = 0;

        /// @brief Whether the current bake cube's IBL convolution is up to date; gates re-convolution.
        ///
        /// On the same dirty signal as the bake, a baked source lit via IBL runs GenerateFromCube
        /// over the bake cube; this flag keeps it once-per-change. Cleared when the resolved sky is
        /// not a baked source lit via IBL, so re-entering the tier re-convolves.
        bool m_SkyCubeConvolved = false;

        /// @brief The atmosphere params the baked atmosphere cube was last baked from; gates the re-bake.
        ///
        /// A baked atmosphere re-bakes when this frame's params or sun direction differ (both feed
        /// the baked sky radiance + disc), mirroring the direct atmosphere's LUT dirty gate.
        Atmosphere m_LastBakedAtmosphere;

        /// @brief The sun direction the baked atmosphere cube was last baked from; gates the re-bake.
        vec3 m_LastBakedAtmosphereSun{0.0f, 1.0f, 0.0f};

        /// @brief Whether m_LastBakedAtmosphere holds a baked set (false until the first bake).
        bool m_BakedAtmosphereValid = false;

        /// @brief The environment the cube-projected skylight SH was last built from; gates re-projection.
        ///
        /// Non-owning. An environment sky lit via SH projects its radiance cube to the skylight SH
        /// on the environment-change signal; this gate keeps it once-per-change. Cleared when the
        /// resolved sky is not an environment lit via SH.
        const EnvironmentMap* m_LastSkyShEnvironment = nullptr;

        /// @brief Whether the last Rebuild wired the SH skylight ambient arm into the lighting pass.
        ///
        /// True when the resolved sky is a cube-backed source on the SH tier. Gates the per-frame CPU
        /// project-and-upload in Execute.
        bool m_SkylightActive = false;

        /// @brief Whether the "multiple Sky components" ambiguity warning has already logged.
        ///
        /// The renderer resolves the first walked Sky and warns once; a persistent second component
        /// does not re-log every frame.
        bool m_MultipleSkyWarned = false;

        /// @brief Whether the unsupported-tier degrade warning has already logged.
        ///
        /// A source × tier combination whose lighting machinery is not yet active degrades to
        /// background-only and logs once, not every frame.
        bool m_UnsupportedTierWarned = false;

        /// @brief The environment the IBL maps were last generated from; gates regeneration.
        ///
        /// Generation re-runs only when the bound environment differs from this. Non-owning —
        /// the AssetHandle keeps the EnvironmentMap alive.
        const EnvironmentMap* m_LastEnvironment = nullptr;

        /// @brief The atmosphere the LUTs were last generated from; gates regeneration.
        ///
        /// Generation re-runs only when this frame's Atmosphere differs from this (a field-wise
        /// compare), and only while the atmosphere sky is active — the once-per-change contract.
        Atmosphere m_LastAtmosphere;

        /// @brief Whether m_LastAtmosphere holds a generated set (false until the first Generate).
        bool m_AtmosphereGenerated = false;

        /// @brief Whether the atmosphere LUTs regenerated during the most recent RecordPreBeginView.
        bool m_AtmosphereRegeneratedLastFrame = false;

        /// @brief The cosine-convolved sky irradiance SH uploaded to the lighting constants.
        ///
        /// Projected from the resolved sky's one radiance cube (an environment's, or a baked
        /// material/atmosphere's) and cached; re-projected on that source's dirty signal. Zeroed
        /// until the first projection. Read every Execute the skylight is active to fill the
        /// SkyShCoeffs.
        Sh9 m_SkySh = Sh9::Zero();

        /// @brief Whether m_SkySh holds a projected set (false until the first projection).
        bool m_SkyShValid = false;

        /// @brief A deferred SH readback in flight: the reduced cube read back without blocking.
        ///
        /// One per-face readback rides the frame command buffer; the completions accumulate the six
        /// faces into Faces and, once Remaining reaches zero, reproject m_SkySh. Handles is kept so a
        /// superseding bake — or this resolver's destruction — can cancel a still-in-flight readback
        /// before its completion writes stale coefficients.
        struct ShReadback
        {
            /// @brief The six per-face readbacks in flight, for cancellation.
            vector<AsyncReadbackHandle> Handles;
            /// @brief The reduced cube being assembled, six faces layer-major, RGBA16F.
            vector<u8> Faces;
            /// @brief The reduced level's face edge length in texels.
            u32 FaceSize = 0;
            /// @brief Faces not yet delivered; the projection runs when this reaches zero.
            u32 Remaining = 0;
        };

        /// @brief The deferred SH readback in flight (empty when none is).
        ShReadback m_ShReadback;
    };
}
