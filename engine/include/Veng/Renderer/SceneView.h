#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/Atmosphere.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Renderer/ShadowCascades.h>
#include <Veng/Renderer/Tonemapper.h>
#include <Veng/Renderer/Types.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Visibility.h>

#include <array>
#include <span>

/// @brief The SceneRenderer per-frame input and construction parameters.
///
/// Split out of SceneRenderer.h so a consumer that assembles a SceneView (or a
/// SceneRendererInfo) need not pull in the renderer class. SceneRenderer.h re-includes
/// this header, so no consumer migrates.
namespace Veng
{
    class Scene;
    class AssetManager;
    class MaterialInstance;
    class EnvironmentMap;
    class SceneBroadphase;
}

namespace Veng::Renderer
{
    class Context;
    class DescriptorSet;

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
}
