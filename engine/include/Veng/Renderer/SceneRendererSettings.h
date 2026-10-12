#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/MeshLod.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/ShadowCascades.h>

#include <array>

/// @brief The SceneRenderer topology/sizing knobs and its debug-view vocabulary.
///
/// The public configuration surface of SceneRenderer, split out of SceneRenderer.h so a
/// settings panel or debug UI can include it without the renderer class. SceneRenderer.h
/// re-includes this header, so no consumer migrates.
namespace Veng::Renderer
{
    /// @brief Maximum number of simultaneously shadowed point/spot lights.
    ///
    /// The first N shadow-casting punctual lights (by per-frame selection) receive a
    /// shadow map; the rest are lit without shadows. A point light costs six cube-face
    /// redraws of its caster set, so N bounds the punctual shadow atlas and the lighting
    /// loop's sample set at 6N depth tiles.
    inline constexpr u32 MaxShadowedPunctual = 4;

    /// @brief Selects which result the renderer produces, re-wiring the pass set.
    ///
    /// Final is the full deferred chain (g-buffer → lighting → tonemap). All other
    /// values terminate the chain after the g-buffer with a single fullscreen debug blit
    /// of one channel or target. A change here is a topology change driven through
    /// Configure → recompile.
    ///
    /// Roughness/Metallic/Occlusion read the packed G2 ORM channels (R=occlusion,
    /// G=roughness, B=metallic). AO reads the SSAO target and Shadows the directional
    /// shadow map; the producing pass is force-wired in those modes regardless of the
    /// Settings.AO / Settings.Shadows toggle. Cascades tints each fragment by the
    /// cascade its view-space depth selects (0 red, 1 green, 2 blue, 3 yellow),
    /// force-wiring the shadow pass so cascade constants are present. PunctualShadows
    /// blits the punctual shadow atlas (raw depth), force-wiring the punctual shadow pass.
    /// Bloom runs the lighting pass and the bloom pyramid sweep, then blits pyramid mip 0
    /// after the up-sweep — the accumulated bloom contribution before composite —
    /// force-wiring the bloom pass regardless of the Settings.Bloom toggle. MotionVectors blits
    /// the per-object velocity g-buffer channel (G3, written by the surface pass every frame)
    /// colorized as an optical-flow field. Emissive blits the HDR emissive g-buffer channel (G4,
    /// written by the surface pass every frame) — the authored emissive contribution alone,
    /// independent of lighting. EnvironmentIrradiance and EnvironmentSource fill the whole frame with
    /// a cube along each view ray — the diffuse irradiance cube the lighting's diffuse IBL reads, and
    /// the raw cube it convolved from (a scene-lighting capture, else the sky's own cube) — so the
    /// environment lighting the scene is inspectable whatever geometry is in front of it.
    /// IblContribution runs the full lighting pass and shows the IBL ambient term alone per pixel
    /// (diffuse + specular IBL × ambient occlusion), force-wiring the IBL path so the contribution is
    /// present whatever the sky tier resolves to. Reflections and ReflectionCoverage composite the
    /// lit opaque scene as Final does and force-wire the screen-space reflection chain regardless of
    /// the Settings.SSR toggle: Reflections blits the chain's denoised reflection alone (the
    /// reflected radiance, before the composite's BRDF weighting), and ReflectionCoverage colours
    /// each pixel by what the chain's tile classifier gave it — dark grey nothing, blue the
    /// environment alone (too rough to trace), green its own ray, yellow a ray traced for its temporal
    /// variance alone, cyan a quad neighbour's ray, white a mirror ray (which skips the denoiser), and
    /// red a glossy pixel that wanted a neighbour's ray no neighbour traced.
    enum class DebugView : u8
    {
        /// @brief Full deferred pipeline output.
        Final,
        /// @brief G0 base color channel.
        Albedo,
        /// @brief G1 world-space normal channel.
        Normal,
        /// @brief Depth buffer visualized as a linear grey scale.
        Depth,
        /// @brief G2 roughness channel (green).
        Roughness,
        /// @brief G2 metallic channel (blue).
        Metallic,
        /// @brief G2 ambient occlusion channel (red).
        Occlusion,
        /// @brief SSAO target (force-wires the SSAO pass).
        AO,
        /// @brief Directional shadow atlas raw depth (force-wires the shadow pass).
        Shadows,
        /// @brief Per-fragment cascade tint (force-wires the shadow pass).
        Cascades,
        /// @brief Punctual shadow atlas raw depth (force-wires the punctual shadow pass).
        PunctualShadows,
        /// @brief Accumulated bloom pyramid mip 0 after the up-sweep (force-wires the bloom pass).
        Bloom,
        /// @brief Per-object velocity (g-buffer channel G3) as an optical-flow field.
        MotionVectors,
        /// @brief The authored emissive g-buffer channel (G4) alone.
        Emissive,
        /// @brief Signed circle of confusion (force-wires the depth-of-field prefilter + tiles).
        CoC,
        /// @brief The diffuse irradiance cube (cosine-convolved), sampled fullscreen along the view ray.
        EnvironmentIrradiance,
        /// @brief The raw cube the IBL convolved from (a lighting capture or source, else the sky cube).
        EnvironmentSource,
        /// @brief The scene's per-pixel IBL ambient contribution alone (diffuse + specular IBL * AO).
        IblContribution,
        /// @brief The screen-space reflection chain's denoised reflection alone (force-wires the
        ///        chain).
        Reflections,
        /// @brief What the reflection chain's classifier gave each pixel, colour-coded (force-wires
        ///        the chain).
        ReflectionCoverage,
    };

    /// @brief Display names for the DebugView arms, indexed by enum value.
    ///
    /// The single source of truth for the "View" combo in both the engine debug panel and the
    /// editor viewport: entry N is the name of DebugView N, so a combo's selected index casts
    /// straight to the enum. The static_assert below keeps it in lockstep with the enum.
    inline constexpr std::array<string_view, 20> DebugViewNames{"Final",
                                                                "Albedo",
                                                                "Normal",
                                                                "Depth",
                                                                "Roughness",
                                                                "Metallic",
                                                                "Occlusion",
                                                                "AO",
                                                                "Shadows",
                                                                "Cascades",
                                                                "Punctual shadows",
                                                                "Bloom",
                                                                "Motion vectors",
                                                                "Emissive",
                                                                "Circle of confusion",
                                                                "IBL irradiance",
                                                                "IBL source cube",
                                                                "IBL contribution",
                                                                "Reflections",
                                                                "Reflection coverage"};
    static_assert(DebugViewNames.size() == static_cast<usize>(DebugView::ReflectionCoverage) + 1,
                  "DebugViewNames must list every DebugView arm in declaration order.");

    /// @brief Selects the bloom pyramid's down/up filter kernel.
    ///
    /// The kernel choice changes the per-level compute shader, so it is a topology knob
    /// (a Configure recompile), like the Bloom toggle. Cod is the reference filter the
    /// golden is blessed against; Kawase is the bandwidth-optimized alternative.
    enum class BloomKernel : u8
    {
        /// @brief Call of Duty / Jimenez 13-tap downsample + 3×3 tent upsample dual filter.
        Cod,
        /// @brief Dual Kawase 5-tap downsample + 8-tap upsample bilinear filter (Bjørge),
        ///        designed for the bandwidth-bound tile-based GPUs veng primarily targets.
        Kawase,
    };

    /// @brief Selects the full-screen anti-aliasing resolve, or none.
    ///
    /// The resolves are **mutually exclusive** — a frame wires at most one. A change re-wires
    /// the pass set through the Configure recompile. FXAA and CMAA2 are spatial post-tonemap passes
    /// reading the tonemapped LDR; TAA and TAAU are the HDR-space temporal resolve (they additionally
    /// jitter the projection and route lighting into a separate lit target — see the TAA field). The
    /// per-object velocity channel (G3) is written every frame regardless, so the temporal resolve
    /// needs no velocity prepass. **Supersampling is not a mode here**: it is orthogonal, driven by
    /// the viewport's MaxAllocationScale, and composes with any mode (SSAA under CMAA2, say).
    ///
    /// TAA and TAAU share the whole temporal machinery (jitter, history, resolve, history-copy) and
    /// the render scale means the same thing under both — it sizes the render allocation the scene
    /// rasterizes into, which is separate from the post-resolve allocation the tail runs at in every
    /// mode. What differs is **where the resolve's output lands**. TAA resolves at the render
    /// allocation, so its history is render-allocation-sized and a spatial promotion carries the
    /// result up to the post-resolve allocation: the reconstruction is temporal, the upscale
    /// spatial. TAAU resolves at the post-resolve allocation, so its history is allocation-sized and
    /// **the resolve is the promotion** — a jittered sub-native render accumulated straight into the
    /// native image, with no second resample behind it. A TAAU frame that also wires a composited
    /// depth-of-field chain resolves like TAA instead (the chain reads render-resolution depth beside
    /// the scene colour), degrading the reconstruction to spatial and keeping everything else.
    enum class AntiAliasingMode : u8
    {
        /// @brief No anti-aliasing resolve — the plain deferred output.
        None,
        /// @brief FXAA: one post-tonemap luma-directed edge blur (spatial, no history).
        FXAA,
        /// @brief TAA: an HDR-space temporal resolve over a jittered, reprojected history.
        TAA,
        /// @brief CMAA2: post-tonemap conservative morphological AA (a compute battery).
        CMAA2,
        /// @brief TAAU: the temporal resolve as an upscaler — render below native, reconstruct native.
        TAAU,
    };

    /// @brief Display names for the AntiAliasingMode arms, indexed by enum value.
    ///
    /// One source of truth for an AA-mode selector in a debug panel or editor viewport, the
    /// DebugViewNames precedent: entry N names AntiAliasingMode N, so a combo's selected index
    /// casts straight to the enum.
    inline constexpr std::array<string_view, 5> AntiAliasingModeNames{"None", "FXAA", "TAA",
                                                                      "CMAA2", "TAAU"};
    static_assert(
        AntiAliasingModeNames.size() == static_cast<usize>(AntiAliasingMode::TAAU) + 1,
        "AntiAliasingModeNames must list every AntiAliasingMode arm in declaration order.");

    /// @brief Selects the renderer's top-level render path — the topology axis above DebugView.
    ///
    /// A second axis of variation orthogonal to DebugView Mode: Shaded is the full deferred pipeline
    /// (g-buffer → lighting → translucent → tonemap, DebugView and all), the default so every
    /// existing consumer is unchanged. GeometryDepthNormal is the shading-free lean path — it
    /// rasterizes opaque geometry writing only depth and a world normal, allocating and running none
    /// of the colour g-buffer / lighting / translucent / tonemap stages, and exposes depth and normal
    /// as sampleable bindless handles (GetDepthHandle / GetNormalHandle). It is for a consumer that
    /// wants a mesh's shape rather than its shading — an object thumbnail, a minimap silhouette, an
    /// editor selection outline, a custom occlusion input.
    ///
    /// When GeometryDepthNormal is selected, ResolveFrameTopology takes the lean topology and ignores
    /// DebugView Mode entirely — the two axes are never combined. This is a topology change (a
    /// Configure → recompile), and the target set is allocated for the path at Create.
    enum class RenderPath : u8
    {
        /// @brief The full deferred pipeline (g-buffer, lighting, translucent, tonemap, DebugView).
        Shaded,
        /// @brief The shading-free lean path: opaque geometry to depth + world normal only.
        GeometryDepthNormal,
    };

    /// @brief Topology and sizing knobs for SceneRenderer.
    ///
    /// A change to any field here is a Configure → recompile. Knobs that turn a pass
    /// on/off or re-wire the pass set live here; per-frame values belong on SceneView.
    struct SceneRendererSettings
    {
        /// @brief Selects whether culling and draw submission run CPU-side or GPU-driven.
        ///
        /// CPU is the BVH frustum descent plus direct per-submesh DrawIndexed calls — the
        /// default and the fallback where multiDrawIndirect / drawIndirectFirstInstance is
        /// unavailable. GPU keeps the same BVH frustum descent (the upload source) but runs
        /// the hi-Z occlusion test in a compute pass that writes each indirect command's
        /// instanceCount, then issues the survivors through vkCmdDrawIndexedIndirect. Both
        /// modes drive the same buffer-indexed surface shader — they differ only in
        /// submission and in who writes instanceCount. Nested to avoid the name collision
        /// with Renderer::CullMode (the rasterizer face-cull mode).
        enum class CullMode : u8
        {
            /// @brief BVH frustum descent + direct per-submesh DrawIndexed.
            CPU,
            /// @brief BVH frustum descent + GPU hi-Z occlusion + vkCmdDrawIndexedIndirect.
            GPU,
        };

        /// @brief Selects the top-level render path; GeometryDepthNormal ignores DebugView Mode.
        ///
        /// Default Shaded, so an existing consumer is unchanged. Selected at Create — the renderer
        /// allocates the target set for its path at construction. A change re-wires the pass set on
        /// the Configure recompile.
        RenderPath Path = RenderPath::Shaded;

        /// @brief Selects which result the renderer produces; re-wires the pass set on change.
        ///
        /// Consulted only on the Shaded path; the GeometryDepthNormal path ignores it entirely.
        DebugView Mode = DebugView::Final;

        /// @brief Whether every opaque g-buffer draw shades through one trivial fragment stage.
        ///
        /// A diagnostic: each draw keeps its own vertex stage, pipeline layout and face culling, its
        /// sets, its submission and its draw count, and its material's fragment stage is replaced by
        /// one writing a constant albedo, the interpolated vertex normal and a motion vector, with
        /// no material or texture read. The geometry, raster and draw cost of the pass are therefore
        /// those of the normal frame, so the change in the "Scene GBuffer" pass's GPU time between
        /// the two settings is what the materials' shading costs, and the remainder is geometry.
        /// The lighting and every later pass run as usual over the untextured surfaces. A fragment a
        /// material would have discarded or given a written depth is shaded at its rasterized
        /// depth instead (no opaque material alpha-tests, so coverage moves only for a custom
        /// fragment that discards). Off by default; a toggle is a Configure like any other field.
        bool GBufferShadingOverride = false;

        /// @brief Whether the compute mip-pyramid bloom runs ahead of tonemap.
        ///
        /// A topology change: it inserts/removes the bloom down/up/composite compute sweep.
        /// BloomThreshold, BloomFireflyCeiling, BloomIntensity, and BloomRadius are per-frame
        /// values on SceneView and do not trigger a recompile.
        bool Bloom = true;

        /// @brief Selects the bloom pyramid's down/up filter kernel.
        ///
        /// A topology change: it selects the compiled down/up compute pipeline. The Cod
        /// default is the golden's kernel; Kawase is the bandwidth-optimized alternative.
        BloomKernel Kernel = BloomKernel::Cod;

        /// @brief Which full-screen anti-aliasing resolve the frame wires (default None).
        ///
        /// A topology change through Configure. FXAA and CMAA2 insert a post-tonemap resolve that
        /// reads the tonemapped LDR and writes the output; TAA and TAAU jitter the projection, insert
        /// the HDR-space resolve + history-copy passes between lighting and the tonemap tail, and
        /// route lighting into a separate target the resolve reads (TAAU reconstructs the native image
        /// from a sub-native render — see the enum). The modes are mutually exclusive. Motion vectors
        /// are per-object, read from the g-buffer velocity channel (G3) the surface pass writes every
        /// frame (camera and object motion combined), so the temporal resolve needs no velocity
        /// prepass and dynamic objects reproject correctly. Supersampling composes on top through
        /// the viewport's MaxAllocationScale — it is not one of these modes.
        AntiAliasingMode AntiAliasing = AntiAliasingMode::None;

        /// @brief Whether the directional light casts a shadow.
        ///
        /// A topology change: it inserts/removes the depth-only ShadowScenePass and the
        /// lighting pass's shadow sample. When off, the lighting pass reads full
        /// visibility for the directional term.
        bool Shadows = true;

        /// @brief Whether the bounded set of point/spot lights cast shadows.
        ///
        /// A topology change: it inserts/removes the depth-only PunctualShadowScenePass
        /// and the lighting pass's per-light sample. When off, the per-light selection
        /// writes slot -1 to every light and the lighting pass reads full visibility for
        /// every punctual term. MaxShadowedPunctual caps the number of shadowed lights
        /// when enabled.
        bool PunctualShadows = true;

        /// @brief Per-cascade shadow tile edge length in texels.
        ///
        /// Changing this recreates the shadow atlas and recompiles. A default 4-cascade
        /// atlas at 1024 is 2048² — the same footprint as a single 2048 map. Values
        /// above GetMaxShadowResolution() are clamped before any atlas is sized.
        u32 ShadowResolution = 1024;

        /// @brief Per-tile edge length in texels of the punctual shadow atlas.
        ///
        /// A spot uses one tile; a point uses six cube-face tiles. Changing this recreates
        /// the punctual atlas and recompiles. The atlas is MaxShadowedPunctual·CubeFaceCount
        /// tiles of this resolution. Values above GetMaxPunctualShadowResolution() are
        /// clamped.
        u32 PunctualShadowResolution = 1024;

        /// @brief Number of shadow cascades the directional light's frustum is split into.
        ///
        /// Clamped to [1, MaxCascades]. Sizing: it sizes the atlas tile grid
        /// (min(Count,2)×ceil(Count/2)), so it recreates the atlas and recompiles.
        u32 CascadeCount = MaxCascades;

        /// @brief PSSM split blend factor (0 = uniform splits, 1 = logarithmic).
        ///
        /// Recompile-safe: it changes the cascade fit, not the atlas size or topology.
        f32 CascadeSplitLambda = 0.85f;

        /// @brief View-space cap on the directional-shadow range, in world units; 0 = uncapped.
        ///
        /// The cascade far split is fitted to the visible scene and then clamped to this
        /// distance, so a large scene (or a distant camera far plane) cannot spread the
        /// cascades thin — texel density near the camera is preserved and shadows fade out
        /// approaching the cap rather than ending at a hard edge. Recompile-safe: it changes
        /// the cascade fit, not the atlas size or topology.
        f32 MaxShadowDistance = 100.0f;

        /// @brief View-space floor on the directional-shadow near, in world units; 0 = the
        ///        camera near.
        ///
        /// The near-side mirror of MaxShadowDistance: the cascade fit runs over
        /// [near, far], and a camera rendering a huge depth range with a very small near
        /// (a reverse-Z scene whose micron near keeps far-field depth precision) otherwise
        /// packs every near cascade into a sub-pixel shell at the eye and reconstructs
        /// degenerate slices, collapsing the shadow. Flooring the shadow near here decouples
        /// the cascade fit from the render near. Recompile-safe: it changes the cascade fit,
        /// not the atlas size or topology.
        f32 MinShadowDistance = 0.0f;

        /// @brief Projected size in texels below which a caster is left out of a cascade.
        ///
        /// A caster whose world bound spans fewer texels than this in a cascade's tile cannot
        /// move that cascade's depth meaningfully, so the cascade skips it. The far cascades,
        /// whose texels are largest, are where it trips. 0 draws every caster. Recompile-safe: it
        /// changes which casters a cascade draws, not the atlas or the topology.
        f32 ShadowCasterMinTexels = 1.0f;

        /// @brief Whether screen-space reflections run.
        ///
        /// A topology change: it allocates the reflection chain's targets, lists and counters and
        /// inserts its passes after the opaque lighting. SsrSamplesPerQuad, SsrRoughnessThreshold,
        /// SsrVarianceThreshold and SsrVarianceGuidedTracing are per-frame values on SceneView and
        /// do not recompile. Off by default, so nothing is allocated or declared.
        bool SSR = false;

        /// @brief Whether the depth-of-field battery runs.
        ///
        /// A topology change: it inserts the circle-of-confusion prefilter, tile reduction,
        /// per-layer ring gather and fill compute stages plus a fullscreen composite between the
        /// lit HDR and the bloom sweep, and routes the bloom/tonemap tail through the composite's
        /// HDR intermediate. DofFocusDistance / DofAperture / DofCocScale / DofMaxCoc /
        /// DofRingCount are per-frame values on SceneView and do not recompile. Off by default, so
        /// the shipping deferred path is byte-identical.
        bool DepthOfField = false;

        /// @brief Whether translucent materials can sample the scene color behind them.
        ///
        /// A topology change: it allocates a full-resolution scene-color intermediate and inserts
        /// a copy of the lit scene color (lighting + sky + emissive) ahead of the translucent
        /// pass, exposing it to translucent fragments through the view block's SceneColor handles
        /// (see Veng/translucent.slang) — the grab pass a refractive or distorting material
        /// (glass, water, heat haze, a lens) samples the scene behind itself through. The copy
        /// predates the translucent pass, so one translucent surface never refracts another. Off
        /// by default, since it holds a full-resolution target; the copy records only on a frame a
        /// drawn material declares "readsSceneColor". The opaque-depth copy beside it needs no
        /// setting: a material declaring "readsSceneDepth" reads it with this off.
        bool Refraction = false;

        /// @brief Whether the scene-color grab carries a mip chain a material can read blurred.
        ///
        /// Requires Refraction, and is a topology change of its own: the grab is allocated with a
        /// mip chain down to roughly an 8-pixel level and a fullscreen halving pass is inserted per
        /// level, so a translucent material can sample the scene behind it at a chosen blur through
        /// SampleSceneColorBlurred (see Veng/translucent.slang) — frosted glass, ground glass, a
        /// backdrop blur behind an interface.
        ///
        /// **A mip chain rather than a wide kernel, because of what is usually behind glass.** A
        /// handful of taps blurs a smooth image acceptably and turns a field of small high-contrast
        /// points into visible duplicates of each one; a halving chain averages every texel exactly
        /// once per level, so a point spreads instead of repeating. It is also far cheaper at a wide
        /// radius — the whole chain costs about a third of one full-resolution pass, and the radius
        /// is then free.
        ///
        /// Off by default and separate from Refraction, because a material that only distorts its
        /// samples needs none of it and the levels are generated whether or not anything reads them.
        bool RefractionBlur = false;

        /// @brief Whether the screen-space ambient occlusion pass runs.
        ///
        /// A topology change: it inserts/removes the fullscreen SsaoScenePass and
        /// selects the lighting pipeline variant that folds the AO target into the
        /// ambient term. SSAO modulates the ambient/indirect term only; kernel constants
        /// (radius/intensity/bias) are fixed in the SSAO shader.
        bool AO = true;

        /// @brief Whether automatic exposure metering drives the tonemap exposure.
        ///
        /// A topology change: it inserts a compute pass that builds a log-luminance histogram of the
        /// lit HDR target each frame, which the renderer averages a frame later (excluding the black
        /// bin) and eases an internal adapted exposure toward (temporal eye adaptation). Metering on
        /// the histogram's lit bins keeps a predominantly-black scene from collapsing exposure to the
        /// floor. With it on, the tonemap
        /// exposure is the adapted value scaled by SceneView::Exposure (a manual bias over the
        /// automatic result); with it off, SceneView::Exposure is used directly. Off by default,
        /// so the shipping path is unchanged. AutoExposureKey / AutoExposureMinLuminance /
        /// AutoExposureMaxLuminance / AutoExposureSpeed are per-frame SceneView values that tune it
        /// without a recompile.
        bool AutoExposure = false;

        /// @brief Whether scene passes cull by frustum.
        ///
        /// The g-buffer pass tests each mesh's world bound against the camera frustum;
        /// the shadow pass tests it against each cascade's light frustum. When off both
        /// record every resident mesh. A toggle drives a recompile (the same passes
        /// record fewer draws), so it still invalidates GetOutput() like any Configure.
        bool FrustumCull = true;

        /// @brief Whether the lighting pass shades each pixel with only the lights that can reach
        ///        its screen tile.
        ///
        /// A topology change: it inserts a compute pass ahead of lighting that cuts the screen into
        /// LightTileSize-pixel tiles, bounds each tile's visible geometry by its depth range, and
        /// writes a mask of the packed lights whose range reaches it; the lighting pass then visits
        /// only the tile's lights instead of every packed light. The bound is conservative, so the
        /// image is the same either way and only the lighting cost moves. Off loops every packed
        /// light at every pixel — kept as a switch so the two costs can be compared.
        bool LightTileCulling = true;

        /// @brief Selects CPU direct draws or the GPU-driven occlusion-cull → indirect-draw path.
        ///
        /// GPU is honored only where Context::IsGpuDrivenCullingSupported() is true; otherwise
        /// the renderer falls back to the CPU path. A change recompiles (the GPU path is a
        /// different pass topology).
        CullMode Cull = CullMode::CPU;

        /// @brief Levels of bias on level-of-detail selection, in every view.
        ///
        /// Each +1 halves every mesh's projected screen size before it is compared against the
        /// mesh's thresholds, so a mesh switches to each coarser level at twice the distance; a
        /// negative bias holds finer levels longer. Not a topology change: a Configure that changes
        /// only the level-of-detail fields rebuilds nothing.
        f32 LodBias = 0.0f;

        /// @brief Half-width of the hysteresis band around each level's threshold, as a fraction of
        ///        the threshold, in [0, 0.5].
        ///
        /// The camera view keeps the level a mesh drew last frame while its screen size stays within
        /// the band, so a mesh resting near a threshold does not alternate between levels. Shadow
        /// views select without it (see the renderer's level-of-detail notes).
        f32 LodHysteresis = 0.1f;

        /// @brief The level every mesh draws at in every view, or AutomaticLod to select by size.
        ///
        /// Clamps to each mesh's coarsest, so CoarsestLod forces every mesh's lowest detail. A
        /// per-entity LodOverride and SceneView::ForcedLod both win over it.
        i32 ForcedLod = AutomaticLod;

        /// @brief Whether the GPU path runs the hi-Z occlusion test (GPU mode only).
        ///
        /// When off, the GPU path issues every camera-frustum survivor (frustum-only). When on,
        /// the cull compute pass drops the provably-occluded against the previous-frame pyramid.
        /// Ignored under CullMode::CPU. A history-invalid frame is frustum-only regardless.
        bool Occlusion = true;

        /// @brief Whether the immediate-mode debug-draw pass runs (off by default).
        ///
        /// A topology change: it inserts the DebugDrawScenePass after the terminal tonemap (Final
        /// mode only), flushing the renderer's DebugDraw accumulator (GetDebugDraw()) into the LDR
        /// scene color. The pass samples the g-buffer depth for a depth-aware occluded fade rather
        /// than hardware depth-testing. Off by default, so the default render is unchanged.
        bool DebugDraw = false;

        /// @brief Whether the entity-id picking pass runs (off by default).
        ///
        /// A topology change: it allocates an R32Uint EntityId target plus a dedicated depth
        /// buffer and inserts a depth-tested geometry pass writing each drawn entity's pick id
        /// (packed slot index + 1) into the id target. Allocated only while set, so the shipping
        /// deferred path is byte-identical and smoke_golden never moves. An authoring concern (the
        /// editor enables it for a viewport's lifetime), never a runtime one. The pass early-outs
        /// on a frame with no pending pick request, so its amortized cost is near zero.
        bool Picking = false;

        /// @brief Whether the renderer gathers and runs the scene's PostProcessEffect components (on by default).
        ///
        /// When set, each Execute resolves the scene's enabled PostProcessEffect components with a
        /// loaded material and runs them fullscreen in the HDR tail before bloom (content-driven, the
        /// lights model — no pass exists while no effect is authored). Cleared to drop them entirely:
        /// a reflection-probe capture renders the same world with a lean battery and wants no screen
        /// effect baked into it, and clearing this also removes any question of the probe's extra
        /// Executes sharing a per-viewport MaterialInstance with the main view.
        bool PostProcessEffects = true;

        /// @brief Whether the frame runs the temporal (jittered-history) AA resolve.
        ///
        /// The temporal path is the one several batteries gate on (the jitter, the history-reset,
        /// and which resolve occupies the anchor — the temporal one, or the spatial promotion that
        /// stands in for it), so it earns a named predicate rather than a bare enum compare at each
        /// site. It does **not** gate the post-resolve chain's resolution: that chain runs at the
        /// full allocation in every mode. Both TAA and TAAU run it; they share the whole temporal
        /// machinery.
        /// @return True for AntiAliasingMode::TAA and AntiAliasingMode::TAAU.
        [[nodiscard]] bool UsesTaa() const
        {
            return AntiAliasing == AntiAliasingMode::TAA || AntiAliasing == AntiAliasingMode::TAAU;
        }

        /// @brief Whether the temporal resolve is acting as an upscaler (render below native).
        ///
        /// TAAU-only: the viewport reads this to pin the allocation to native and route the render
        /// scale into the rendered sub-rect, so the resolve reconstructs the native image. TAA leaves
        /// the render scale an allocation scale, so it returns false there.
        /// @return True only for AntiAliasingMode::TAAU.
        [[nodiscard]] bool UsesTaaUpscaling() const
        {
            return AntiAliasing == AntiAliasingMode::TAAU;
        }

        /// @brief Compares two settings field-for-field for topology equivalence.
        ///
        /// Every field here is a topology/sizing knob whose change forces a Viewport::Configure
        /// recompile, so this is the dirty-compare a re-apply path uses to skip a needless recompile
        /// when only per-frame (SceneView) knobs moved. Defaulted, so a field added to this struct is
        /// covered automatically.
        [[nodiscard]] bool operator==(const SceneRendererSettings&) const = default;
    };
}

VE_ENUM(::Veng::Renderer::DebugView, 0xC23242A904ADC170ULL)
VE_ENUMERATOR(Final)
VE_ENUMERATOR(Albedo)
VE_ENUMERATOR(Normal)
VE_ENUMERATOR(Depth)
VE_ENUMERATOR(Roughness)
VE_ENUMERATOR(Metallic)
VE_ENUMERATOR(Occlusion)
VE_ENUMERATOR(AO)
VE_ENUMERATOR(Shadows)
VE_ENUMERATOR(Cascades)
VE_ENUMERATOR(PunctualShadows)
VE_ENUMERATOR(Bloom)
VE_ENUMERATOR(MotionVectors)
VE_ENUMERATOR(Emissive)
VE_ENUMERATOR(CoC)
VE_ENUMERATOR(EnvironmentIrradiance)
VE_ENUMERATOR(EnvironmentSource)
VE_ENUMERATOR(IblContribution)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::BloomKernel, 0x62E9A50DEDB73EC0ULL)
VE_ENUMERATOR(Cod)
VE_ENUMERATOR(Kawase)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::AntiAliasingMode, 0xAA935BA2CB779756ULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(FXAA)
VE_ENUMERATOR(TAA)
VE_ENUMERATOR(CMAA2)
VE_ENUMERATOR(TAAU)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::RenderPath, 0xAA4082E0B2E08CFBULL)
VE_ENUMERATOR(Shaded)
VE_ENUMERATOR(GeometryDepthNormal)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::SceneRendererSettings::CullMode, 0xF097C435E1F04F2FULL)
VE_ENUMERATOR(CPU)
VE_ENUMERATOR(GPU)
VE_ENUM_END();

// Reflected so a field-masked override set (FieldOverrides) and a reflected-field settings surface
// can address every field without listing them. Category groups the fields into the sections a
// settings surface draws; the render path is chosen at Create, so it reads but does not edit.
VE_REFLECT(::Veng::Renderer::SceneRendererSettings, 0x086EC468B7B8D334ULL)
VE_FIELD(Path, .DisplayName = "Render path", .ReadOnly = true, .Category = "Pipeline")
VE_FIELD(Mode, .DisplayName = "Debug view", .Category = "Pipeline")
VE_FIELD(GBufferShadingOverride, .DisplayName = "G-buffer shading override",
         .Tooltip = "Shades every opaque draw with a constant albedo and its vertex normal, so "
                    "the g-buffer pass's time without material shading can be compared",
         .Category = "Pipeline")
VE_FIELD(Bloom, .DisplayName = "Bloom", .Category = "Bloom")
VE_FIELD(Kernel, .DisplayName = "Bloom kernel", .Category = "Bloom")
VE_FIELD(AntiAliasing, .DisplayName = "Anti-aliasing", .Category = "Anti-aliasing")
VE_FIELD(Shadows, .DisplayName = "Directional shadows", .Category = "Shadows")
VE_FIELD(PunctualShadows, .DisplayName = "Punctual shadows", .Category = "Shadows")
VE_FIELD(ShadowResolution, .DisplayName = "Shadow resolution",
         .Display = {.Min = 256.0, .Max = 8192.0, .Step = 16.0}, .Category = "Shadows")
VE_FIELD(PunctualShadowResolution, .DisplayName = "Punctual shadow resolution",
         .Display = {.Min = 256.0, .Max = 8192.0, .Step = 16.0}, .Category = "Shadows")
VE_FIELD(CascadeCount, .DisplayName = "Cascades",
         .Display = {.Min = 1.0, .Max = static_cast<f64>(::Veng::Renderer::MaxCascades)},
         .Category = "Shadows")
VE_FIELD(CascadeSplitLambda, .DisplayName = "Cascade split lambda",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 1.0},
         .Category = "Shadows")
VE_FIELD(MaxShadowDistance, .DisplayName = "Max shadow distance",
         .Display = {.Min = 0.0, .Max = 10000.0}, .Category = "Shadows")
VE_FIELD(MinShadowDistance, .DisplayName = "Min shadow distance", .Display = {.Min = 0.0},
         .Category = "Shadows")
VE_FIELD(ShadowCasterMinTexels, .DisplayName = "Shadow caster min texels", .Display = {.Min = 0.0},
         .Category = "Shadows")
VE_FIELD(SSR, .DisplayName = "Screen-space reflections", .Category = "Screen-space reflections")
VE_FIELD(DepthOfField, .DisplayName = "Depth of field", .Category = "Depth of field")
VE_FIELD(Refraction, .DisplayName = "Refraction (scene-color grab)", .Category = "Refraction")
VE_FIELD(RefractionBlur, .DisplayName = "Refraction blur (grab mip chain)",
         .Category = "Refraction")
VE_FIELD(AO, .DisplayName = "SSAO", .Category = "Lighting")
VE_FIELD(AutoExposure, .DisplayName = "Auto exposure", .Category = "Exposure")
VE_FIELD(FrustumCull, .DisplayName = "Frustum culling", .Category = "Culling")
VE_FIELD(LightTileCulling, .DisplayName = "Light tile culling", .Category = "Culling")
VE_FIELD(Cull, .DisplayName = "Cull mode", .Category = "Culling")
VE_FIELD(Occlusion, .DisplayName = "GPU occlusion", .Category = "Culling")
VE_FIELD(LodBias, .DisplayName = "LOD bias", .Display = {.Min = -4.0, .Max = 4.0},
         .Category = "Level of detail")
VE_FIELD(LodHysteresis, .DisplayName = "LOD hysteresis",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 0.5},
         .Category = "Level of detail")
VE_FIELD(ForcedLod, .DisplayName = "Forced LOD", .Tooltip = "-1 selects by screen size",
         .Display = {.Min = -1.0, .Max = 7.0}, .Category = "Level of detail")
VE_FIELD(DebugDraw, .DisplayName = "Debug draw", .Category = "Authoring")
VE_FIELD(Picking, .DisplayName = "Picking", .Category = "Authoring")
VE_FIELD(PostProcessEffects, .DisplayName = "Post-process effects", .Category = "Pipeline")
VE_REFLECT_END();
