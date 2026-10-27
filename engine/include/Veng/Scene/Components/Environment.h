#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Variant.h>
#include <Veng/Renderer/Atmosphere.h>
#include <Veng/Renderer/SunPosition.h>

namespace Veng
{
    class MaterialInstance;
    class EnvironmentMap;

    namespace Renderer
    {
        class BakedSkyCube;
    }

    /// @brief Sky source: an environment map drives the background and image-based lighting.
    ///
    /// One alternative of the SkySource variant. The map's radiance cube is the visible sky, and
    /// its convolved maps feed image-based lighting when the Sky component's lighting tier requests
    /// it. An empty Map is a no-op.
    struct EnvironmentSky
    {
        /// @brief The environment map (radiance/irradiance/prefiltered/BRDF) for the sky and IBL.
        AssetHandle<EnvironmentMap> Map;
    };

    /// @brief How a procedural or authored sky reaches the screen and whether it can light the scene.
    ///
    /// Keyed to how the sky changes over time. Direct evaluates the sky per pixel every frame —
    /// right for a sky that animates continuously — and cannot light the scene (a direct sky
    /// displays only). Baked renders the sky into the six faces of a radiance cubemap once per
    /// change and samples that cube per frame through the skybox path — right for the common
    /// static-between-changes case — and is the mode that can light the scene. Integer values are
    /// stable — persisted in prefabs.
    enum class SkyMode : u8
    {
        /// @brief Evaluate the sky per pixel every frame; display-only.
        Direct = 0,
        /// @brief Bake the sky into a radiance cube on change and sample it; can light the scene.
        Baked = 1,
    };

    /// @brief Sky source: a procedural physically-based atmosphere fills the sky.
    ///
    /// One alternative of the SkySource variant. The atmosphere renders the background sky along
    /// each view ray; the toward-sun direction is the inverse of the scene's first directional
    /// Light's travel direction (itself derivable from a TimeOfDay component), so the sky and the
    /// direct lighting share one sun. Mode selects the direct per-pixel path (right for a
    /// continuously-moving sun) or the baked-cube path (bake on the sun/params dirty signal, sample
    /// a cube per frame); the two render the same sky, and only the baked path can light the scene.
    struct AtmosphereSky
    {
        /// @brief Physically-based atmosphere parameters; the renderer regenerates its LUTs on change.
        ///
        /// The defaults describe Earth at sea level.
        Renderer::Atmosphere Params;
        /// @brief Whether the atmosphere is composited per pixel (Direct) or baked to a cube (Baked).
        SkyMode Mode = SkyMode::Baked;
    };

    /// @brief Sky source: an authored Sky-domain material fills the background sky.
    ///
    /// One alternative of the SkySource variant. The material owns its own parameters and any
    /// buffers/textures it reads; the engine supplies the view ray and the g-buffer depth mask.
    /// An empty Material is a no-op. Mode selects the direct per-pixel path or the baked-cube path;
    /// the two render the same sky, and the author picks per the sky's dynamics.
    struct MaterialSky
    {
        /// @brief The Sky-domain material instance rendered as the background sky.
        AssetHandle<MaterialInstance> Material;
        /// @brief Whether the material is composited per pixel (Direct) or baked to a cube (Baked).
        SkyMode Mode = SkyMode::Baked;
    };

    /// @brief Sky source: a caller-owned, already-baked radiance cube fills the sky.
    ///
    /// One alternative of the SkySource variant. Where a MaterialSky in SkyMode::Baked has the
    /// renderer bake the cube per SceneRenderer, a CubeSky points the renderer at a Renderer::BakedSkyCube
    /// the caller owns and bakes itself: the skybox samples it and the lighting tiers derive from it,
    /// but the renderer bakes nothing. It is the shared-sky path — one baked cube several renderers
    /// (a main viewport and its capture probes) and several worlds (two flight regimes of a system)
    /// all sample, so a world swap costs no re-bake. A null Cube is a no-op. Runtime-only: the Ref is
    /// never reflected, cooked, or serialized (no VE_FIELD), so a CubeSky is only ever set at runtime.
    struct CubeSky
    {
        /// @brief The caller-owned baked radiance cube the sky samples; runtime-only, never serialized.
        Ref<Renderer::BakedSkyCube> Cube;
    };

    /// @brief The active source of a Sky component: environment map, atmosphere, material, or baked cube.
    ///
    /// The active alternative selects the sky kind and carries that kind's parameters. Empty means
    /// no sky (the flat-ambient fallback). The renderer resolves the active alternative per frame
    /// and recompiles its own pass set when the source kind changes.
    using SkySource = Variant<EnvironmentSky, AtmosphereSky, MaterialSky, CubeSky>;

    /// @brief How the sky lights the scene, beyond displaying it.
    ///
    /// The lighting tier of a Sky component: None displays the sky without lighting the scene, SH
    /// projects it to a spherical-harmonic ambient (the cheap diffuse arm), and IBL runs the full
    /// split-sum image-based lighting. A tier is a request — the renderer activates it per source:
    /// a cube-backed source (an environment map, or a material sky in SkyMode::Baked) drives both
    /// SH and IBL; a per-pixel source (a direct material sky) cannot light and degrades either to
    /// background-only (bake to light). An active scene-lighting capture
    /// (Renderer::CaptureOutput::SceneLighting) stands in for the source: it makes the effective
    /// tier IBL whatever is authored here, resuming this tier when it goes inactive.
    ///
    /// Cost is explicit and authored. None is free; SH pays a one-time projection of the sky's
    /// radiance cube on the sky's dirty signal (the cheap tier — a faint, low-frequency glow); IBL
    /// pays a full split-sum prefilter on that same signal (the expensive tier — justified for a
    /// bright, directional sky). A near-black sky should stay None. The engine never rate-limits:
    /// every committed dirty signal pays its tier's cost, so a live edit of a lit sky's params pays
    /// a full re-convolution per commit — bounding that frequency (debouncing before commit) is the
    /// consumer's responsibility.
    enum class SkyLighting : u8
    {
        /// @brief Display the sky only; the scene keeps its flat ambient fallback.
        None,
        /// @brief Light the diffuse term from a spherical-harmonic projection of the sky.
        SH,
        /// @brief Light the scene with full split-sum image-based lighting from the sky.
        IBL,
    };

    /// @brief The scene's one authored sky: a source, an intensity, and a lighting tier.
    ///
    /// One per scene, resolved by the renderer via TryGetFirst<Sky> each Execute — the lights
    /// model. Source selects the sky kind (environment map / atmosphere / material) and carries its
    /// parameters; an empty Source is no sky (the flat fallback). Intensity scales the background
    /// and any ambient radiance the tier casts. Lighting requests how the sky lights the scene; an
    /// active scene-lighting capture (Renderer::CaptureOutput::SceneLighting) overrides both the
    /// lighting input and the tier while it lights the scene. If several Sky components
    /// exist the first walked wins and a warning logs once. No transform is read — the sky is
    /// scene-global; its orientation is Rotation.
    struct Sky
    {
        /// @brief The active sky source, or empty for no sky.
        SkySource Source;
        /// @brief Scales the sky's background and ambient radiance.
        f32 Intensity = 1.0f;
        /// @brief How the sky lights the scene, beyond displaying it.
        SkyLighting Lighting = SkyLighting::None;
        /// @brief The sky's orientation in the scene: a feature the source holds at direction d is
        ///        displayed at world direction Rotation * d.
        ///
        /// For an application whose rendered frame turns relative to the backdrop — a frame that
        /// rotates with whatever carries the camera — so the sky turns rather than staying fixed to
        /// the frame. It turns the displayed sky (direct and baked alike) and the
        /// image-based and SH lighting the sky casts, so reflections turn with what is shown. It
        /// turns lookups rather than content: changing it re-bakes no cube and re-convolves no
        /// lighting, so it may change every frame. A scene-lighting capture's lighting is rendered
        /// in the scene's own frame and is not turned. An AtmosphereSky ignores it: an atmosphere
        /// is defined by the frame's up and the scene's sun, which do not turn with a backdrop.
        /// Identity by default; a non-unit value is normalised.
        quat Rotation = quat(1.0f, 0.0f, 0.0f, 0.0f);
    };

    /// @brief Where the scene's coordinate origin stands in the frame its content rests in.
    ///
    /// A consumer that re-centres its world on the viewer each frame moves scene space with the
    /// viewer, so a renderer that snaps to scene space snaps to a grid sliding under content at
    /// rest. This component tells the renderer where the origin stands: a point at scene
    /// coordinates p lies at p + Position in the stable frame, a translation of scene space and
    /// never a rotation. The directional shadow cascades snap their texel grid in that frame, so
    /// content at rest keeps its sub-texel place in them while the origin moves.
    ///
    /// Write it in the same pass that moves the origin, from the same value — the pass that
    /// re-bases the entities' Transforms and calls OffsetRibbons — so the scene's placements and
    /// the offset agree on every frame. Keep Position modest, or re-base it rarely: its magnitude
    /// multiplies any frame-to-frame wobble in a light's direction into a move of that light's
    /// grid.
    ///
    /// Geometry the viewer carries rests in scene space, not in the stable frame.
    /// ViewerFrameReach is the view depth out to which such geometry extends; every cascade whose
    /// slice begins nearer than it snaps in scene space instead, riding the viewer. The trade:
    /// content resting in one frame shifts sub-texel against a grid snapped in the other while the
    /// two frames move apart. With a reach set, the carried geometry and the distant scene both
    /// hold still, and scene content close enough to fall in the reach's cascades shifts slightly
    /// while the viewer translates; without one, the carried geometry shifts instead. The cascade
    /// blend band cross-fades between the two kinds of cascade with no seam.
    ///
    /// One per scene, resolved by the renderer via TryGetFirst each Execute; with several the first
    /// walked wins and a warning logs once. No transform is read. A scene without one is a
    /// fixed-origin scene, snapped in its own frame. Runtime-only: it carries no reflected field,
    /// so it never serializes and never crosses the wire.
    struct FloatingOrigin
    {
        /// @brief The scene origin's place in the stable frame, in world units.
        dvec3 Position{0.0};
        /// @brief View depth out to which the viewer carries geometry of its own; 0 for none.
        f32 ViewerFrameReach = 0.0f;
    };

    /// @brief Time-of-day sun drive: the author's opt-in to derive the sun from a clock time.
    ///
    /// An author adds this component to a scene entity to position the sun by time of day
    /// instead of authoring a direction: TimeOfDaySystem derives the toward-sun direction from
    /// Hours, DayOfYear, and the orbital parameters and writes the scene's first directional
    /// Light's travel direction from it each tick — so direct lighting, shadows, and the sky all
    /// track the one derived sun. While present, the light's authored Direction is overwritten;
    /// a game animates the cycle by advancing Hours.
    struct TimeOfDay
    {
        /// @brief Time of day in solar hours; 12 is solar noon. Wraps over 24.
        f32 Hours = 12.0f;
        /// @brief Day of the year, measured from the northern spring equinox (days).
        f32 DayOfYear = 0.0f;
        /// @brief Orbital and site parameters the sun path is derived from; Earth defaults.
        Renderer::SunOrbit Orbit;
    };
}

VE_REFLECT(::Veng::EnvironmentSky, 0x51902800E072B6E9ULL)
VE_FIELD(Map, .DisplayName = "Map")
VE_REFLECT_END();

VE_ENUM(::Veng::SkyMode, 0x9C2A0D5E7B1F4488ULL)
VE_ENUMERATOR(Direct)
VE_ENUMERATOR(Baked)
VE_ENUM_END();

VE_REFLECT(::Veng::AtmosphereSky, 0x2091BE830E0AA76DULL)
VE_FIELD(Params, .DisplayName = "Parameters")
VE_FIELD(Mode, .DisplayName = "Mode")
VE_REFLECT_END();

VE_REFLECT(::Veng::MaterialSky, 0x278971B85ADDA928ULL)
VE_FIELD(Material, .DisplayName = "Material")
VE_FIELD(Mode, .DisplayName = "Mode")
VE_REFLECT_END();

// CubeSky carries only a runtime-only Ref (no VE_FIELD): reflected so the SkySource variant can
// enumerate it, but it is never serialized — a CubeSky is set at runtime, never authored.
VE_REFLECT(::Veng::CubeSky, 0x363CB1B30EC3016FULL)
VE_REFLECT_END();

VE_ENUM(::Veng::SkyLighting, 0xB2C6211BC808C7BDULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(SH)
VE_ENUMERATOR(IBL)
VE_ENUM_END();

VE_VARIANT(::Veng::SkySource, 0x3638DADE35D35C41ULL);

// Registered without a reflected field, so it neither serializes nor rides the wire: the origin's
// place is per-frame state its owner derives, meaningless to anyone else.
VE_TYPE(::Veng::FloatingOrigin, 0xFF863E05FE73F806ULL);

VE_REFLECT(::Veng::Sky, 0xC7D64305B199222CULL)
VE_FIELD(Source, .DisplayName = "Source")
VE_FIELD(Intensity, .DisplayName = "Intensity", .Display = {.Min = 0.0})
VE_FIELD(Lighting, .DisplayName = "Lighting")
VE_FIELD(Rotation, .DisplayName = "Rotation",
         .Tooltip = "Orientation of the sky in the scene; turns the display and its lighting")
VE_REFLECT_END();

VE_REFLECT(::Veng::TimeOfDay, 0x3096812B98FEC8D2ULL)
VE_FIELD(Hours, .DisplayName = "Hours", .Tooltip = "Solar hours; 12 is noon",
         .Display = {.Min = 0.0, .Max = 24.0})
VE_FIELD(DayOfYear, .DisplayName = "Day of year",
         .Tooltip = "Days since the northern spring equinox", .Display = {.Min = 0.0})
VE_FIELD(Orbit, .DisplayName = "Orbit")
VE_REFLECT_END();
