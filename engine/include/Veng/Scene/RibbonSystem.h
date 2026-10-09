#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    struct Trail;

    /// @brief Advances one trail by a frame: moves and ages its samples, drops the expired, records
    ///        the head.
    ///
    /// Every sample moves by its velocity over @p delta as that velocity decays by the trail's Drag,
    /// exactly rather than in steps, so its path does not depend on the frame rate; then it ages
    /// by @p delta and those at or past the trail's Lifetime are dropped. A new sample leaves with
    /// the trail's EmitVelocity carried into world space by the head's rotation (not its scale),
    /// plus InheritVelocity of the emitter's velocity — the head's travel since the last advance
    /// over @p delta, zero on the first advance after a restart — or the trail's EmitterVelocity
    /// when its owner supplied one, which this advance consumes. A trail whose samples move drops
    /// any sample a newer one has overtaken — the chain is joined in emission order, so a faster
    /// sample passing a slower one would double it back on itself. While
    /// the trail is Emitting, @p head is recorded as a new sample when the trail holds none or when
    /// it lies further than MinSampleDistance from the newest; then the oldest are dropped until at
    /// most MaxSamples remain. A trail whose head stops moving therefore empties to its one newest
    /// sample within Lifetime, and draws nothing.
    /// @param trail  The trail to advance.
    /// @param head   The trail's entity's world transform this frame; a recorded sample keeps its
    ///               position and its X and Y axes, which orient a CrossSection.
    /// @param delta  Seconds since the previous advance.
    VE_API void AdvanceTrail(Trail& trail, const mat4& head, f32 delta);

    /// @brief Advances one trail by a frame with its head at a position, its axes the world's.
    ///
    /// The overload for a trail with no CrossSection, which reads no orientation.
    /// @param trail  The trail to advance.
    /// @param head   The trail's entity's world position this frame.
    /// @param delta  Seconds since the previous advance.
    VE_API void AdvanceTrail(Trail& trail, const vec3& head, f32 delta);

    /// @brief The width a trail's elliptical cross-section shows a viewer at one point of it.
    ///
    /// The cross-section is the ellipse with semi-axes @p semiX and @p semiY, and the trail runs
    /// along @p tangent. The band is drawn across the side perpendicular to the trail and to the eye
    /// ray, as the ribbon pass draws every band, and its width is the ellipse's extent along that
    /// side: the full span of an axis seen square across it, the span of the other seen edge on, and
    /// the same for a circle from every side. Looking straight down the trail leaves no such side,
    /// and one perpendicular to the trail stands in.
    /// @param semiX       The ellipse's first semi-axis, world space.
    /// @param semiY       The ellipse's second semi-axis, world space.
    /// @param tangent     The trail's direction at the point.
    /// @param eyeToPoint  The point less the eye's position.
    /// @return The band's full width there.
    [[nodiscard]] VE_API f32 TrailCrossSectionWidth(const vec3& semiX, const vec3& semiY,
                                                    const vec3& tangent, const vec3& eyeToPoint);

    /// @brief Empties a trail and forgets where its head stood, so it starts afresh at the next
    ///        advance.
    ///
    /// What a teleported or re-stood emitter needs: its old samples would join it across the jump,
    /// and the jump would read as one frame's enormous velocity for its first new sample.
    /// @param trail  The trail to restart.
    VE_API void RestartTrail(Trail& trail);

    /// @brief Gives @p entity a trail that starts fresh at its current position.
    ///
    /// Adds @p trail to the entity, or replaces the trail it carries, with any recorded samples
    /// cleared, so a reused or teleported entity does not draw a streak from where it stood before.
    /// RibbonSystem records it from the next frame; the entity needs a Transform to be followed.
    /// @param scene   The scene the entity lives in.
    /// @param entity  The entity the trail follows.
    /// @param trail   The trail's look and timing; its Samples are ignored.
    /// @return The entity's trail component.
    VE_API Trail& AttachTrail(Scene& scene, Entity entity, const Trail& trail);

    /// @brief Moves every ribbon's endpoints and every trail's recorded samples by one offset.
    ///
    /// The re-base a scene drawn about a floating origin needs. Moving the origin moves every world
    /// position by the same displacement, but a ribbon's endpoints are world positions its owner
    /// wrote and a trail's samples are world positions recorded on earlier frames, so neither
    /// follows by itself: left alone, a trail streams off along its viewer's own motion. Call it
    /// with the displacement each time the origin moves, before RibbonSystem records the frame's
    /// heads. An entity's Transform is not touched; re-basing it is the owner's, as it always is.
    /// The head position a trail remembers for its emitter's velocity moves with its samples.
    /// @param scene   The scene whose ribbons and trails are re-based.
    /// @param offset  The displacement every world position moved by.
    VE_API void OffsetRibbons(Scene& scene, const vec3& offset);

    /// @brief View-phase system that ages every Ribbon and advances every Trail.
    ///
    /// Each frame it adds the frame delta to every Ribbon's Age (the fade a positive Lifetime runs),
    /// and advances every Trail on an entity with a Transform through AdvanceTrail, with the head at
    /// the entity's world position interpolated by the frame's render fraction, so the samples lie
    /// on the path the entity is drawn along. Presentation only: it runs in the View phase, on every
    /// peer, and writes nothing authoritative. A level lists it to have its trails record and its
    /// ribbons fade.
    class VE_API RibbonSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — ribbons and trails are presentation, advanced once per frame.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Ages every ribbon and advances every trail.
        /// @param scene    The scene whose ribbons and trails advance.
        /// @param delta    Time in seconds since the previous frame.
        /// @param context  Per-tick services; its Alpha places each trail's head.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::RibbonSystem, 0x989855CD1F605885ULL, "Ribbon");
