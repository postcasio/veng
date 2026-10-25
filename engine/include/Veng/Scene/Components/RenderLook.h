#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Renderer/Tonemapper.h>

namespace Veng
{
    class MaterialInstance;
    class Texture;

    /// @brief A scene-authored fullscreen post-process effect the renderer runs over scene color.
    ///
    /// Resolved by the renderer via View<PostProcessEffect> each Execute — the lights model: every
    /// present, Enabled component with a loaded PostProcess-domain Material contributes one
    /// fullscreen pass to the HDR tail, run in Order (ascending) after the scene is composited and
    /// before bloom, so the effect is bloomed and tonemapped with the scene. Each pass receives the
    /// finished scene color and the scene depth as inputs, so an effect may distort, tint, or gate
    /// on depth. The material owns its own parameters (a consumer writes them on the
    /// MaterialInstance each frame); the engine owns the pass, its anchor, and the color/depth
    /// wiring. Adding or removing an enabled effect (or reordering, or toggling Enabled) recompiles
    /// the pass set at the frame boundary; a stable set replays. Reflection-camera captures do not
    /// run these effects (a probe wants no screen effect baked into it).
    ///
    /// An effect whose material carries per-frame parameters must use a per-viewport
    /// MaterialInstance: the pass writes per-viewport state (the runtime-bound source handles) into
    /// the instance's single ring slot each frame, so two viewports sharing one instance would
    /// clobber each other.
    struct PostProcessEffect
    {
        /// @brief The PostProcess-domain material run fullscreen; an empty or unloaded handle is a no-op.
        AssetHandle<MaterialInstance> Material;
        /// @brief Sort key: effects run in ascending Order (ties keep scene iteration order).
        i32 Order = 0;
        /// @brief Whether this effect runs; a disabled effect is dropped from the frame.
        bool Enabled = true;
    };

    /// @brief A scene's render look: the view-wide post and pipeline knobs it is presented with.
    ///
    /// One per scene: every viewport presenting the scene reads it each frame
    /// (Renderer::Viewport) and resolves it — on change only — into the topology it configures
    /// (Bloom / Shadows / AO / SSR / …) and the per-frame knobs it writes over each pushed view
    /// (Exposure, BloomThreshold, BloomFireflyCeiling, BloomIntensity, AmbientFloor, …), so a system writing a field here
    /// changes the next frame's render, and a prefab carrying one brings its look into the scene.
    /// With several, the first in pool order wins and a presenting viewport warns once. A level's authored
    /// "render" block seeds one onto its settings entity (SeedLevel). A reflected,
    /// tolerantly-serialized struct, not a renderer type, so a new field does not invalidate
    /// existing level or prefab blobs. The sky/environment knobs are not here: they are the
    /// author-opt-in Sky and TimeOfDay scene components, resolved by the renderer itself.
    struct RenderLook
    {
        /// @brief Tonemap exposure fed into the per-frame SceneView.
        f32 Exposure = 1.0f;
        /// @brief The tone curve the terminal tonemap pass maps the exposed HDR through, fed into
        ///        the per-frame SceneView. Serialized by name (in a level's "render" block too).
        Renderer::Tonemapper Tonemapper = Renderer::Tonemapper::ACES;
        /// @brief Whether auto-exposure adapts the tonemap exposure to the scene's luminance.
        ///
        /// When set, Exposure becomes a manual bias on the adapted result instead of the absolute
        /// exposure. Off matches the renderer's own default.
        bool AutoExposure = false;
        /// @brief Auto-exposure lower clamp on the adapted average luminance.
        ///
        /// Bounds how bright the metering lets a dark scene drive the exposure; a small value suits a
        /// scene whose dimmest lit content is far below mid-grey. Together with the upper clamp it is
        /// the range the adapted luminance moves in; what is metered at all is the floor's
        /// (AutoExposureMeterFloor). The default matches the renderer's own ViewState default, so a
        /// look leaving it unset is unchanged.
        f32 AutoExposureMinLuminance = 0.002f;
        /// @brief Auto-exposure upper clamp on the adapted average luminance.
        ///
        /// Bounds how dark the metering lets a bright scene drive the exposure, so a subject brighter
        /// than it (a star filling the view) is shown bright rather than metered down to mid-grey or
        /// to the highlight limit; a large value lets the exposure follow any scene down. It clamps
        /// the adaptation only — the meter reads every luminance the scene colour can hold. The
        /// default matches the renderer's own ViewState default, so a look leaving it unset is
        /// unchanged.
        f32 AutoExposureMaxLuminance = 8.0f;
        /// @brief Auto-exposure metering floor: a pixel darker than this is not metered.
        ///
        /// Separate from AutoExposureMinLuminance, so a dim field can be metered and shown dim under
        /// that clamp instead of being dropped. The default matches the renderer's own ViewState
        /// default. @see Renderer::SceneView::AutoExposureMeterFloor
        f32 AutoExposureMeterFloor = 0.002f;
        /// @brief Lower percentile of the lit-pixel histogram the metering averages from, in [0, 1].
        ///
        /// Discards the darkest tail before averaging (0 keeps it). The default matches the
        /// renderer's own ViewState default.
        f32 AutoExposureLowPercentile = 0.0f;
        /// @brief Upper percentile of the lit-pixel histogram the metering averages to, in [0, 1].
        ///
        /// Discards the brightest tail before averaging (1 keeps it). The default matches the
        /// renderer's own ViewState default.
        f32 AutoExposureHighPercentile = 1.0f;
        /// @brief The percentile of the metered pixels the highlight cap keeps from blowing out.
        /// @see Renderer::SceneView::AutoExposureHighlightPercentile
        f32 AutoExposureHighlightPercentile = 0.99f;
        /// @brief The exposed luminance the highlight percentile may reach; zero (the default)
        ///        turns the cap off.
        /// @see Renderer::SceneView::AutoExposureHighlightLuminance
        f32 AutoExposureHighlightLuminance = 0.0f;
        /// @brief Whether the bloom battery is enabled.
        bool Bloom = true;
        /// @brief Bloom bright-pass luminance knee fed into the per-frame SceneView.
        f32 BloomThreshold = 1.0f;
        /// @brief The most luminance one bloom bright-pass tap carries, display-referred, fed into
        ///        the per-frame SceneView.
        ///
        /// Caps what a single extreme texel seeds into the bloom while leaving ordinary bright
        /// content averaged linearly; raise it for a scene whose legitimate small sources sit near
        /// it. @see Renderer::SceneView::BloomFireflyCeiling
        f32 BloomFireflyCeiling = 64.0f;
        /// @brief Bloom composite intensity fed into the per-frame SceneView.
        f32 BloomIntensity = 1.0f;
        /// @brief Bloom upsample spread fed into the per-frame SceneView.
        f32 BloomRadius = 1.0f;
        /// @brief The lens-dirt mask the bloom lights, fed into the per-frame SceneView.
        ///
        /// A greyscale screen-space texture (its red channel is read) of the smudges and dust a
        /// bright source reveals on a dirty lens. It is fitted over the presented frame preserving
        /// its aspect, so any texture shape works. Empty draws no dirt.
        /// @see Renderer::SceneView::LensDirt
        AssetHandle<Texture> LensDirt;
        /// @brief How strongly the bloom lights the lens dirt, fed into the per-frame SceneView.
        ///
        /// The default of zero draws no dirt and costs nothing, whatever LensDirt names.
        /// @see Renderer::SceneView::LensDirtIntensity
        f32 LensDirtIntensity = 0.0f;
        /// @brief How strongly the anamorphic streak adds to the scene, fed into the per-frame
        ///        SceneView.
        ///
        /// Bright sources stretched into thin horizontal lines in their own colour. The default of
        /// zero draws no streak and costs nothing.
        /// @see Renderer::SceneView::StreakIntensity
        f32 StreakIntensity = 0.0f;
        /// @brief How far the anamorphic streak reaches, as a share of the frame's width, fed into
        ///        the per-frame SceneView.
        /// @see Renderer::SceneView::StreakLength
        f32 StreakLength = 0.25f;
        /// @brief How differently the anamorphic streak's colour channels spread, fed into the
        ///        per-frame SceneView.
        /// @see Renderer::SceneView::StreakChromaticSpread
        f32 StreakChromaticSpread = 0.0f;
        /// @brief Whether the directional cascaded-shadow battery is enabled.
        bool Shadows = true;
        /// @brief Whether the punctual shadow atlas is enabled.
        ///
        /// Drives the point/spot shadow maps and the soft (PCSS) shadows cast by Sphere/Rect/Polygon
        /// area lights — independent of the directional Shadows toggle above.
        bool PunctualShadows = true;
        /// @brief Far distance (world units) the directional cascades are fit and rendered out to.
        ///
        /// Caps the shadowed range: cascades pack their resolution into this distance from the
        /// camera, and geometry past it is unshadowed. A large world needs a large value so distant
        /// casters still shadow; the default matches the renderer's own MaxShadowDistance default.
        f32 MaxShadowDistance = 100.0f;
        /// @brief How the directional cascades split the shadowed range: 0 uniform, 1 logarithmic.
        ///
        /// Belongs beside MaxShadowDistance: a long range keeps its near cascades as tight as a short
        /// one's only by leaning further toward the logarithmic split. The default matches the
        /// renderer's own CascadeSplitLambda default.
        f32 CascadeSplitLambda = 0.85f;
        /// @brief Directional cascade shadow-map resolution: the per-cascade tile edge, in texels.
        ///
        /// A larger value sharpens the cascades at a memory/bandwidth cost. Clamped to the device
        /// maximum by the renderer; the default matches the renderer's own ShadowResolution default.
        u32 ShadowResolution = 1024;
        /// @brief The scene's authored flat-fallback ambient, multiplied by surface occlusion.
        ///
        /// The ambient a surface receives in a scene with no lit sky (no environment, no SH/IBL sky
        /// tier). Mapped into the per-frame view and pushed into the lighting pass; the default is
        /// the engine's flat ambient, so a look leaving it unset renders unchanged.
        vec3 AmbientFloor{0.12f, 0.13f, 0.16f};
        /// @brief A linear RGB colour multiplying the light the sky gives the scene: the
        ///        image-based and SH skylight ambient arms, and the reflections' fallback to them.
        ///
        /// White leaves the sky's light as it is. The skybox, the direct lights and the flat
        /// ambient floor are not tinted. Mapped into the per-frame view each frame, so it moves with
        /// no rebuild.
        vec3 EnvironmentLightTint{1.0f};
        /// @brief Whether the SSAO battery is enabled.
        bool AO = true;
        /// @brief Whether screen-space reflections run.
        ///
        /// Off matches the renderer's own default.
        bool SSR = false;
        /// @brief Screen-space reflection rays traced per 2×2 quad of glossy pixels (1, 2 or 4), fed
        ///        into the per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrSamplesPerQuad
        u32 SsrSamplesPerQuad = 1;
        /// @brief The GGX width alpha below which a pixel traces a screen-space reflection, fed into
        ///        the per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrRoughnessThreshold
        f32 SsrRoughnessThreshold = 0.2f;
        /// @brief The reflection variance above which a skipped glossy pixel traces, fed into the
        ///        per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrVarianceThreshold
        f32 SsrVarianceThreshold = 0.0f;
        /// @brief Whether a noisy reflection history makes a skipped glossy pixel trace, fed into the
        ///        per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrVarianceGuidedTracing
        bool SsrVarianceGuidedTracing = true;
        /// @brief How far behind the depth buffer a screen-space reflection hit still counts, fed
        ///        into the per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrDepthBufferThickness
        f32 SsrDepthBufferThickness = 0.015f;
        /// @brief The iterations a screen-space reflection ray's march takes at most, fed into the
        ///        per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrMaxTraversalIntersections
        u32 SsrMaxTraversalIntersections = 128;
        /// @brief The active subgroup lanes below which a glossy reflection ray's march stops, fed
        ///        into the per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrMinTraversalOccupancy
        u32 SsrMinTraversalOccupancy = 4;
        /// @brief How strongly the reflection denoiser's temporal resolve holds its history, fed
        ///        into the per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SsrTemporalStabilityFactor
        f32 SsrTemporalStabilityFactor = 0.7f;
        /// @brief Geometric specular anti-aliasing's screen-space variance scale, fed into the
        ///        per-frame SceneView; zero turns it off.
        ///
        /// How strongly a glossy surface's roughness widens with the variation of its normal across
        /// a pixel. The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SpecularAntiAliasingVariance
        f32 SpecularAntiAliasingVariance = 0.15f;
        /// @brief Geometric specular anti-aliasing's ceiling on the alpha² it adds, fed into the
        ///        per-frame SceneView.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::SpecularAntiAliasingThreshold
        f32 SpecularAntiAliasingThreshold = 0.2f;
        /// @brief Normal-map specular anti-aliasing's scale on the variance a normal map's mips
        ///        average away, fed into the per-frame SceneView; zero turns it off.
        ///
        /// The default matches the renderer's own ViewState default.
        /// @see Renderer::SceneView::NormalMapAntiAliasingScale
        f32 NormalMapAntiAliasingScale = 1.0f;
        /// @brief Whether the pre-translucent scene-color copy is populated each frame.
        ///
        /// Enables the grab a Translucent-domain material samples the scene behind its fragment
        /// through (SampleSceneColor); without it those samples read black. Off matches the
        /// renderer's own default — a scene whose translucents refract or distort the scene
        /// authors it on.
        bool Refraction = false;
        /// @brief Whether the scene-color copy carries a mip chain a material can read blurred.
        ///
        /// Requires Refraction. Enables SampleSceneColorBlurred — frosted glass, a backdrop blur —
        /// by giving the grab a halving chain down to roughly an 8-pixel level. Off matches the
        /// renderer's own default; a scene whose translucents only *distort* their samples wants it
        /// off, since the levels are generated whether or not anything reads them.
        bool RefractionBlur = false;
        /// @brief Whether the depth-of-field battery runs.
        ///
        /// Off matches the renderer's own default.
        bool DepthOfField = false;
        /// @brief Depth-of-field focus plane distance in metres.
        ///
        /// Not consulted while the resolved camera is Physical — that camera's lens authors the
        /// focus distance, and this value is stored but inert until it stops being Physical. The
        /// default matches the renderer's own ViewState default.
        f32 DofFocusDistance = 10.0f;
        /// @brief Depth-of-field aperture diameter in metres.
        ///
        /// Not consulted while the resolved camera is Physical, exactly as DofFocusDistance is not.
        /// The default matches the renderer's own ViewState default (50mm f/2.8).
        f32 DofAperture = 0.0179f;
        /// @brief Depth-of-field blur radius ceiling in half-resolution pixels.
        ///
        /// A quality knob rather than a lens value, so it applies in every camera mode. Clamped to
        /// DofCocCeiling on the way in — a cooked level or prefab is untrusted input.
        f32 DofMaxCoc = 16.0f;
        /// @brief Depth-of-field gather ring count.
        ///
        /// A quality knob rather than a lens value, so it applies in every camera mode. Clamped to
        /// MaxDofRings on the way in: it is a GPU loop bound, and an unclamped authored value
        /// reaching the shader is a device hang rather than a recoverable error.
        u32 DofRingCount = 4;

        /// @brief Whether two looks are identical field for field.
        friend bool operator==(const RenderLook&, const RenderLook&) = default;
    };
}

VE_REFLECT(::Veng::PostProcessEffect, 0xE760A6F6C3F08F48ULL)
VE_FIELD(Material, .DisplayName = "Material")
VE_FIELD(Order, .DisplayName = "Order", .Tooltip = "Effects run in ascending order")
VE_FIELD(Enabled, .DisplayName = "Enabled")
VE_REFLECT_END();

VE_REFLECT(::Veng::RenderLook, 0x28E4618C66455E21ULL)
VE_FIELD(Exposure, .DisplayName = "Exposure", .Display = {.Min = 0.0})
VE_FIELD(Tonemapper, .DisplayName = "Tonemapper")
VE_FIELD(AutoExposure, .DisplayName = "Auto Exposure")
VE_FIELD(AutoExposureMinLuminance, .DisplayName = "Auto Exposure Min Luminance",
         .Display = {.Min = 0.0})
VE_FIELD(AutoExposureMaxLuminance, .DisplayName = "Auto Exposure Max Luminance",
         .Display = {.Min = 0.0})
VE_FIELD(AutoExposureMeterFloor, .DisplayName = "Auto Exposure Meter Floor",
         .Display = {.Min = 0.0})
VE_FIELD(AutoExposureLowPercentile, .DisplayName = "Auto Exposure Low Percentile",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(AutoExposureHighPercentile, .DisplayName = "Auto Exposure High Percentile",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(AutoExposureHighlightPercentile, .DisplayName = "Auto Exposure Highlight Percentile",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(AutoExposureHighlightLuminance, .DisplayName = "Auto Exposure Highlight Luminance",
         .Display = {.Min = 0.0})
VE_FIELD(Bloom, .DisplayName = "Bloom")
VE_FIELD(BloomThreshold, .DisplayName = "Bloom Threshold", .Display = {.Min = 0.0})
VE_FIELD(BloomFireflyCeiling, .DisplayName = "Bloom Firefly Ceiling", .Display = {.Min = 0.0})
VE_FIELD(BloomIntensity, .DisplayName = "Bloom Intensity", .Display = {.Min = 0.0})
VE_FIELD(BloomRadius, .DisplayName = "Bloom Radius", .Display = {.Min = 0.0})
VE_FIELD(LensDirt, .DisplayName = "Lens Dirt",
         .Tooltip = "Greyscale mask the bloom lights, fitted over the frame")
VE_FIELD(LensDirtIntensity, .DisplayName = "Lens Dirt Intensity", .Display = {.Min = 0.0})
VE_FIELD(StreakIntensity, .DisplayName = "Streak Intensity",
         .Tooltip = "Anamorphic streak: bright sources stretched into horizontal lines",
         .Display = {.Min = 0.0})
VE_FIELD(StreakLength, .DisplayName = "Streak Length",
         .Tooltip = "Share of the frame's width the streak falls to 1/e over",
         .Display = {.Min = 0.01, .Max = 1.0})
VE_FIELD(StreakChromaticSpread, .DisplayName = "Streak Chromatic Spread",
         .Tooltip = "Positive shortens red and blue against green: violet near the source",
         .Display = {.Min = -0.9, .Max = 0.9})
VE_FIELD(Shadows, .DisplayName = "Shadows")
VE_FIELD(PunctualShadows, .DisplayName = "Punctual / Area Shadows")
VE_FIELD(MaxShadowDistance, .DisplayName = "Max Shadow Distance", .Display = {.Min = 0.0})
VE_FIELD(CascadeSplitLambda, .DisplayName = "Cascade Split Lambda",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(ShadowResolution, .DisplayName = "Shadow Resolution", .Display = {.Min = 1})
VE_FIELD(AmbientFloor, .DisplayName = "Ambient Floor")
VE_FIELD(EnvironmentLightTint, .DisplayName = "Environment Light Tint")
VE_FIELD(AO, .DisplayName = "SSAO")
VE_FIELD(SSR, .DisplayName = "Screen-Space Reflections")
VE_FIELD(SsrSamplesPerQuad, .DisplayName = "SSR Samples Per Quad", .Display = {.Min = 1, .Max = 4})
VE_FIELD(SsrRoughnessThreshold, .DisplayName = "SSR Roughness Threshold",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 1.0})
VE_FIELD(SsrVarianceThreshold, .DisplayName = "SSR Variance Threshold",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 0.01, .Precision = 4})
VE_FIELD(SsrVarianceGuidedTracing, .DisplayName = "SSR Variance-Guided Tracing")
VE_FIELD(SsrDepthBufferThickness, .DisplayName = "SSR Depth Buffer Thickness",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 0.03, .Precision = 4})
VE_FIELD(SsrMaxTraversalIntersections, .DisplayName = "SSR Max Traversal Iterations",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0, .Max = 256})
VE_FIELD(SsrMinTraversalOccupancy, .DisplayName = "SSR Min Traversal Occupancy",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0, .Max = 32})
VE_FIELD(SsrTemporalStabilityFactor, .DisplayName = "SSR Temporal Stability",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 1.0})
VE_FIELD(SpecularAntiAliasingVariance, .DisplayName = "Specular AA Variance",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 1.0})
VE_FIELD(SpecularAntiAliasingThreshold, .DisplayName = "Specular AA Threshold",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 1.0})
VE_FIELD(NormalMapAntiAliasingScale, .DisplayName = "Normal-Map AA Scale",
         .Display = {.Widget = ::Veng::WidgetKind::Slider, .Min = 0.0, .Max = 2.0})
VE_FIELD(Refraction, .DisplayName = "Refraction (Scene-Color Grab)")
VE_FIELD(RefractionBlur, .DisplayName = "Refraction Blur (Grab Mip Chain)")
VE_FIELD(DepthOfField, .DisplayName = "Depth of Field")
VE_FIELD(DofFocusDistance, .DisplayName = "DoF Focus Distance", .Display = {.Min = 0.0})
VE_FIELD(DofAperture, .DisplayName = "DoF Aperture", .Display = {.Min = 0.0})
VE_FIELD(DofMaxCoc, .DisplayName = "DoF Max Blur Radius",
         .Display = {.Min = 0.0, .Max = static_cast<f64>(::Veng::Renderer::DofCocCeiling)})
VE_FIELD(DofRingCount, .DisplayName = "DoF Ring Count",
         .Display = {.Min = 1, .Max = static_cast<f64>(::Veng::Renderer::MaxDofRings)})
VE_REFLECT_END();
