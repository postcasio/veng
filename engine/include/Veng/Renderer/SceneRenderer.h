#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/Atmosphere.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Tonemapper.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/HiZHistory.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/VolumeMarch.h>
#include <Veng/Renderer/ShadowCascades.h>

#include <Veng/Math/SphericalHarmonics.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneBroadphase.h>
#include <Veng/Scene/Visibility.h>

#include <array>
#include <span>

/// @brief Long-lived, configurable deferred render pipeline.
///
/// Owns an offscreen target, renders a Scene from a Camera through an internal
/// compiled RenderGraph composed of reusable ScenePass units, and hands back a
/// sampleable result.
///
/// Surface lifetime split by how often each piece of state changes:
/// - Create: allocate persistent resources and compile the graph.
/// - Resize: recreate extent-sized resources and recompile.
/// - Configure: recreate affected resources and recompile topology.
/// - Execute: replay the graph per frame — no reallocation or recompile.
/// - GetOutput: return the owned sampleable result.
namespace Veng
{
    class Scene;
    class AssetManager;
    class Material;
    class MaterialInstance;
    class EnvironmentMap;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class ScenePass;
    class ShadowScenePass;
    class PunctualShadowScenePass;
    class ShadowSystem;
    class BloomPyramid;
    class AutoExposureMeter;
    class TaaResolve;
    class SsrChain;
    class RefractionGrab;
    class GpuCullSystem;
    class PickingSystem;
    class Image;
    class Sampler;
    class Buffer;
    class DescriptorSet;
    class DescriptorSetLayout;
    class SkyResolver;
    class PointField;

    /// @brief Maximum number of simultaneously shadowed point/spot lights.
    ///
    /// The first N shadow-casting punctual lights (by per-frame selection) receive a
    /// shadow map; the rest are lit without shadows. A point light costs six cube-face
    /// redraws of its caster set, so N bounds the punctual shadow atlas and the lighting
    /// loop's sample set at 6N depth tiles.
    inline constexpr u32 MaxShadowedPunctual = 4;

    /// @brief Per-shadowed-light GPU record uploaded to set 1 binding 3.
    ///
    /// glm-only — no backend types — so it lives in a public header. Its layout is
    /// std140/std430-identical to the shader's PunctualShadowRecord, so the same struct
    /// serves a uniform or SSBO binding.
    struct PunctualShadowRecord
    {
        /// @brief World → light-clip transforms with atlas tile-remap baked in (384 bytes).
        ///
        /// [0] for a spot's single perspective view; [0..5] for a point's six cube faces
        /// in CubeFace order. A lit fragment projected by ViewProj[f] lands in this
        /// light's atlas tile f, so the lighting pass samples the correct tile.
        mat4 ViewProj[CubeFaceCount];

        /// @brief World position (xyz) and falloff range (w) (16 bytes).
        ///
        /// xyz is the light's world position for cube-face selection and depth
        /// linearization; w is the range the depth pass projects to.
        vec4 PositionRange;

        /// @brief Type (x), near (y), far (z), depth bias (w) (16 bytes).
        ///
        /// x encodes the light type: 1 = point, 2 = spot, 0 = unused/zeroed slot.
        vec4 Params;
    };

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
    /// independent of lighting.
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
        /// @brief Raw SSR reflection target (rgb radiance, force-wires the SSR pass).
        Reflections,
        /// @brief The authored emissive g-buffer channel (G4) alone.
        Emissive,
    };

    /// @brief Display names for the DebugView arms, indexed by enum value.
    ///
    /// The single source of truth for the "View" combo in both the engine debug panel and the
    /// editor viewport: entry N is the name of DebugView N, so a combo's selected index casts
    /// straight to the enum. The static_assert below keeps it in lockstep with the enum.
    inline constexpr std::array<string_view, 15> DebugViewNames{
        "Final",          "Albedo",      "Normal",  "Depth",    "Roughness",        "Metallic",
        "Occlusion",      "AO",          "Shadows", "Cascades", "Punctual shadows", "Bloom",
        "Motion vectors", "Reflections", "Emissive"};
    static_assert(DebugViewNames.size() == static_cast<usize>(DebugView::Emissive) + 1,
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

        /// @brief Resolution the SSR trace, min-Z pyramid, and blur chain run at.
        ///
        /// The trace is SSR's dominant cost; running it at a fraction of the render
        /// resolution cuts that roughly quadratically. The g-buffer it reads stays
        /// full-resolution (sampled by normalized UV) and the composite upsamples the
        /// reflection back to full resolution, so only the reflection working set shrinks.
        enum class SsrResolution : u8
        {
            /// @brief Trace at the full render resolution.
            Full,
            /// @brief Trace at half the render resolution per axis (quarter the pixels).
            Half,
            /// @brief Trace at a quarter of the render resolution per axis (a sixteenth the pixels).
            Quarter,
        };

        /// @brief Selects which result the renderer produces; re-wires the pass set on change.
        DebugView Mode = DebugView::Final;

        /// @brief Whether the compute mip-pyramid bloom runs ahead of tonemap.
        ///
        /// A topology change: it inserts/removes the bloom down/up/composite compute sweep.
        /// BloomThreshold, BloomIntensity, and BloomRadius are per-frame values on SceneView
        /// and do not trigger a recompile.
        bool Bloom = true;

        /// @brief Selects the bloom pyramid's down/up filter kernel.
        ///
        /// A topology change: it selects the compiled down/up compute pipeline. The Cod
        /// default is the golden's kernel; Kawase is the bandwidth-optimized alternative.
        BloomKernel Kernel = BloomKernel::Cod;

        /// @brief Whether temporal anti-aliasing resolves the lit image.
        ///
        /// A topology change: it jitters the projection, inserts the TAA resolve and
        /// history-copy passes between lighting and tonemap, and routes lighting into a
        /// separate target the resolve reads. Off by default. Motion vectors are per-object,
        /// read from the g-buffer velocity channel (G3) the surface pass writes every frame
        /// (camera and object motion combined), so dynamic objects reproject correctly too.
        bool TAA = false;

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

        /// @brief Whether screen-space reflections run.
        ///
        /// A topology change: it inserts the SSR min-Z reduction, trace, blur, and composite
        /// passes between lighting and the bloom/tonemap tail and routes the lit scene color
        /// through an intermediate the SSR composite reflects into. SsrIntensity / SsrMaxDistance
        /// / SsrThickness / SsrMaxRoughness are per-frame values on SceneView and do not recompile.
        /// Off by default (like TAA). SSR disables the dynamic-resolution sub-rect path while
        /// active (the g-buffer renders at full resolution); SsrResolutionScale sizes the SSR
        /// trace itself.
        bool SSR = false;

        /// @brief Resolution the SSR trace/min-Z/blur chain runs at relative to the render target.
        ///
        /// A topology change: it resizes the SSR reflection chain and min-Z pyramid, so a change
        /// recompiles. Defaults to Half — the trace cost falls ~4x and the composite upsamples the
        /// reflection, with little visible loss on the rough/glossy surfaces SSR targets. Ignored
        /// when SSR is inactive.
        SsrResolution SsrResolutionScale = SsrResolution::Half;

        /// @brief Whether translucent materials can sample the scene color behind them.
        ///
        /// A topology change: it allocates a full-resolution scene-color intermediate and inserts
        /// a copy of the lit scene color (lighting + sky + emissive) ahead of the translucent
        /// pass, exposing it to translucent fragments through the view block's SceneColor handles
        /// (see Veng/translucent.slang) — the grab pass a refractive or distorting material
        /// (glass, water, heat haze, a lens) samples the scene behind itself through. The copy
        /// predates the translucent pass, so one translucent surface never refracts another. Off
        /// by default: the copy costs a full-resolution pass per frame whether or not any material
        /// samples it.
        bool Refraction = false;

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

        /// @brief Selects CPU direct draws or the GPU-driven occlusion-cull → indirect-draw path.
        ///
        /// GPU is honored only where Context::IsGpuDrivenCullingSupported() is true; otherwise
        /// the renderer falls back to the CPU path. A change recompiles (the GPU path is a
        /// different pass topology).
        CullMode Cull = CullMode::CPU;

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
    };

    /// @brief Construction parameters for SceneRenderer.
    struct SceneRendererInfo
    {
        /// @brief The Vulkan context for resource creation.
        Context& Context;
        /// @brief Asset manager used to load engine shaders (lighting pass, fullscreen blit).
        ///
        /// Must outlive the renderer.
        AssetManager& Assets;
        /// @brief Pixel format of the owned output target.
        Format OutputFormat = Format::Undefined;
        /// @brief Initial render extent.
        uvec2 Extent = {};
        /// @brief Initial topology and sizing knobs.
        SceneRendererSettings Settings;
    };

    /// @brief Per-frame input for SceneRenderer::Execute.
    ///
    /// Not owned by the renderer; World and Camera are borrowed references. The renderer
    /// overwrites the output fields (LightCount, CascadeViewProj, etc.) on every Execute
    /// — a caller's values in those fields are ignored.
    ///
    /// The renderer reads the scene's lights itself: on every Execute it walks
    /// View<Transform, Light> up to MaxLights, packs each into the ring-buffered light
    /// buffer, and the lighting pass loops over the live count. A scene with no Light
    /// renders flat-ambient.
    struct SceneView
    {
        /// @brief The scene to render.
        const Scene& World;
        /// @brief The viewpoint to render from.
        const CameraView& Camera;
        /// @brief Frame delta time in seconds.
        f32 Delta = 0.0f;

        /// @brief Interpolation fraction into the next Sim tick, in [0, 1).
        ///
        /// The gather blends each candidate's world transform between the scene's last two Sim-tick
        /// snapshots by this (Scene::GetInterpolatedWorldTransform) before the passes consume it, so a
        /// fixed-rate simulation renders smoothly above its tick rate. Zero (or a scene with no motion
        /// history) renders the current pose, byte-identical to the un-interpolated path.
        f32 Alpha = 0.0f;

        /// @brief Dynamic-resolution multiplier on the allocated extent for this frame.
        ///
        /// The renderer's targets are allocated at a high-water-mark extent; each Execute renders
        /// into the top-left round(allocExtent * RenderScale) sub-rect of them and the result is
        /// upscaled by the consumer. (0,1] renders below the allocation (dynamic resolution
        /// scaling); a value that would exceed the current allocation grows it (a one-time
        /// resize). 1.0 renders at full allocation. A debug view (Mode != Final) forces 1.0.
        /// Clamped to a valid range by the renderer; the realized sub-rect is GetValidExtent().
        f32 RenderScale = 1.0f;

        /// @brief This frame's render-target sub-rect extent; set by the renderer each Execute.
        ///
        /// round(allocExtent * RenderScale), clamped to [1, allocExtent]. Every pass sizes its
        /// viewport/scissor and compute dispatch to it; a caller's value is overwritten.
        uvec2 RenderExtent = {};

        /// @brief Live light count this frame; set by the renderer on every Execute.
        ///
        /// The number of (Transform, Light) entities packed, capped at MaxLights. The
        /// lighting pass loops [0, LightCount). A caller's value is overwritten.
        u32 LightCount = 0;

        /// @brief Maximum number of lights the renderer packs per frame.
        static constexpr u32 MaxLights = BindlessRegistry::MaxLights;

        /// @brief Exposure scale applied before the tone curve; written into the tonemap material's param block each Execute.
        ///
        /// Read fresh every Execute, so tuning it never triggers a recompile. With
        /// SceneRendererSettings::AutoExposure on this is a manual bias multiplied onto the
        /// metered/adapted exposure rather than the exposure itself.
        f32 Exposure = 1.0f;

        /// @brief The tone curve the terminal tonemap pass maps the exposed HDR through.
        ///
        /// Written into the tonemap material's param block each Execute (as a float), so switching
        /// it never triggers a recompile.
        Tonemapper Tonemapper = ::Veng::Renderer::Tonemapper::ACES;

        /// @brief Auto-exposure target key: the mid-grey the adapted average luminance maps to.
        ///
        /// The exposure the metering resolves to is Key / adaptedLuminance, so a larger key
        /// brightens the auto-exposed image. Ignored when SceneRendererSettings::AutoExposure is
        /// off.
        f32 AutoExposureKey = 0.18f;

        /// @brief Auto-exposure lower clamp on the adapted average luminance, in cd-equivalent HDR units.
        ///
        /// Bounds how bright the metering can drive a very dark scene (a small value = a brighter
        /// cap in the dark). Ignored when auto-exposure is off.
        f32 AutoExposureMinLuminance = 0.002f;

        /// @brief Auto-exposure upper clamp on the adapted average luminance.
        ///
        /// Bounds how dark the metering can drive a very bright scene. Ignored when auto-exposure
        /// is off.
        f32 AutoExposureMaxLuminance = 8.0f;

        /// @brief Auto-exposure adaptation rate per second (the eye-adaptation speed).
        ///
        /// The internal adapted luminance eases toward each frame's metered luminance at this
        /// exponential rate, so a larger value adapts faster (0 freezes the current adaptation).
        /// Ignored when auto-exposure is off.
        f32 AutoExposureSpeed = 2.5f;

        /// @brief Lower percentile of the lit-pixel histogram the metering averages from, in [0, 1].
        ///
        /// The metering averages log-luminance over the histogram slice between the low and high
        /// percentiles of lit pixels, discarding the tails outside them. The default 0..1 band
        /// meters every lit pixel; a raised low percentile makes a bimodal scene (a sun-lit
        /// surface against a near-black sky) meter on its bright content instead of the mean of
        /// both. Ignored when auto-exposure is off.
        f32 AutoExposureLowPercentile = 0.0f;

        /// @brief Upper percentile of the lit-pixel histogram the metering averages to, in [0, 1].
        ///
        /// See AutoExposureLowPercentile; lowering it excludes extreme highlights (a star disc)
        /// from the meter. Ignored when auto-exposure is off.
        f32 AutoExposureHighPercentile = 1.0f;

        /// @brief Environment map for the skybox and image-based lighting; empty for none.
        ///
        /// The renderer fills this from the resolved Sky component each Execute (its source is an
        /// EnvironmentSky) — never pushed by a consumer. When resident, the renderer (re)generates
        /// its IBL maps on change and the lighting pass replaces the flat ambient term with
        /// split-sum IBL; empty falls back to the flat ambient.
        AssetHandle<EnvironmentMap> Environment;

        /// @brief Scales the IBL ambient + skybox radiance; read by the lighting + skybox passes each Execute.
        ///
        /// Filled from the resolved Sky component's Intensity. Ignored when no environment is bound.
        f32 EnvironmentIntensity = 1.0f;

        /// @brief Scales the procedural atmosphere sky + sun disk; read by the SkyScenePass each Execute.
        ///
        /// Filled from the resolved Sky component's Intensity. Ignored when the atmosphere sky is off.
        f32 AtmosphereIntensity = 1.0f;

        /// @brief Scales the dynamic SH skylight ambient; read by the lighting pass each Execute.
        ///
        /// Filled from the resolved Sky component's Intensity. Effective only when the resolved sky
        /// lights the scene via SH and no environment is bound (the second ambient arm, below IBL).
        f32 SkylightIntensity = 1.0f;

        /// @brief Whether the procedural atmosphere sky renders this frame.
        ///
        /// Set by the renderer when the resolved Sky component's source is an AtmosphereSky, so the
        /// SkyScenePass (present in that topology) fills the background; else false. The sky pass
        /// discards every pixel when this is false.
        bool AtmosphereEnabled = false;

        /// @brief Normalized direction toward the sun for the procedural atmosphere (world up +Y).
        ///
        /// Derived by the renderer from the scene's first directional light. Drives the sky color
        /// and the sun-disk placement; a day/night cycle animates it with no precompute (a runtime
        /// LUT-sample parameter, never a precompute input). Ignored when the atmosphere sky is off.
        vec3 SunDirection{0.0f, 1.0f, 0.0f};

        /// @brief Procedural-atmosphere parameters; the LUTs regenerate when these change.
        ///
        /// Filled from the resolved Sky component's AtmosphereSky source. Compared field-for-field
        /// against the last-generated set each Execute; a change records the (one-time) LUT
        /// regeneration before the graph. Ignored when the atmosphere sky is off.
        Atmosphere Atmosphere;

        /// @brief Authored Sky-domain material rendered as the background sky; empty for none.
        ///
        /// Filled from the resolved Sky component's MaterialSky source. The SkyMaterialScenePass
        /// (present in that topology) runs it fullscreen in the sky slot, compositing its radiance
        /// over the lit scene color; empty leaves the sky slot empty (the lit color shows through).
        /// The material owns its own params and any buffers/textures it reads (a game binds a storage
        /// buffer via MaterialInstance::SetStorageBufferHandle); the engine supplies only the view
        /// ray and the g-buffer depth mask. It fills the background only — it feeds no lighting.
        AssetHandle<MaterialInstance> SkyMaterial;

        /// @brief Bloom bright-pass luminance knee; pushed to the downsample compute each Execute.
        ///
        /// The soft-knee threshold the HDR → mip 0 downsample applies, display-referred: it is
        /// divided by the frame's resolved exposure (manual or metered), so 1.0 sits at the
        /// post-exposure white point and blooms what the tone curve cannot show regardless of the
        /// lighting regime. Tuning this rides the compute push, so it does not trigger a
        /// recompile. Ignored when bloom is inactive.
        f32 BloomThreshold = 1.0f;
        /// @brief Bloom composite mix intensity; pushed to the composite compute each Execute.
        ///
        /// Scales the accumulated bloom added back into the HDR. Tuning this rides the compute
        /// push, so it does not trigger a recompile. Ignored when bloom is inactive.
        f32 BloomIntensity = 1.0f;
        /// @brief Bloom upsample spread; pushed to the upsample compute each Execute.
        ///
        /// Scales each tent up-step's contribution as it accumulates back up the pyramid, so a
        /// larger value spreads the glow wider. Rides the compute push (no recompile). Ignored
        /// when bloom is inactive.
        f32 BloomRadius = 1.0f;

        /// @brief SSR reflection mix scale; pushed to the SSR composite each Execute.
        ///
        /// Scales the Fresnel-weighted reflection added back into the scene color. Rides the push
        /// (no recompile). Ignored when SSR is inactive.
        f32 SsrIntensity = 1.0f;
        /// @brief SSR maximum ray length in view-space units; pushed to the SSR trace each Execute.
        f32 SsrMaxDistance = 12.0f;
        /// @brief SSR view-space depth thickness accepted as a ray hit; pushed to the SSR trace.
        f32 SsrThickness = 0.5f;
        /// @brief SSR roughness cutoff; surfaces rougher than this trace no reflection ray.
        f32 SsrMaxRoughness = 0.8f;

        /// @brief RAW (non-tile-remapped) per-cascade world → light-clip transforms this frame.
        ///
        /// Computed by the renderer on every Execute from the first directional light (identity
        /// when there is none). The shadow pass renders cascade k with CascadeViewProj[k] pushed
        /// and the viewport placing it in its atlas tile. Only [0, CascadeCount) are valid. The
        /// lighting pass reads the tile-remapped matrices from the set-1 ShadowConstants buffer.
        /// A caller's values are overwritten.
        std::array<mat4, MaxCascades> CascadeViewProj{};
        /// @brief RAW per-cascade caster-cull transforms this frame; near-extended toward the light.
        ///
        /// The shadow pass culls casters against these (a caster between the light and the
        /// slice must survive) but renders through CascadeViewProj, whose tight near plane the
        /// depth-clamped pipeline pancakes those casters onto. Identical to CascadeViewProj on
        /// a device without depth clamp. Only [0, CascadeCount) are valid; a caller's values
        /// are overwritten.
        std::array<mat4, MaxCascades> CascadeCullViewProj{};
        /// @brief Number of valid entries in CascadeViewProj; set by the renderer each Execute.
        u32 CascadeCount = 0;

        /// @brief Shadowed punctual lights selected this frame (the first MaxShadowedPunctual shadow-casting lights).
        ///
        /// The punctual shadow pass renders each record's views into the atlas; the lighting pass
        /// samples Records[slot]. PunctualShadowCount records are valid. A caller's values are
        /// overwritten by the renderer each Execute.
        std::array<PunctualShadowRecord, MaxShadowedPunctual> PunctualShadows{};
        /// @brief Number of valid entries in PunctualShadows.
        u32 PunctualShadowCount = 0;

        /// @brief RAW (non-tile-remapped) per-record/per-face world → light-clip transforms this frame.
        ///
        /// Parallel to PunctualShadows; computed by the renderer on every Execute. A spot fills
        /// [slot][0]; a point fills [slot][0..5]. The punctual shadow pass renders each view with
        /// this raw matrix pushed and the viewport placing it in the record's atlas tile, and culls
        /// against the raw (not tile-remapped) frustum. Only [0, PunctualShadowCount) are valid; a
        /// caller's values are overwritten.
        std::array<std::array<mat4, CubeFaceCount>, MaxShadowedPunctual>
            PunctualShadowRawViewProj{};

        /// @brief Resident mesh candidates for this frame; set by the renderer on every Execute.
        ///
        /// The g-buffer pass culls this span against the camera frustum; the shadow pass culls it
        /// against each cascade's light frustum. Borrowed broadphase-cached scratch valid only for
        /// the Execute that produced it. A caller's value is overwritten.
        std::span<const VisibleMesh> Visible;

        /// @brief The renderer's spatial broadphase; set on every Execute.
        ///
        /// A pass queries it (Cull) for the candidate indices its frustum touches; returned ids
        /// index Visible. A caller's value is overwritten.
        const SceneBroadphase* Broadphase = nullptr;

        /// @brief The per-instance skinning palette descriptor set; set by the renderer each Execute.
        ///
        /// Bound by the shadow passes for skinned casters (and the geometry pass for skinned
        /// draws). Holds the same buffer the geometry pass fills in PrepareDraws.
        Ref<DescriptorSet> SkinningPalette;

        /// @brief This frame's PaletteBase per skinned entity (packed Entity → base); set each Execute.
        ///
        /// Filled by the geometry-pass draw preparation; a shadow pass looks up a skinned caster's
        /// palette base here so it casts its posed shadow. Borrowed; valid only for this Execute.
        const unordered_map<u64, u32>* SkinnedPaletteBases = nullptr;
    };

    /// @brief Long-lived deferred render pipeline owning an offscreen target.
    ///
    /// Single-owner (Unique); Create is the factory. See the namespace-level doc
    /// for the lifetime-split surface (Create/Resize/Configure/Execute/GetOutput).
    class SceneRenderer
    {
    public:
        /// @brief Creates a SceneRenderer and compiles its initial render graph.
        static Unique<SceneRenderer> Create(const SceneRendererInfo& info);
        /// @brief Destroys all owned resources through the deferred-destruction retire path.
        ~SceneRenderer();

        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        /// @brief Recreates the extent-sized output and recompiles the internal graph.
        ///
        /// Invalidates the Ref a prior GetOutput() returned. A consumer caching a
        /// bindless TextureHandle or ImGui texture from it must re-fetch and re-register
        /// after this call.
        /// @param extent  New render extent in pixels.
        void Resize(uvec2 extent);

        /// @brief Recreates affected resources and recompiles the graph's topology.
        ///
        /// Invalidates the prior GetOutput() Ref like Resize. ShadowResolution and
        /// PunctualShadowResolution are clamped to GetMaxShadowResolution() /
        /// GetMaxPunctualShadowResolution() before any atlas is sized, so an over-large
        /// request degrades to the largest valid atlas rather than a fatal driver error.
        /// @param settings  New topology and sizing knobs.
        void Configure(const SceneRendererSettings& settings);

        /// @brief Largest directional-cascade tile resolution this device supports.
        ///
        /// The directional atlas tiles its cascades in a grid at most two tiles per
        /// side (2×2 at four cascades), so a tile larger than
        /// Context::GetMaxImageDimension2D() / 2 would overflow the device's image
        /// limit. Configure() clamps ShadowResolution to this; a UI sizing the knob
        /// uses it as the slider maximum.
        /// @return The maximum valid ShadowResolution, in texels.
        [[nodiscard]] u32 GetMaxShadowResolution() const;

        /// @brief Largest punctual-atlas tile resolution this device supports.
        ///
        /// The punctual atlas tiles CubeFaceCount columns × MaxShadowedPunctual rows,
        /// so its widest dimension is CubeFaceCount × resolution; a tile larger than
        /// Context::GetMaxImageDimension2D() / CubeFaceCount would overflow the
        /// device's image limit. Configure() clamps PunctualShadowResolution to this;
        /// a UI sizing the knob uses it as the slider maximum.
        /// @return The maximum valid PunctualShadowResolution, in texels.
        [[nodiscard]] u32 GetMaxPunctualShadowResolution() const;

        /// @brief Replays the internal graph against this frame's view.
        ///
        /// Records each pass unit's draws. Never reallocates or recompiles.
        /// @param cmd   Command buffer to record into.
        /// @param view  Per-frame scene input; the renderer overwrites its output fields.
        void Execute(CommandBuffer& cmd, const SceneView& view);

        /// @brief Returns the sampleable view of the owned result.
        ///
        /// The image is allocated at the high-water-mark extent; under dynamic resolution only
        /// its top-left GetValidExtent() sub-rect holds this frame's rendered content. A consumer
        /// upscales that sub-rect (see GetValidExtent). Invalidated by Resize and Configure;
        /// re-fetch after those calls.
        [[nodiscard]] Ref<ImageView> GetOutput() const;

        /// @brief Returns the valid sub-rect extent of the output from the last Execute.
        ///
        /// round(allocExtent * SceneView::RenderScale) from the last Execute, clamped to
        /// [1, allocExtent]. The output image (GetOutput) is allocated at the full extent; only
        /// the top-left GetValidExtent() texels are this frame's content, and a consumer sampling
        /// it must remap its UVs into [0, GetValidExtent()/allocExtent] to upscale (a half-texel
        /// inset avoids bleeding past the valid edge). Equal to the allocated extent before the
        /// first Execute and whenever RenderScale is 1.0.
        [[nodiscard]] uvec2 GetValidExtent() const;

        /// @brief Returns the total resident per-submesh candidate count from the last Execute.
        ///
        /// One per submesh of every (Transform, MeshRenderer) with a loaded mesh, before any
        /// frustum cull. Zero before the first Execute.
        [[nodiscard]] u32 GetLastVisibleCount() const;

        /// @brief Returns the number of submesh candidates that survived the camera-frustum cull in the last Execute.
        ///
        /// The BVH frustum descent's survivor count — one per per-submesh candidate the
        /// camera frustum kept, always <= GetLastVisibleCount(). A materialless or
        /// not-yet-resident survivor still counts. Under CullMode::GPU this is the count
        /// uploaded to the cull compute pass. The middle stage of the gathered →
        /// frustum-survived → drawn funnel. Zero before the first Execute.
        [[nodiscard]] u32 GetFrustumSurvivedCount() const;

        /// @brief Returns the per-submesh count the g-buffer pass drew in the last Execute.
        ///
        /// Equals GetFrustumSurvivedCount() — every frustum survivor is a draw under
        /// CullMode::CPU (a materialless or not-yet-resident survivor counts even though it
        /// records no command). The terminal stage of the gathered → frustum-survived → drawn
        /// funnel. Under CullMode::GPU the occlusion stage shows up separately as
        /// GetLastGpuSurvivorCount() (the device-side draws after the hi-Z test zeros occluded
        /// commands). Zero before the first Execute.
        [[nodiscard]] u32 GetLastDrawnCount() const;

        /// @brief Returns the aggregate point-field draw statistics from the last Execute.
        ///
        /// The point-field pass's per-frame counters — fields walked, cells in-frustum / measured,
        /// resolved sprite draws issued and points submitted, and aggregate splats drawn — summed
        /// across every field. The point-field analogue of the mesh cull funnel
        /// (GetLastVisibleCount / GetFrustumSurvivedCount / GetLastDrawnCount): a consumer profiling
        /// a heavy field reads the sprite/splat split here instead of GPU timestamps. All zero when
        /// no point-field pass is active or before the first Execute that drew a field.
        /// @return The last Execute's point-field draw statistics.
        [[nodiscard]] PointFieldStats GetPointFieldStats() const;

        /// @brief Forces the point-field pass onto the direct sprite path (A/B verification hook).
        ///
        /// Bypasses the per-point compute expansion so both paths can be captured and compared; a
        /// no-op when no point-field pass is active or on a device without the compute path. The
        /// compute and direct paths draw a surviving point bit-comparably (modulo record f16
        /// quantization), so this is the reference the automatic selection is checked against.
        /// @param force True to draw every field direct; false to restore automatic selection.
        void SetPointFieldForceDirect(bool force);

        /// @brief Returns the topology/sizing settings in effect, as of the last Create/Configure.
        ///
        /// The requested settings (shadow resolutions clamped to the device caps at apply time);
        /// a settings editor reads its starting state from here rather than mirroring a copy.
        [[nodiscard]] const SceneRendererSettings& GetSettings() const { return m_Settings; }

        /// @brief Returns the cull mode actually in effect, after the device-support fallback.
        ///
        /// Equals Settings.Cull when CullMode::GPU is requested and
        /// Context::IsGpuDrivenCullingSupported() is true; otherwise CullMode::CPU. Reflects the
        /// last Configure/Create.
        [[nodiscard]] SceneRendererSettings::CullMode GetActiveCullMode() const;

        /// @brief Returns the GPU cull's survivor count read back from the previous Execute.
        ///
        /// Under CullMode::GPU this is the number of candidates whose instanceCount the cull wrote
        /// 1 (the draws the indirect submission actually issued), read back one frame late so it
        /// never gates the draw. Zero under CullMode::CPU and before the second GPU Execute.
        [[nodiscard]] u32 GetLastGpuSurvivorCount() const;

        /// @brief Reads back the GPU cull's per-candidate instanceCount verdicts from the last Execute.
        ///
        /// One entry per camera-frustum survivor candidate (in dispatch order), each 1 (drawn) or 0
        /// (occluded), downloaded from the indirect command buffer. Blocks on a device read; exposed
        /// for the GPU↔CPU set-equivalence test. Empty under CullMode::CPU.
        /// @return The per-candidate instanceCount verdicts, or empty if no GPU Execute has run.
        [[nodiscard]] vector<u32> ReadbackGpuSurvivorFlags() const;

        /// @brief Returns true if the broadphase rebuilt its tree during the most recent Execute.
        ///
        /// False on a fully static frame (the scene's spatial version was unchanged).
        /// Diagnostic only; the rendered image is identical regardless.
        [[nodiscard]] bool DidBroadphaseRebuildLastFrame() const;

        /// @brief Returns true if the atmosphere LUTs regenerated during the most recent Execute.
        ///
        /// True only on a frame the Atmosphere parameters changed (or the first frame the
        /// atmosphere sky was active) — the once-per-change contract. Diagnostic only.
        [[nodiscard]] bool DidRegenerateAtmosphereLastFrame() const;

        /// @brief Returns the number of nodes in the broadphase BVH (internal + leaf).
        ///
        /// Diagnostic only. Zero before the first Execute or with no resident candidates.
        [[nodiscard]] u32 GetBroadphaseNodeCount() const;

        /// @brief Returns the g-buffer albedo (G0) view.
        ///
        /// Renderer-owned; invalidated by Resize. Exposed for tests and tooling; normal
        /// consumers read only GetOutput().
        [[nodiscard]] Ref<ImageView> GetAlbedoView() const;
        /// @brief Returns the g-buffer world-normal (G1) view. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetNormalView() const;
        /// @brief Returns the g-buffer packed ORM (G2) view. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetOrmView() const;
        /// @brief Returns the depth buffer view. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetDepthView() const;

        /// @brief Returns the whole-chain sampled view of the hi-Z depth pyramid.
        ///
        /// The max-Z mip chain reduced from the depth target each Execute. Renderer-owned and
        /// persisted across frames; invalidated by Resize and Configure. Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetHiZView() const;

        /// @brief Returns the storage view of hi-Z mip @p level (one mip per view).
        ///
        /// Exposed for tests reading back a single reduced mip. Invalidated by Resize and Configure.
        /// @param level  Mip level in [0, mip count).
        [[nodiscard]] Ref<ImageView> GetHiZMipView(u32 level) const;

        /// @brief Returns the number of mip levels in the hi-Z pyramid.
        [[nodiscard]] u32 GetHiZMipCount() const;

        /// @brief Returns whether the previous-frame pyramid is valid to occlusion-test against this frame.
        ///
        /// False on the first Execute, the Execute immediately after a Resize/Configure
        /// recreated the pyramid, and on a detected large view delta (translation past a
        /// fraction of the scene diagonal, forward-axis rotation past the threshold, or
        /// any projection change). When false the GPU cull skips occlusion (frustum-only),
        /// so stale or absent history can only leave a draw in, never wrongly cull it.
        /// Reflects the most recent Execute; defaults false before the first.
        [[nodiscard]] bool IsHiZHistoryValid() const;

        /// @brief Returns the camera world->clip matrix Execute captured last frame.
        ///
        /// The occlusion test screen-bounds a candidate against the previous-frame pyramid,
        /// so it must use the previous-frame view-projection (decision 2). Identity before
        /// the first Execute. Valid to test against only when IsHiZHistoryValid() is true.
        [[nodiscard]] mat4 GetPreviousViewProj() const;

        /// @brief Returns the HDR target the deferred lighting pass writes before tonemap.
        ///
        /// Exposed for tests and tooling. Invalidated by Resize.
        [[nodiscard]] Ref<ImageView> GetHdrView() const;

        /// @brief Returns the bloom composite result the tonemap stage reads when Bloom is on.
        ///
        /// Null when Bloom is off (tonemap reads the raw HDR target instead). Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetBloomResultView() const;

        /// @brief Returns the persisted TAA history target, or null when TAA is off.
        ///
        /// Holds the previous frame's resolved HDR. Renderer-owned; invalidated by Resize
        /// and Configure. Exposed for tests inspecting temporal accumulation.
        [[nodiscard]] Ref<ImageView> GetTaaHistoryView() const;

        /// @brief Returns the per-object velocity target (g-buffer channel G3).
        ///
        /// RG screen-space motion vectors written by the surface pass as a fourth g-buffer
        /// channel every frame (not a separate prepass, never null). Renderer-owned;
        /// invalidated by Resize and Configure. Exposed for tests.
        [[nodiscard]] Ref<ImageView> GetVelocityView() const;

        /// @brief Returns the punctual shadow atlas view (set 1 binding 4).
        ///
        /// A 2D depth atlas of MaxShadowedPunctual·CubeFaceCount tiles, SampleCmp'd by the
        /// lighting pass. Renderer-owned; invalidated by Resize and Configure. Exposed for
        /// the render-pass handoff and for tests inspecting the atlas extent.
        [[nodiscard]] Ref<ImageView> GetPunctualShadowView() const;

        /// @brief Returns the immediate-mode debug-draw accumulator for this renderer.
        ///
        /// A caller pushes lines/billboards each frame; the DebugDrawScenePass flushes them when
        /// SceneRendererSettings::DebugDraw is on. The accumulator clears at the start of every
        /// Execute, so a primitive is re-pushed each frame it should appear. A mutable reference
        /// from a const method by the Native-idiom rule: the renderer's constness is its own
        /// identity, not the per-frame accumulator state.
        /// @return The renderer-owned DebugDraw accumulator.
        [[nodiscard]] DebugDraw& GetDebugDraw() const;

        /// @brief Records a pending pick at a render-target texel, serviced by the next Execute(s).
        ///
        /// The next Execute that runs the picking pass copies the (2*Picking::SearchRadius+1)²
        /// texel neighborhood around @p texel out of the id target into a host-visible staging
        /// buffer, on the graphics queue; the result becomes readable through PollPickId() once that
        /// frame's GPU work has completed (a frame or two later — never a WaitIdle). A request issued
        /// while one is already in flight replaces it. A no-op when SceneRendererSettings::Picking is
        /// not set.
        /// @param texel  The render-target texel to pick, in allocation pixels (top-left origin).
        /// @pre SceneRendererSettings::Picking is set on this renderer.
        void RequestPick(uvec2 texel);

        /// @brief Returns true when a pick request has been issued but not yet resolved or polled.
        ///
        /// Covers the window between RequestPick() and the PollPickId() that consumes the result.
        [[nodiscard]] bool IsPickInFlight() const;

        /// @brief Returns the resolved pick id once a requested pick's readback is ready, else nullopt.
        ///
        /// Applies the screen-space search radius to the staged neighborhood: the exact cursor texel
        /// wins when non-zero; otherwise the nearest non-zero id to the cursor. Returns the raw pick
        /// id (packed entity index + 1, or Picking::NoEntityId for background). Returns nullopt while
        /// the readback is still in flight (the staged frame has not completed). Consuming the result
        /// clears the in-flight state, so a caller polls each frame until it returns a value.
        /// @return The resolved pick id when ready; nullopt while the readback is still pending.
        [[nodiscard]] optional<u32> PollPickId();

    private:
        explicit SceneRenderer(const SceneRendererInfo& info);

        /// @brief Recreates the owned output image and view at the current extent and format.
        void CreateOutput();
        /// @brief Recreates g-buffer images/views at the current extent and (re-)registers them into bindless.
        void CreateGBuffer();
        /// @brief Loads the baked LTC lookup tables from the core pack into textures and registers them.
        void CreateLtcResources();
        /// @brief Recreates the HDR image/view at the current extent and (re-)registers it into bindless.
        void CreateHdr();
        /// @brief Builds the engine-owned fullscreen pipelines and loads the core PostProcess materials.
        ///
        /// Called once at Create; the lighting pipeline writes the HDR format, the debug-blit pipelines
        /// write the output format.
        void CreatePipelines();

        /// @brief Rebuilds the pass set from Settings.Mode and recompiles the RenderGraph.
        void Rebuild();

        /// @brief Resolves the scene's PointField components into this Execute's live field set.
        ///
        /// Walks View<PointField> off @p view.World, applies each component's authored Lod to its
        /// built field, and collects every live (non-null, non-empty) Renderer::PointField into
        /// m_PointFields for the point-field pass to draw — the lights model. When whether any live
        /// field exists changes between Executes, drives an internal Rebuild to insert or drop the
        /// pass at the frame boundary (reusing the imported output, so GetOutput() stays valid).
        /// @param view  The scene to resolve the point fields from.
        void ResolvePointFields(const SceneView& view);

        /// @brief Resolves the scene's VolumeField components into this Execute's live field set.
        ///
        /// Walks View<VolumeField> off @p view.World, folds each component's authored knobs with its
        /// built field into a VolumeFieldInstance, and collects every live (non-null) field into
        /// m_VolumeFields for the volume pass to march — the lights model. When whether any live
        /// field exists changes between Executes, drives an internal Rebuild to insert or drop the
        /// pass at the frame boundary (reusing the imported output, so GetOutput() stays valid).
        /// @param view  The scene to resolve the volume fields from.
        void ResolveVolumeFields(const SceneView& view);

        /// @brief Fills the per-draw DrawData buffer (and, under GPU mode, the candidate buffer + groups) for this Execute.
        ///
        /// Computes the camera-frustum survivors, writes the current frame's DrawData region, and
        /// builds the geometry pass's submission plan (m_Internal->Plan). The geometry pass reads
        /// the plan at record time.
        /// @param view                The frame's scene view (broadphase already synced).
        /// @param viewConstantsIndex  This frame's view-constants ring region.
        void PrepareDraws(const SceneView& view, u32 viewConstantsIndex);

        /// @brief Vulkan context for all resource creation.
        Context& m_Context;
        /// @brief Asset manager for engine shader loading.
        AssetManager& m_Assets;
        /// @brief Pixel format of the owned output target.
        Format m_OutputFormat;
        /// @brief Allocated render extent — the high-water-mark every target is sized to.
        uvec2 m_Extent;
        /// @brief This frame's valid sub-rect extent (round(m_Extent * RenderScale)); GetValidExtent.
        uvec2 m_ValidExtent;
        /// @brief Previous frame's sub-rect UV mapping (validExtent/allocExtent), for TAA history.
        vec2 m_PreviousRenderScaleUV{1.0f};
        /// @brief Previous frame's clamped max valid UV ((validExtent-0.5)/allocExtent), for TAA history.
        vec2 m_PreviousMaxValidUV{1.0f};
        /// @brief Current topology and sizing knobs.
        SceneRendererSettings m_Settings;

        /// @brief Owned output image.
        Ref<Image> m_OutputImage;
        /// @brief View over m_OutputImage.
        Ref<ImageView> m_OutputView;

        /// @brief G-buffer targets (G0 albedo, G1 world-normal, G2 packed ORM, depth).
        ///
        /// Renderer-owned (sampled downstream, so not graph transients) and imported into
        /// the internal graph.
        Ref<Image> m_AlbedoImage;
        /// @brief View over m_AlbedoImage.
        Ref<ImageView> m_AlbedoView;
        /// @brief G1 world-normal image.
        Ref<Image> m_NormalImage;
        /// @brief View over m_NormalImage.
        Ref<ImageView> m_NormalView;
        /// @brief G2 packed ORM image.
        Ref<Image> m_OrmImage;
        /// @brief View over m_OrmImage.
        Ref<ImageView> m_OrmView;
        /// @brief Depth image.
        Ref<Image> m_DepthImage;
        /// @brief View over m_DepthImage.
        Ref<ImageView> m_DepthView;

        /// @brief Camera world->clip captured at the end of last Execute (this frame's pyramid pairs with it).
        ///
        /// Identity before the first Execute. Packed into the shared set-0 view-constants block's
        /// PrevViewProj unconditionally every frame, and passed to the GPU cull subsystem as the
        /// previous-frame matrix the occlusion test screen-bounds candidates with.
        mat4 m_PreviousViewProj{1.0f};

        /// @brief HDR target the deferred lighting pass writes (linear, unbounded range).
        ///
        /// Renderer-owned and imported like the g-buffer; tonemap maps it to the output format.
        /// Single-copy: one Execute resolves and completes before the next begins, so no
        /// cross-frame ring buffer is needed — the output is consumed in the frame it is written.
        Ref<Image> m_HdrImage;
        /// @brief View over m_HdrImage.
        Ref<ImageView> m_HdrView;

        /// @brief Per-object screen-space motion vector target — g-buffer channel G3.
        ///
        /// RG16Sfloat, full extent. The surface pass writes it as SV_Target3 alongside the
        /// other g-buffer channels every frame (no separate prepass), so it is always
        /// allocated; the TAA resolve and the MotionVectors debug blit read it. Created in
        /// CreateGBuffer and recreated on Resize/Configure with the rest of the g-buffer.
        Ref<Image> m_VelocityImage;
        /// @brief View over m_VelocityImage.
        Ref<ImageView> m_VelocityView;
        /// @brief Bindless slot for the velocity view; the resolve samples per-object motion through it.
        TextureHandle m_VelocityHandle;

        /// @brief HDR emissive target — g-buffer channel G4.
        ///
        /// B10G11R11Ufloat, full extent. The surface pass writes authored emission as SV_Target4
        /// alongside the other g-buffer channels every frame (no separate pass), so it is always
        /// allocated; the lighting pass samples it and adds it into the outgoing radiance. Created
        /// in CreateGBuffer and recreated on Resize/Configure with the rest of the g-buffer.
        Ref<Image> m_EmissiveImage;
        /// @brief View over m_EmissiveImage.
        Ref<ImageView> m_EmissiveView;
        /// @brief Bindless slot for the emissive view; the lighting pass samples G4 through it.
        TextureHandle m_EmissiveHandle;

        /// @brief LTC inverse-matrix lookup table for area-light shading (RGBA32F, LtcLut::Size²).
        Ref<Image> m_LtcMatImage;
        /// @brief View over m_LtcMatImage.
        Ref<ImageView> m_LtcMatView;
        /// @brief Bindless slot for the LTC matrix LUT.
        TextureHandle m_LtcMatHandle;
        /// @brief LTC magnitude/Fresnel lookup table for area-light shading (RGBA32F, LtcLut::Size²).
        Ref<Image> m_LtcMagImage;
        /// @brief View over m_LtcMagImage.
        Ref<ImageView> m_LtcMagView;
        /// @brief Bindless slot for the LTC magnitude LUT.
        TextureHandle m_LtcMagHandle;

        /// @brief The GPU occlusion-cull cluster + the hi-Z pyramid it tests against; created at Create.
        ///
        /// Owns the hi-Z reduce set/pipeline layouts + reduce pipeline, the pyramid image/views/sets,
        /// the cull compute cluster (candidate/indirect/count buffers, pipeline/layout/set), the
        /// cross-frame history-validity state, and the active cull mode. The SSR chain borrows its
        /// hi-Z reduce layouts, so it is constructed before the SSR chain; its pyramid is recreated
        /// from the g-buffer create/recreate tail (ResizeHiZ binds the fresh depth view).
        Unique<GpuCullSystem> m_GpuCull;

        /// @brief The screen-space-reflection chain — scene-color intermediate, mip chain, min-Z pyramid, and sweep.
        ///
        /// Its blur pipeline layout reserves the bloom down/up set layout and its min-Z reduce
        /// pipeline builds on the GPU cull subsystem's hi-Z reduce layout, so it is constructed after
        /// the GPU cull subsystem and the bloom subsystem exist; Recreate rebuilds the chain at the
        /// SsrResolution scale after the g-buffer depth and HDR targets.
        Unique<SsrChain> m_Ssr;

        /// @brief The entity-id picking cluster + its request → stage → poll state machine; created at Create.
        ///
        /// Owns the R32Uint EntityId target + dedicated depth (allocated only while picking is on),
        /// the lazily-built static/skinned id pipelines, and the readback ring. The renderer wires
        /// its passes inline in Rebuild, reading this subsystem's pipeline pointers and graph ids.
        Unique<PickingSystem> m_Picking;

        /// @brief The pre-translucent refraction grab — scene-color/depth intermediates and the copy pipeline.
        Unique<RefractionGrab> m_Refraction;

        /// @brief Shared sampler fullscreen passes use to sample the g-buffer and HDR target.
        Ref<Sampler> m_Sampler;

        /// @brief Bindless slots for the g-buffer/HDR views and the shared sampler.
        ///
        /// Registered once at Create; re-registered on Resize (old slots released through the
        /// per-frame retire window).
        TextureHandle m_AlbedoHandle;
        /// @brief Bindless slot for the world-normal view.
        TextureHandle m_NormalHandle;
        /// @brief Bindless slot for the ORM view.
        TextureHandle m_OrmHandle;
        /// @brief Bindless slot for the depth view.
        TextureHandle m_DepthHandle;
        /// @brief Bindless slot for the HDR view.
        TextureHandle m_HdrHandle;
        /// @brief Bindless slot for the shared sampler.
        SamplerHandle m_SamplerHandle;

        /// @brief Engine-owned lighting pipeline writing the HDR format.
        ///
        /// Built once at Create from the core pack's shaders. The pass set Mode references the
        /// pipelines it needs; the rest stay built but unused.
        Ref<class GraphicsPipeline> m_LightingPipeline;
        /// @brief Layout for m_LightingPipeline.
        Ref<class PipelineLayout> m_LightingLayout;

        /// @brief SSAO-enabled lighting variant; selected when Settings.AO is on.
        ///
        /// A separate fragment shader compiled with the AO fold. SSAO is a compile-time pipeline
        /// variant, not a per-frame branch.
        Ref<class GraphicsPipeline> m_SsaoLightingPipeline;
        /// @brief Layout for the SSAO lighting variant.
        Ref<class PipelineLayout> m_SsaoLightingLayout;

        /// @brief Cascade-debug lighting variant (DebugView::Cascades).
        ///
        /// Tint fragment shader over the plain lighting layout (set 1 + non-SSAO push block),
        /// writing the output format directly. Reuses m_LightingLayout.
        Ref<class GraphicsPipeline> m_CascadeDebugPipeline;

        /// @brief Fullscreen skybox pipeline (radiance cube over the lit HDR), writing HdrFormat.
        Ref<class GraphicsPipeline> m_SkyboxPipeline;
        /// @brief Layout for m_SkyboxPipeline: the IBL set (set 1) + the skybox push block.
        Ref<class PipelineLayout> m_SkyboxLayout;

        /// @brief Fullscreen procedural-atmosphere sky pipeline (LUTs over the lit HDR), writing HdrFormat.
        Ref<class GraphicsPipeline> m_SkyPipeline;
        /// @brief Layout for m_SkyPipeline: the atmosphere set (set 1) + the sky push block.
        Ref<class PipelineLayout> m_SkyLayout;

        /// @brief SSAO fullscreen pipeline writing the R8 AO target.
        Ref<class GraphicsPipeline> m_SsaoPipeline;
        /// @brief Layout for the SSAO pipeline.
        Ref<class PipelineLayout> m_SsaoLayout;

        /// @brief Debug blit for the albedo channel.
        Ref<class GraphicsPipeline> m_AlbedoBlitPipeline;
        Ref<class PipelineLayout> m_AlbedoBlitLayout;
        /// @brief Debug blit for the normal channel.
        Ref<class GraphicsPipeline> m_NormalBlitPipeline;
        Ref<class PipelineLayout> m_NormalBlitLayout;
        /// @brief Debug blit for the depth buffer.
        Ref<class GraphicsPipeline> m_DepthBlitPipeline;
        Ref<class PipelineLayout> m_DepthBlitLayout;

        /// @brief ORM-channel blit shared by the Roughness/Metallic/Occlusion arms.
        ///
        /// The channel select is a push value, not a separate pipeline. Writes the output format.
        Ref<class GraphicsPipeline> m_OrmBlitPipeline;
        Ref<class PipelineLayout> m_OrmBlitLayout;
        /// @brief Debug blit for the SSAO target.
        Ref<class GraphicsPipeline> m_AoBlitPipeline;
        Ref<class PipelineLayout> m_AoBlitLayout;
        /// @brief Debug blit colorizing the per-object velocity target (DebugView::MotionVectors).
        Ref<class GraphicsPipeline> m_MotionBlitPipeline;
        Ref<class PipelineLayout> m_MotionBlitLayout;
        /// @brief Debug blit for the directional shadow atlas.
        Ref<class GraphicsPipeline> m_ShadowBlitPipeline;
        Ref<class PipelineLayout> m_ShadowBlitLayout;

        /// @brief The set-1 shadow descriptor system + punctual atlas + constants rings; created at Create.
        ///
        /// Owns the comparison sampler, the set-1 layout/set, the debug-blit layout/set/sampler,
        /// the dummy and punctual atlases, and both constants rings. The lighting layout reserves
        /// its set layout, so it exists before the pipelines. The directional cascade atlas is not
        /// owned here — ShadowScenePass owns it and Rebuild binds its view (or the dummy) into the
        /// set through RebuildSets.
        Unique<ShadowSystem> m_Shadows;

        /// @brief The compute mip-pyramid bloom battery — pyramid, pipelines, sets, and the sweep.
        ///
        /// Its down/up set layout is reserved by the SSR blur pipeline layout, so it is constructed
        /// before CreatePipelines; Resize rebuilds the extent-sized pyramid after the HDR target.
        Unique<BloomPyramid> m_Bloom;

        /// @brief The auto-exposure metering battery — histogram pipeline, ring, and adaptation state.
        Unique<AutoExposureMeter> m_AutoExposure;

        /// @brief The TAA resolve battery — resolve/copy pipelines, lit/history targets, reset gate.
        Unique<TaaResolve> m_Taa;

        /// @brief Number of frames-in-flight the renderer-owned rings are sized for.
        ///
        /// Seeded at construction (before the ring allocations below) and read by the draw-data,
        /// skinning-palette, and cull rings.
        u32 m_FramesInFlight = 0;

        /// @brief Core tonemap PostProcess material, loaded once at Create.
        ///
        /// The Final chain's terminal PostProcessScenePass drives it (HDR target as the
        /// runtime-bound input; Exposure written per Execute into its param block).
        AssetHandle<MaterialInstance> m_TonemapMaterial;

        /// @brief Renderer-owned pass units; rebuilt per Settings.Mode on every Rebuild.
        ///
        /// The geometry pass is always first; Mode selects the tail.
        vector<Unique<ScenePass>> m_Passes;

        /// @brief The point-field pass, held outside m_Passes and declared at the HDR tail anchor.
        ///
        /// The fields accumulate into the final HDR between the SSR composite and the bloom
        /// sweep — a position only the tail anchor occupies, so the pass cannot ride the list.
        /// Null unless the Final arm is built while a live HdrTail-placed field exists
        /// (m_PointFieldActive).
        Unique<ScenePass> m_PointFieldPass;

        /// @brief The scene-color point-field pass, for fields placed in the lit scene color.
        ///
        /// Rides m_Passes (it writes the in-list io.Hdr lit target), inserted ahead of the
        /// refraction copy and the translucent pass so translucents blend over the fields and the
        /// scene-color grab includes them. A non-owning observer for the stats/force-direct
        /// accessors; null unless the Final arm is built while a live SceneColor-placed field
        /// exists (m_ScenePointFieldActive).
        ScenePass* m_ScenePointFieldPass = nullptr;

        /// @brief BVH broadphase over resident draw candidates.
        ///
        /// Synced once at the top of Execute; its candidate span is pointed at by
        /// SceneView::Visible and its tree is queried by the g-buffer and shadow passes.
        /// A static scene does not rebuild the tree.
        SceneBroadphase m_Broadphase;

        /// @brief Per-frame copy of the broadphase candidates with interpolated world transforms.
        ///
        /// Filled only on a frame that interpolates (nonzero alpha and a scene with motion history):
        /// each candidate's World is re-blended between the last two Sim-tick snapshots and its
        /// WorldBounds recomputed, and SceneView::Visible points here instead of at the broadphase's
        /// current-tick candidates. Empty (and unused) on a static or tick-aligned frame.
        vector<VisibleMesh> m_InterpolatedCandidates;

        /// @brief Per-submesh frustum-survivor count from the last Execute.
        ///
        /// Set by PrepareDraws each Execute: the number of per-submesh candidates the camera
        /// frustum kept (a materialless or not-yet-resident survivor still counts). The middle
        /// funnel stage; the upload count under CullMode::GPU. Zero before the first Execute.
        u32 m_FrustumSurvivedCount = 0;

        /// @brief Per-submesh drawn count from the last Execute (equals m_FrustumSurvivedCount).
        ///
        /// The terminal funnel stage: every frustum survivor is a draw under CullMode::CPU.
        /// Zero before the first Execute.
        u32 m_LastDrawnCount = 0;

        /// @brief Allocates the mode-independent per-draw buffers + their descriptor sets.
        ///
        /// The per-draw DrawData SSBO, the skinning palette, and the identity candidate-id buffer —
        /// used by both cull modes (the buffer-indexed surface draw) — sized to
        /// MaxCullCandidates × frames-in-flight. The GPU-cull candidate/indirect/count buffers and
        /// the cull compute pipeline live on m_GpuCull. Called once at Create.
        void CreateCullResources();

        /// @brief Maximum per-submesh candidates a frame's per-draw / cull buffers hold.
        ///
        /// The fixed candidate maximum (decision 2): the indirect buffer covers this many slots,
        /// culled ones no-op. A frame exceeding it is clamped (the overflow submeshes are not
        /// drawn), asserted in a debug build.
        static constexpr u32 MaxCullCandidates = 4096;

        /// @brief Per-draw DrawData SSBO (set used by the surface pipeline's set 1, binding 0).
        ///
        /// Host-visible, ring-buffered for frames-in-flight (MaxCullCandidates records per region);
        /// the surface vertex stage reads its record by the candidate id folded with the pushed
        /// FrameBase. Drives both cull modes' buffer-indexed draw.
        Ref<Buffer> m_DrawDataBuffer;
        /// @brief Set 1 for the surface pipeline: binding 0 the DrawData SSBO.
        Ref<DescriptorSetLayout> m_DrawDataSetLayout;
        /// @brief Descriptor set bound at set 1 for every surface draw.
        Ref<DescriptorSet> m_DrawDataSet;

        /// @brief Identity candidate-id buffer bound to vertex binding 1 (instance rate).
        ///
        /// Element k holds k, so a draw's firstInstance = candidateId fetches candidateId as the
        /// instance attribute (the per-draw DrawData index). Created once; shared by both cull
        /// modes' draws. MaxCullCandidates elements.
        Ref<Buffer> m_CandidateIdBuffer;

        /// @brief Maximum bone matrices a single skinned instance contributes to the palette.
        static constexpr u32 MaxBonesPerSkinnedInstance = 256;
        /// @brief Maximum skinning matrices uploaded per frame across all skinned instances.
        static constexpr u32 MaxSkinningMatricesPerFrame = 8192;

        /// @brief Per-instance skinning palette (mat4 per bone), bound at set 2 for skinned draws.
        ///
        /// Host-visible, ring-buffered for frames-in-flight (MaxSkinningMatricesPerFrame matrices
        /// per region). Each skinned instance's bones are appended contiguously and its DrawData
        /// PaletteBase is the absolute index of its first bone in this buffer.
        Ref<Buffer> m_PaletteBuffer;
        /// @brief Set 2 for the skinned surface pipeline / set 1 for the skinned shadow pipeline: the palette SSBO.
        Ref<DescriptorSetLayout> m_PaletteSetLayout;
        /// @brief Descriptor set holding the palette buffer, bound for skinned draws.
        Ref<DescriptorSet> m_PaletteSet;
        /// @brief This frame's PaletteBase per skinned entity (packed Entity → base), read by the shadow passes.
        unordered_map<u64, u32> m_PaletteBaseByEntity;
        /// @brief Previous frame's PaletteBase per skinned entity; surface_skinned.vert skins the
        ///        previous position through it for velocity. Swapped from m_PaletteBaseByEntity each frame.
        unordered_map<u64, u32> m_PreviousPaletteBaseByEntity;

        /// @brief Reused per-frame frustum-survivor candidate ids (broadphase Cull scratch).
        ///
        /// Filled by PrepareDraws (the frustum descent) and iterated to lay out the draw slots for
        /// both cull modes; the GPU cull's device-side buffers live on m_GpuCull.
        vector<u32> m_CullScratch;

        /// @brief Imported resource ids re-declared on every Rebuild.
        ///
        /// Bound to their concrete views per Execute and threaded to pass units through PassIO.
        ResourceId m_AlbedoId;
        /// @brief Imported id for the world-normal target.
        ResourceId m_NormalId;
        /// @brief Imported id for the packed ORM target.
        ResourceId m_OrmId;
        /// @brief Imported id for the depth target.
        ResourceId m_DepthId;
        /// @brief Imported id for the HDR target.
        ResourceId m_HdrId;
        /// @brief Imported id for the lighting target under TAA (the resolve's current input).
        ResourceId m_LitId;
        /// @brief Imported id for the persisted TAA history target.
        ResourceId m_TaaHistoryId;
        /// @brief Imported id for the velocity g-buffer channel (G3), written every frame.
        ResourceId m_VelocityId;
        /// @brief Imported id for the emissive g-buffer channel (G4), written every frame.
        ResourceId m_EmissiveId;
        /// @brief Per-mip subresource handle for the bloom pyramid the down/up sweep reads and writes.
        MipChainId m_BloomChainId;
        /// @brief Imported id for the bloom composite result.
        ResourceId m_BloomResultId;
        /// @brief Imported buffer id for the auto-exposure histogram buffer.
        ResourceId m_AutoExposureId;
        /// @brief Imported id for the directional shadow atlas.
        ResourceId m_ShadowId;
        /// @brief Imported id for the SSAO target.
        ResourceId m_SsaoId;
        /// @brief Imported id for the SSR lit scene-color intermediate.
        ResourceId m_SsrSceneId;
        /// @brief Imported id for the refraction scene-color intermediate.
        ResourceId m_RefractionSceneId;
        /// @brief Imported id for the refraction scene-depth intermediate.
        ResourceId m_RefractionDepthId;
        /// @brief Per-mip subresource handle for the SSR reflection pyramid (trace + blur).
        MipChainId m_SsrReflectionChainId;
        /// @brief Per-mip subresource handle for the SSR min-Z pyramid (reduction + trace).
        MipChainId m_SsrHiZChainId;
        /// @brief Imported id for the final output target.
        ResourceId m_OutputId;

        /// @brief True when the last Rebuild wired the bloom chain (Final mode + Settings.Bloom).
        ///
        /// Execute binds the bloom imports and writes the bloom params only when true.
        bool m_BloomActive = false;

        /// @brief True when the last Rebuild wired the auto-exposure metering pass.
        ///
        /// Execute binds the histogram buffer import, averages the metered histogram, and drives
        /// the adapted exposure only when true.
        bool m_AutoExposureActive = false;

        /// @brief True when the last Rebuild wired the TAA passes (Final mode + Settings.TAA).
        ///
        /// Execute jitters the projection, binds the lit/history imports, and pushes the
        /// resolve's history-validity flag only when true.
        bool m_TaaActive = false;

        /// @brief Monotonic frame counter driving the Halton jitter sequence.
        ///
        /// Incremented every Execute (independent of the TAA toggle so toggling on does not
        /// snap the sequence). Folds into TaaJitterSampleCount.
        u64 m_FrameIndex = 0;

        /// @brief Previous frame's world matrix per entity, keyed by a packed Entity id.
        ///
        /// The surface pass writes velocity from each drawn object's prior transform; PrepareDraws
        /// looks it up here and writes it into the per-draw record (DrawData.PrevWorld). An entity
        /// absent (first seen) reprojects with zero object motion. Maintained every frame (velocity
        /// is always written); swapped from m_CurrentWorlds at the end of each Execute.
        unordered_map<u64, mat4> m_PreviousWorlds;
        /// @brief This frame's world matrix per entity; swapped into m_PreviousWorlds after Execute.
        unordered_map<u64, mat4> m_CurrentWorlds;

        /// @brief True when the last Rebuild wired the directional shadow pass (Final mode + Settings.Shadows).
        ///
        /// Execute binds the shadow import and writes the light-space matrix only when true.
        bool m_ShadowActive = false;

        /// @brief Non-owning pointer to the wired ShadowScenePass, or null when shadows are compiled out.
        ///
        /// The renderer reads its produced atlas view to thread into PassIO and to bind the shadow
        /// import per Execute. The pass outlives this pointer (m_Passes is cleared and rebuilt together).
        ShadowScenePass* m_ShadowPass = nullptr;

        /// @brief True when the last Rebuild wired the punctual shadow pass.
        ///
        /// Execute binds the punctual atlas import only when true. The pass renders the
        /// shadow system's punctual atlas (m_Shadows->GetPunctualView()).
        bool m_PunctualShadowActive = false;
        /// @brief Imported id for the punctual shadow atlas.
        ResourceId m_PunctualShadowId;
        /// @brief Non-owning pointer to the wired punctual shadow pass.
        PunctualShadowScenePass* m_PunctualShadowPass = nullptr;

        /// @brief True when the last Rebuild wired the SSAO pass (Final mode + Settings.AO).
        ///
        /// Execute binds the AO import only when true.
        bool m_SsaoActive = false;
        /// @brief Non-owning pointer into m_Passes to the SsaoScenePass; null when AO is off.
        class SsaoScenePass* m_SsaoPass = nullptr;

        /// @brief Non-owning pointer into m_Passes to the SkyMaterialScenePass; null when the sky is not a material.
        ///
        /// Execute forwards the resolved SceneView::SkyMaterial to it before the graph runs.
        class SkyMaterialScenePass* m_SkyMaterialPass = nullptr;

        /// @brief True when the last Rebuild wired the SSR passes (Final mode + Settings.SSR, or the
        ///        Reflections debug arm). Execute binds the SSR imports only when true.
        bool m_SsrActive = false;

        /// @brief True when the last Rebuild wired the scene-color copy pass (a scene-composited
        ///        mode + Settings.Refraction). Execute binds the refraction import and populates
        ///        the view block's SceneColor handles only when true.
        bool m_RefractionActive = false;

        /// @brief The sky-resolve state machine and the three sky radiance-cube helpers; created at Create.
        ///
        /// Owns the image-based-lighting maps (set 2 for the lighting pass), the procedural-atmosphere
        /// LUTs (set 1 for the sky pass), and the baked-sky cube, plus the whole resolve state machine
        /// (resolved source-kind/tier/bake-mode, the once-per-change dirty gates, the projected
        /// skylight SH). Created before CreatePipelines so the lighting and sky layouts reserve its
        /// consumer set layouts; Rebuild reaches the sets/layouts and the resolved kind/tier through
        /// its getters.
        Unique<SkyResolver> m_SkyResolver;

        /// @brief Immediate-mode debug-draw accumulator flushed by the DebugDrawScenePass.
        ///
        /// Mutable so GetDebugDraw() (a const accessor) hands out a writable reference: the
        /// renderer's constness is its own identity, not the per-frame accumulator. Cleared at
        /// the start of every Execute.
        mutable DebugDraw m_DebugDraw;

        /// @brief This Execute's live HdrTail-placed point fields, borrowed from the scene's
        /// PointField components.
        ///
        /// Refilled every Execute by ResolvePointFields walking View<PointField>; the point-field
        /// pass reads it by pointer and draws each field. A borrow only — the scene's components own
        /// the fields' lifetimes, so a dropped field is simply absent next Execute (no teardown
        /// ordering). Empty when no component carries a live field.
        vector<const PointField*> m_PointFields;

        /// @brief This Execute's live SceneColor-placed point fields; m_ScenePointFieldPass's set.
        vector<const PointField*> m_ScenePointFields;

        /// @brief Persisted point-field force-direct hook, reapplied when the passes are rebuilt.
        bool m_PointFieldForceDirect = false;

        /// @brief Whether the current pass set carries the tail point-field pass; gates the
        /// internal Rebuild.
        ///
        /// True once ResolvePointFields has seen a live field and wired the pass. Compared against
        /// each Execute's presence so the pass inserts on the first live field and drops when the
        /// last one goes, recompiling at the frame boundary (reusing the imported output).
        bool m_PointFieldActive = false;

        /// @brief Whether the current pass set carries the scene-color point-field pass.
        bool m_ScenePointFieldActive = false;

        /// @brief This Execute's live volume fields, resolved from the scene's VolumeField components.
        ///
        /// Refilled every Execute by ResolveVolumeFields walking View<VolumeField>; the volume pass
        /// reads it by pointer and marches each field. A borrow of the built fields (the scene's
        /// components own their lifetimes), with the authored knobs folded in per instance. Empty
        /// when no component carries a live field.
        vector<VolumeFieldInstance> m_VolumeFields;

        /// @brief Whether the current pass set carries the volume pass; gates the internal Rebuild.
        ///
        /// True once ResolveVolumeFields has seen a live field and wired the pass. Compared against
        /// each Execute's presence so the pass inserts on the first live field and drops when the last
        /// one goes, recompiling at the frame boundary (reusing the imported output).
        bool m_VolumeFieldActive = false;

        /// @brief Opaque compiled graph; replayed every Execute.
        ///
        /// Held behind an opaque pointer so this header stays free of the full CompiledGraph type.
        struct Internal;
        Unique<Internal> m_Internal;
    };
}
