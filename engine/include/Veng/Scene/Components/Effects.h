#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/RenderLayer.h>

namespace Veng
{
    class Flipbook;
    class ParticleEffect;

    /// @brief How a FlipbookSprite composites into the scene.
    enum class SpriteBlend : u8
    {
        /// @brief Takes the flipbook's own recommended compositing (FlipbookBlend).
        Asset = 0,
        /// @brief Composited over the scene by coverage, sorted back to front.
        Alpha = 1,
        /// @brief Added onto the scene, in no particular order.
        Additive = 2,
    };

    /// @brief How a FlipbookSprite moves from one frame to the next.
    enum class SpriteFrameBlend : u8
    {
        /// @brief Takes the flipbook's own recommendation (FlipbookFrameBlend).
        Asset = 0,
        /// @brief Shows one frame at a time.
        Step = 1,
        /// @brief Fades from each frame into the next.
        Crossfade = 2,
        /// @brief Warps both frames toward each other along the flipbook's motion vectors, then
        ///        fades; a crossfade on a flipbook with no motion sheet.
        Motion = 3,
    };

    /// @brief Plays a flipbook as a camera-facing sprite in the world.
    ///
    /// Drawn by the renderer's sprite pass in the translucent layer — after the opaque scene and
    /// its translucent surfaces, before the bloom and tonemap tail — as an HDR quad facing the
    /// camera, centred on the entity so the flipbook's anchor sits on its world position. The
    /// pass depth-tests against the opaque scene without writing depth, sorts Alpha sprites back
    /// to front and leaves Additive ones unsorted. FlipbookSystem advances Time; a one-shot sprite
    /// sets Finished once its last frame has played. The component is presentation, never
    /// replicated.
    ///
    /// An unlit flipbook draws its colour sheet tinted and writes the bloom mask by luminance, so a
    /// bright sprite glows. A six-way flipbook is lit by the view's directional, point and spot
    /// lights, gathered once per sprite and shadowed by the opaque scene, plus the scene's ambient;
    /// Tint colours that lit term. Its temperature glows through an emission ramp — the
    /// flipbook's own unless EmissionRamp or Ignition overrides it — scaled by EmissionIntensity,
    /// and only the emission writes the bloom mask, so a hot core glows and sunlit smoke does not.
    /// FrameBlend blends between frames without any temporal accumulation, and SoftDistance fades
    /// the sprite where it meets opaque geometry. By default a flipbook takes its own recommended
    /// blend (stepped for a schema-1 source) and only a six-way one fades softly, so an unlit
    /// stepped flipbook draws as a plain tinted quad.
    struct FlipbookSprite
    {
        /// @brief The flipbook played; a sprite whose flipbook is not resident draws nothing and
        ///        does not advance.
        AssetHandle<Flipbook> Flipbook;
        /// @brief The quad's width in world units at unit scale; 0 takes the flipbook's world
        ///        extent (or 1 when it records none). The entity's world scale multiplies it.
        f32 Size = 1.0f;
        /// @brief Multiplies the sampled colour, in linear HDR — above 1 drives bloom.
        vec3 Tint{1.0f};
        /// @brief Multiplies the sprite's coverage (and, for an additive sprite, its brightness).
        f32 Opacity = 1.0f;
        /// @brief Multiplies the flipbook's authored frame rate.
        f32 PlaybackRate = 1.0f;
        /// @brief When positive, plays the whole sequence over this many seconds instead, and
        ///        PlaybackRate is ignored — a long bake can serve a short burst.
        f32 DurationOverride = 0.0f;
        /// @brief Seconds of playback so far; advanced by FlipbookSystem. Authoring it starts the
        ///        sprite part-way through.
        f32 Time = 0.0f;
        /// @brief The compositing; Asset takes the flipbook's recommendation.
        SpriteBlend Blend = SpriteBlend::Asset;
        /// @brief Roll in the screen plane, in radians, counter-clockwise.
        f32 Rotation = 0.0f;
        /// @brief How the sprite moves between frames; Asset takes the flipbook's recommendation.
        SpriteFrameBlend FrameBlend = SpriteFrameBlend::Asset;
        /// @brief The depth over which the sprite fades into opaque geometry behind it: below 0 the
        ///        flipbook's default (half the quad's width for a six-way flipbook, off otherwise),
        ///        0 off, above 0 world units at unit scale (the entity's world scale multiplies it).
        f32 SoftDistance = -1.0f;
        /// @brief Multiplies a six-way flipbook's emission.
        f32 EmissionIntensity = 1.0f;
        /// @brief Added to a six-way flipbook's normalised temperature before the ramp: a negative
        ///        offset reads cooler.
        f32 TemperatureOffset = 0.0f;
        /// @brief Replaces a six-way flipbook's emission ramp when non-empty; keys ascend in T, and
        ///        past MaxFlipbookEmissionKeys the rest are ignored.
        vector<FlipbookEmissionKey> EmissionRamp;
        /// @brief Replaces the ignition threshold when at least 0; below 0 takes the flipbook's.
        f32 Ignition = -1.0f;
        /// @brief Set by FlipbookSystem once a one-shot sequence has played its last frame; never
        ///        set for a looping one. A finished sprite draws nothing. Runtime-only: it carries
        ///        no VE_FIELD and never serializes.
        bool Finished = false;
    };

    /// @brief Plays a particle effect: many sprites and streaks flying out from one anchor.
    ///
    /// The renderer simulates and draws the effect's particles on the GPU through the same shading
    /// a FlipbookSprite uses, so a lit flipbook looks alike as one sprite or as a thousand
    /// particles. Each particle's state is written once, at spawn; its position at any age is the
    /// closed form of constant acceleration plus linear drag, so a particle with neither keeps the
    /// velocity it was given (p = p0 + v0 t) and every renderer drawing the scene shows it in the
    /// same place. ParticleEffectSystem advances Time and sets Finished once every particle and
    /// flash the effect can produce has ended, which the CPU knows by construction. The component
    /// is presentation, never replicated, as FlipbookSprite is; an EffectPool spawns one through
    /// EffectDesc::Particles.
    ///
    /// The instance anchors in the frame the scene's content rests in — FloatingOrigin's stable
    /// frame, or world space where the scene has none — at double precision, latched from the
    /// entity's world pose once (LatchParticleEffect), and its particles are f32 offsets from that
    /// anchor. Nothing re-bases the anchor when the origin moves, since it is already in the frame
    /// that does not move; writing Anchor afterwards moves the whole cloud rigidly, which is how an
    /// owner carries a live effect.
    struct ParticleEffectInstance
    {
        /// @brief The effect played; an instance whose effect is not resident holds at its start.
        AssetHandle<ParticleEffect> Effect;
        /// @brief The seed every particle's random draws derive from; 0 derives one at the latch
        ///        from the entity and the playback, so two instances differ unless told alike.
        u32 Seed = 0;
        /// @brief Added to every particle's initial velocity, in world units per second: the
        ///        emitting body's velocity. Not scaled by Scale.
        vec3 InheritVelocity{0.0f};
        /// @brief Multiplies every length the effect authors — spawn radii, sizes, speeds,
        ///        accelerations, streak lengths and flash ranges — and leaves every time alone, so
        ///        one authored effect serves many sizes with the same timing.
        f32 Scale = 1.0f;
        /// @brief Whether rate emitters keep spawning; clearing it stops them, and the instance
        ///        finishes once what they spawned has died.
        bool Emitting = true;
        /// @brief Seconds of playback, advanced by ParticleEffectSystem. Authoring it starts the
        ///        playback that far in, showing exactly what a playback run from 0 would show then.
        f32 Time = 0.0f;
        /// @brief The render layer it is drawn on; a view whose layer mask omits it does not draw it.
        ///
        /// A view that leaves it out still simulates the instance, so the effect plays on unseen
        /// and shows where it would have been once a view draws it again.
        RenderLayer Layer = RenderLayer::Default;
        /// @brief Where the effect is anchored, in the stable frame; set by LatchParticleEffect.
        ///        Runtime-only.
        dvec3 Anchor{0.0};
        /// @brief The effect's frame, which turns its emitters' cones; set by LatchParticleEffect.
        ///        Runtime-only.
        quat Orientation{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Distinguishes successive playbacks on one entity, so a renderer treats a restart
        ///        as a new instance. Runtime-only.
        u32 Playback = 0;
        /// @brief The Time at which Emitting was cleared, or below 0 while still emitting.
        ///        Runtime-only.
        f32 StopTime = -1.0f;
        /// @brief Whether Anchor and Orientation have been latched. Runtime-only.
        bool Anchored = false;
        /// @brief Set once every particle and flash of the effect has ended. Runtime-only.
        bool Finished = false;
    };

    /// @brief One particle an owner places for a fed emitter: where and when it spawns, and how it
    ///        leaves.
    struct ParticleFeedSpawn
    {
        /// @brief Where it spawns, in the frame the instance anchors in (FloatingOrigin's stable
        ///        frame, or world space without one).
        dvec3 Position{0.0};
        /// @brief Its velocity at spawn, world units per second; the emitter's cone speed and the
        ///        instance's InheritVelocity are added.
        vec3 Velocity{0.0f};
        /// @brief Seconds of the instance's playback it spawns at.
        f32 Time = 0.0f;
        /// @brief Which of the effect's emitters it belongs to; one not fed draws nothing.
        u32 Emitter = 0;
    };

    /// @brief The particles an owner has placed for an instance's fed emitters.
    ///
    /// Beside a ParticleEffectInstance whose effect has a fed emitter (ParticleEmitterDesc::Fed).
    /// The owner appends to Spawns in non-decreasing Time, at or before the instance's Time; each
    /// spawn's place in the sequence — First plus its index — is the particle's index, so every
    /// renderer draws it once and alike, and a renderer meeting the feed late issues only those
    /// still alive. ParticleEffectSystem drops spawns from the front once their emitter's longest
    /// lifetime has passed, advancing First. Presentation, never replicated or saved: a VE_TYPE.
    struct ParticleFeed
    {
        /// @brief The spawns still possibly alive, oldest first.
        vector<ParticleFeedSpawn> Spawns;
        /// @brief The sequence number of Spawns' first entry.
        u64 First = 0;
    };

    /// @brief A straight camera-facing ribbon between two world points: a beam, a streak, a tether.
    ///
    /// Drawn by the renderer's ribbon pass in the translucent layer, just ahead of the sprite pass,
    /// as an HDR quad from From to To that turns about its own axis to face the camera. Width,
    /// colour and opacity run linearly from the From end to the To end, and the quad falls off
    /// softly across its width. The endpoints are world space and ignore the entity's Transform, so
    /// the owner moves them directly. The pass depth-tests against the opaque scene without writing
    /// depth, sorts alpha ribbons back to front, and writes the bloom mask by luminance so an HDR
    /// ribbon glows. With a positive Lifetime the ribbon fades out over it as RibbonSystem advances
    /// Age, which is how a short-lived beam fades (see SpawnTransientBeam). A ribbon narrower than
    /// about a pixel draws a pixel wide at proportionally lower opacity, so a distant thin beam
    /// neither vanishes nor shimmers. Presentation only, never replicated.
    struct Ribbon
    {
        /// @brief The world position the ribbon starts at.
        vec3 From{0.0f};
        /// @brief The world position the ribbon ends at.
        vec3 To{0.0f};
        /// @brief The ribbon's world width at From.
        f32 WidthFrom = 0.1f;
        /// @brief The ribbon's world width at To.
        f32 WidthTo = 0.1f;
        /// @brief The linear HDR colour at From; above 1 drives bloom.
        vec3 ColorFrom{1.0f};
        /// @brief The linear HDR colour at To.
        vec3 ColorTo{1.0f};
        /// @brief The opacity at From (for an additive ribbon, its brightness scale).
        f32 OpacityFrom = 1.0f;
        /// @brief The opacity at To.
        f32 OpacityTo = 1.0f;
        /// @brief Adds light onto the scene (order-free) rather than compositing over it by coverage.
        bool Additive = true;
        /// @brief The render layer it is drawn on; a view whose layer mask omits it does not draw it
        ///        (an environment capture omits ViewAnchored and Display by default).
        RenderLayer Layer = RenderLayer::Default;
        /// @brief When positive, seconds over which the ribbon fades linearly to nothing; 0 holds
        ///        it at full opacity.
        f32 Lifetime = 0.0f;
        /// @brief Seconds of life so far; advanced by RibbonSystem. Authoring it starts the fade
        ///        part-way through.
        f32 Age = 0.0f;
    };

    /// @brief The most points a Trail's Outline holds; the rest are ignored.
    inline constexpr u32 MaxTrailOutlinePoints = 16;

    /// @brief How a Trail is drawn.
    enum class TrailShape : u8
    {
        /// @brief A camera-facing band through the samples.
        Band = 0,
        /// @brief A tube swept through the samples with the trail's cross-section outline.
        Tube = 1,
    };

    /// @brief One recorded position of a Trail's head, and how long ago it was recorded.
    struct TrailSample
    {
        /// @brief The world position the trail's entity stood at.
        vec3 Position{0.0f};
        /// @brief Seconds since the sample was recorded.
        f32 Age = 0.0f;
        /// @brief The entity's local X axis in world space as it stood, carrying its world scale.
        vec3 AxisX{1.0f, 0.0f, 0.0f};
        /// @brief The entity's local Y axis in world space as it stood, carrying its world scale.
        vec3 AxisY{0.0f, 1.0f, 0.0f};
        /// @brief The world velocity the sample moves at, decaying by the trail's Drag.
        vec3 Velocity{0.0f};
    };

    /// @brief A ribbon through the recent positions of its entity: a projectile's tracer, a
    ///        vehicle's wake, a missile's exhaust.
    ///
    /// RibbonSystem records the entity's world position each frame it has moved, into a bounded
    /// ring of samples, and ages them out after Lifetime; the ribbon pass draws a camera-facing
    /// strip through them to the entity's current position. Width, colour and opacity are full at
    /// the head and run down the length by each sample's age, opacity reaching nothing at Lifetime
    /// and width reaching Width × TailWidthScale, so the trail fades along its length and a trail
    /// that stops moving empties within Lifetime. Clearing Emitting stops recording and detaches
    /// the head, so the trail drains away behind a stopped or finished emitter; the trail goes with
    /// its entity when the entity is destroyed. Presentation only, never replicated: a prefab's
    /// Trail stands on every peer that instantiates it. See AttachTrail.
    ///
    /// A non-zero CrossSection gives the trail the shape of its emitter: an elliptical tube whose
    /// extents lie along the entity's local X and Y as each sample was recorded, drawn as that tube's
    /// silhouette from the viewer, so a flat emitter leaves a plume that reads wide from one side and
    /// thin from the other. Width then scales the cross-section, tapering by TailWidthScale as it does.
    /// Shape Tube draws the trail as real geometry instead: the Outline (or, with none, the
    /// CrossSection's ellipse, or a circle Width across) swept through the samples, each ring in
    /// the plane of the axes its sample recorded, glowing brightest where it faces the viewer.
    ///
    /// Samples need not stay where they were recorded. A new one leaves the emitter at
    /// EmitVelocity (along the entity's local axes) plus InheritVelocity of the emitter's own
    /// velocity, and every sample's velocity decays by Drag: so a jet stays straight out of its
    /// nozzle while it is fast, and slows to rest into an ordinary trail. With all three zero, as
    /// they default, a sample stays where it was recorded. The emitter's velocity is read from the
    /// head's motion between advances unless its owner supplies it (EmitterVelocity).
    struct Trail
    {
        /// @brief Seconds a sample lives; a trail with none draws nothing.
        f32 Lifetime = 0.5f;
        /// @brief The world width at the head; with a CrossSection, the scale applied to it.
        f32 Width = 0.2f;
        /// @brief The full extents of the trail's cross-section across its entity's local X and Y,
        ///        in the entity's units; zero on both draws a camera-facing band of Width.
        vec2 CrossSection{0.0f};
        /// @brief Whether the trail draws as a camera-facing band or a swept tube.
        TrailShape Shape = TrailShape::Band;
        /// @brief A Tube's cross-section as a closed polygon in the entity's local X and Y units,
        ///        scaled by Width; empty takes the CrossSection's ellipse. At most
        ///        MaxTrailOutlinePoints are used.
        vector<vec2> Outline;
        /// @brief How sharply a Tube's glow falls off toward its silhouette; 0 draws it flat.
        f32 Softness = 1.0f;
        /// @brief The velocity a new sample leaves with, along the entity's local axes, in world
        ///        units per second.
        vec3 EmitVelocity{0.0f};
        /// @brief The share of the emitter's own world velocity a new sample keeps.
        f32 InheritVelocity = 0.0f;
        /// @brief The rate a sample's velocity decays at, per second; 0 keeps it.
        f32 Drag = 0.0f;
        /// @brief The width at the tail as a fraction of Width; 1 keeps the trail even, 0 tapers it.
        f32 TailWidthScale = 1.0f;
        /// @brief The linear HDR colour; above 1 drives bloom.
        vec3 Color{1.0f};
        /// @brief The opacity at the head (for an additive trail, its brightness scale).
        f32 Opacity = 1.0f;
        /// @brief The most samples the trail holds; past it the oldest is dropped.
        u32 MaxSamples = 32;
        /// @brief How far, in world units, the head must move past the newest sample before
        ///        another is recorded; 0 records every frame the head moves at all.
        f32 MinSampleDistance = 0.0f;
        /// @brief Adds light onto the scene (order-free) rather than compositing over it by coverage.
        bool Additive = true;
        /// @brief The render layer it is drawn on; a view whose layer mask omits it does not draw it
        ///        (an environment capture omits ViewAnchored and Display by default).
        RenderLayer Layer = RenderLayer::Default;
        /// @brief Whether the trail records new samples and joins them to the entity; clearing it
        ///        lets the recorded trail fade out where it lies.
        bool Emitting = true;
        /// @brief The recorded samples, oldest first; advanced by RibbonSystem. Runtime-only: it
        ///        carries no VE_FIELD and never serializes.
        vector<TrailSample> Samples;
        /// @brief Where the head stood at the last advance, from which the emitter's velocity is
        ///        read. Runtime-only.
        vec3 PreviousHead{0.0f};
        /// @brief Whether PreviousHead holds an advance's head. Runtime-only.
        bool HasPreviousHead = false;
        /// @brief The emitter's world velocity, supplied by its owner for the next advance only.
        ///        Runtime-only.
        ///
        /// Read in place of the head's motion between advances, which is exact only when the head
        /// moves every frame: an emitter stepped on a fixed tick moves on some frames and not
        /// others, so its motion reads as zero on one frame and double on the next.
        vec3 EmitterVelocity{0.0f};
        /// @brief Whether EmitterVelocity is supplied; the next advance clears it. Runtime-only.
        bool HasEmitterVelocity = false;
    };

    /// @brief Where in the frame a RibbonPath draws.
    enum class RibbonPlacement : u8
    {
        /// @brief Into the lit scene colour at the render allocation, after the translucent pass:
        ///        depth-tested, resolved by TAA with the scene, and resampled by the promotion.
        Scene = 0,
        /// @brief Into the finished scene colour at the post-resolve allocation, after the
        ///        post-process effects and before the pre-bloom GUI overlay and bloom, through the
        ///        unjittered projection: never temporally resolved or resampled.
        PostResolve = 1,
    };

    /// @brief One polyline of a RibbonPath, in its entity's local space.
    ///
    /// Consecutive points closer than about a ten-thousandth of a world unit merge into one. A strip
    /// left with one distinct point draws a round, camera-facing dot of diameter Width centred on it
    /// (the polyline analogue of a round cap on a zero-length subpath); one with none draws nothing.
    /// Each joint's tangent is taken across its two neighbours (wrapping at the ends of a closed
    /// strip), so the segments meeting there share their edge and a curve drawn as a fine polyline
    /// shows neither a gap nor a notch.
    struct RibbonStrip
    {
        /// @brief The polyline's points, in the entity's local space.
        vector<vec3> Points;
        /// @brief Joins the last point back to the first, with a shared joint there; a closed strip
        ///        of N distinct points draws N segments, an open one N − 1.
        bool Closed = false;
        /// @brief The band's world width (a one-point strip's dot diameter) at unit scale; the
        ///        entity's world scale multiplies it.
        f32 Width = 0.01f;
        /// @brief The linear HDR colour; above 1 drives bloom.
        vec3 Color{1.0f};
        /// @brief The opacity (for an additive path, its brightness scale); 0 hides the strip.
        f32 Opacity = 1.0f;
    };

    /// @brief An authored shape of camera-facing ribbons that rides its entity: a wireframe, a
    ///        drawn orbit, a range ring, a reticle standing in the world.
    ///
    /// Each strip is a polyline in the entity's local space, drawn by the renderer's ribbon pass
    /// exactly as a Trail is — camera-facing about its own axis, floored at about a pixel wide with
    /// its opacity scaled to match, depth-tested against the opaque scene without writing depth,
    /// additive or sorted alpha-over, and glowing through the bloom mask by luminance. The points
    /// are placed by the entity's interpolated world transform, the pose its meshes draw at, so a
    /// path parented under a moving, turning entity rides it without judder, and the entity's world
    /// scale (the length of its world X axis, as for FlipbookSprite::Size) multiplies every strip's
    /// Width. A path on an entity without a Transform draws nothing, as a mesh's does. The strips are
    /// plain data a system may rewrite every frame — points, colours, the strip count — and the
    /// pass draws whatever they hold. Presentation only, never replicated, and nothing advances
    /// it; a floating origin's re-base reaches it through its entity's Transform.
    ///
    /// Placement chooses where in the frame the path draws. Scene (the default) draws with the rest
    /// of the scene and is resolved and resampled with it. PostResolve draws the same bands at the
    /// output resolution after the temporal resolve and the upscale, through the unjittered
    /// projection, so crisp world-space linework — gizmos, projected orbits, wireframe holograms,
    /// measurement guides — neither softens nor shimmers; it is occluded per fragment by the scene
    /// depth rather than by a depth test, and still feeds bloom.
    ///
    /// Occluded chooses whether nearer scene geometry hides the path. A path that is not drawn over
    /// everything else of its placement, after the occluded paths, in either placement.
    struct RibbonPath
    {
        /// @brief The polylines drawn, each with its own width and colour.
        vector<RibbonStrip> Strips;
        /// @brief Adds light onto the scene (order-free) rather than compositing over it by coverage.
        bool Additive = true;
        /// @brief The render layer it is drawn on; a view whose layer mask omits it does not draw it
        ///        (an environment capture omits ViewAnchored and Display by default).
        RenderLayer Layer = RenderLayer::Default;
        /// @brief Where in the frame the path draws; Scene unless the linework must stay crisp.
        RibbonPlacement Placement = RibbonPlacement::Scene;
        /// @brief Whether nearer scene geometry hides it; false draws it through everything, as a
        ///        hologram or a gizmo that must stay readable inside the geometry it stands in.
        bool Occluded = true;
        /// @brief Whether it keeps its authored brightness whatever the frame's exposure.
        ///
        /// A path draws into the scene HDR, so the tone curve multiplies it by the frame's exposure.
        /// When set, its colour is divided by that exposure as it draws, so it shows as it would at
        /// an exposure of 1 — an instrument stays as bright as it was authored while auto-exposure
        /// follows the scene. Its coverage, and the bloom mask it writes, follow the divided colour.
        bool CompensateExposure = false;
    };
}

VE_TYPE(::Veng::ParticleFeed, 0x22741CAA9D6E422BULL);

VE_ENUM(::Veng::SpriteBlend, 0xE291125B259E4EC5ULL)
VE_ENUMERATOR(Asset)
VE_ENUMERATOR(Alpha)
VE_ENUMERATOR(Additive)
VE_ENUM_END();

VE_ENUM(::Veng::SpriteFrameBlend, 0x498911DAF3756ACFULL)
VE_ENUMERATOR(Asset)
VE_ENUMERATOR(Step)
VE_ENUMERATOR(Crossfade)
VE_ENUMERATOR(Motion)
VE_ENUM_END();

VE_REFLECT(::Veng::FlipbookEmissionKey, 0x2E44CF4E37870991ULL)
VE_FIELD(T, .DisplayName = "Temperature", .Tooltip = "Normalised temperature the key sits at",
         .Display = {.Min = 0.0, .Max = 1.0, .Step = 0.01})
VE_FIELD(Color, .DisplayName = "Color", .Tooltip = "Linear HDR emission")
VE_REFLECT_END();

VE_REFLECT(::Veng::FlipbookSprite, 0x36AE315773D9D391ULL)
VE_FIELD(Flipbook, .DisplayName = "Flipbook")
VE_FIELD(Size, .DisplayName = "Size",
         .Tooltip = "Width in world units at unit scale; 0 takes the flipbook's world extent",
         .Display = {.Min = 0.0, .Step = 0.05})
VE_FIELD(Tint, .DisplayName = "Tint", .Tooltip = "Linear HDR colour multiplier")
VE_FIELD(Opacity, .DisplayName = "Opacity", .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(PlaybackRate, .DisplayName = "Playback Rate", .Display = {.Min = 0.0, .Step = 0.05})
VE_FIELD(DurationOverride, .DisplayName = "Duration Override",
         .Tooltip = "When positive, plays the whole sequence over this many seconds",
         .Display = {.Min = 0.0, .Step = 0.05})
VE_FIELD(Time, .DisplayName = "Time", .Display = {.Min = 0.0})
VE_FIELD(Blend, .DisplayName = "Blend")
VE_FIELD(Rotation, .DisplayName = "Rotation", .Display = {.Step = 0.01})
VE_FIELD(FrameBlend, .DisplayName = "Frame Blend",
         .Tooltip = "How frames blend; Asset takes the flipbook's recommendation")
VE_FIELD(SoftDistance, .DisplayName = "Soft Distance",
         .Tooltip = "Fade depth into geometry; below 0 the flipbook's default, 0 off",
         .Display = {.Min = -1.0, .Step = 0.05})
VE_FIELD(EmissionIntensity, .DisplayName = "Emission Intensity",
         .Tooltip = "Multiplies a six-way flipbook's emission",
         .Display = {.Min = 0.0, .Step = 0.05})
VE_FIELD(TemperatureOffset, .DisplayName = "Temperature Offset",
         .Tooltip = "Added to the temperature before the ramp; negative reads cooler",
         .Display = {.Min = -1.0, .Max = 1.0, .Step = 0.01})
VE_ARRAY_FIELD(EmissionRamp, .DisplayName = "Emission Ramp",
               .Tooltip = "Replaces the flipbook's ramp when non-empty")
VE_FIELD(Ignition, .DisplayName = "Ignition",
         .Tooltip = "Replaces the ignition threshold when at least 0",
         .Display = {.Min = -1.0, .Max = 1.0, .Step = 0.01})
VE_REFLECT_END();

// Presentation, local-only — never replicated. The anchor, orientation, playback counter, stop
// time and latch and finish flags are runtime-only and carry no reflected field.
VE_REFLECT(::Veng::ParticleEffectInstance, 0x67B79C3C429B3740ULL)
VE_FIELD(Effect, .DisplayName = "Effect")
VE_FIELD(Seed, .DisplayName = "Seed", .Tooltip = "0 derives one per entity and playback")
VE_FIELD(InheritVelocity, .DisplayName = "Inherit Velocity",
         .Tooltip = "World velocity added to every particle's initial velocity")
VE_FIELD(Scale, .DisplayName = "Scale", .Tooltip = "Multiplies every length; times are unchanged",
         .Display = {.Min = 0.0, .Step = 0.05})
VE_FIELD(Emitting, .DisplayName = "Emitting")
VE_FIELD(Time, .DisplayName = "Time", .Display = {.Min = 0.0})
VE_FIELD(Layer, .DisplayName = "Render layer")
VE_REFLECT_END();

VE_REFLECT(::Veng::Ribbon, 0xA8DF9582C85E18F2ULL)
VE_FIELD(From, .DisplayName = "From", .Tooltip = "World-space start point")
VE_FIELD(To, .DisplayName = "To", .Tooltip = "World-space end point")
VE_FIELD(WidthFrom, .DisplayName = "Width From", .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(WidthTo, .DisplayName = "Width To", .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(ColorFrom, .DisplayName = "Color From", .Tooltip = "Linear HDR colour at the start")
VE_FIELD(ColorTo, .DisplayName = "Color To", .Tooltip = "Linear HDR colour at the end")
VE_FIELD(OpacityFrom, .DisplayName = "Opacity From", .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(OpacityTo, .DisplayName = "Opacity To", .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(Additive, .DisplayName = "Additive")
VE_FIELD(Layer, .DisplayName = "Render layer")
VE_FIELD(Lifetime, .DisplayName = "Lifetime",
         .Tooltip = "When positive, seconds over which the ribbon fades out",
         .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(Age, .DisplayName = "Age", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_ENUM(::Veng::TrailShape, 0xB110381580C8CCB2ULL)
VE_ENUMERATOR(Band)
VE_ENUMERATOR(Tube)
VE_ENUM_END();

VE_REFLECT(::Veng::Trail, 0xCA5518C6D69E6858ULL)
VE_FIELD(Lifetime, .DisplayName = "Lifetime", .Tooltip = "Seconds a sample lives",
         .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(Width, .DisplayName = "Width", .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(CrossSection, .DisplayName = "Cross Section",
         .Tooltip = "Extents across the entity's local X and Y; zero draws a camera-facing band",
         .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(Shape, .DisplayName = "Shape", .Tooltip = "A camera-facing band or a swept tube")
VE_ARRAY_FIELD(Outline, .DisplayName = "Outline",
               .Tooltip = "A tube's cross-section polygon in the entity's local X and Y")
VE_FIELD(Softness, .DisplayName = "Softness",
         .Tooltip = "How sharply a tube's glow falls off toward its silhouette",
         .Display = {.Min = 0.0, .Step = 0.05})
VE_FIELD(EmitVelocity, .DisplayName = "Emit Velocity",
         .Tooltip = "Velocity a new sample leaves with, along the entity's local axes")
VE_FIELD(InheritVelocity, .DisplayName = "Inherit Velocity",
         .Tooltip = "Share of the emitter's own velocity a new sample keeps",
         .Display = {.Min = 0.0, .Max = 1.0, .Step = 0.05})
VE_FIELD(Drag, .DisplayName = "Drag", .Tooltip = "Per-second decay of a sample's velocity",
         .Display = {.Min = 0.0, .Step = 0.1})
VE_FIELD(TailWidthScale, .DisplayName = "Tail Width Scale",
         .Tooltip = "Width at the tail as a fraction of the head's", .Display = {.Min = 0.0})
VE_FIELD(Color, .DisplayName = "Color", .Tooltip = "Linear HDR colour")
VE_FIELD(Opacity, .DisplayName = "Opacity", .Display = {.Min = 0.0, .Max = 1.0})
VE_FIELD(MaxSamples, .DisplayName = "Max Samples", .Display = {.Min = 1.0})
VE_FIELD(MinSampleDistance, .DisplayName = "Min Sample Distance",
         .Tooltip = "World distance the head moves before another sample is recorded",
         .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(Additive, .DisplayName = "Additive")
VE_FIELD(Layer, .DisplayName = "Render layer")
VE_FIELD(Emitting, .DisplayName = "Emitting")
VE_REFLECT_END();

VE_REFLECT(::Veng::RibbonStrip, 0x42276DD8FF4B9918ULL)
VE_ARRAY_FIELD(Points, .DisplayName = "Points", .Tooltip = "Polyline points in the entity's space")
VE_FIELD(Closed, .DisplayName = "Closed", .Tooltip = "Joins the last point back to the first")
VE_FIELD(Width, .DisplayName = "Width", .Tooltip = "World width at unit scale",
         .Display = {.Min = 0.0, .Step = 0.001})
VE_FIELD(Color, .DisplayName = "Color", .Tooltip = "Linear HDR colour")
VE_FIELD(Opacity, .DisplayName = "Opacity", .Display = {.Min = 0.0, .Max = 1.0})
VE_REFLECT_END();

VE_ENUM(::Veng::RibbonPlacement, 0x7ADCEEC4BBBADE05ULL)
VE_ENUMERATOR(Scene)
VE_ENUMERATOR(PostResolve)
VE_ENUM_END();

VE_REFLECT(::Veng::RibbonPath, 0x9BCA6107D31AB0F6ULL)
VE_ARRAY_FIELD(Strips, .DisplayName = "Strips")
VE_FIELD(Additive, .DisplayName = "Additive")
VE_FIELD(Layer, .DisplayName = "Render layer")
VE_FIELD(Placement, .DisplayName = "Placement",
         .Tooltip = "Scene: resolved with the scene. PostResolve: drawn crisp at output resolution")
VE_FIELD(Occluded, .DisplayName = "Occluded",
         .Tooltip = "Whether nearer scene geometry hides it; off draws it through everything")
VE_FIELD(CompensateExposure, .DisplayName = "Compensate Exposure",
         .Tooltip = "Keep the authored brightness whatever the frame's exposure")
VE_REFLECT_END();
