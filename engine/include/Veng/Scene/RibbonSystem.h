#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    struct Trail;

    /// @brief Advances one trail by a frame: ages its samples, drops the expired, records the head.
    ///
    /// Every sample ages by @p delta and those at or past the trail's Lifetime are dropped. While
    /// the trail is Emitting, @p head is recorded as a new sample when the trail holds none or when
    /// it lies further than MinSampleDistance from the newest; then the oldest are dropped until at
    /// most MaxSamples remain. A trail whose head stops moving therefore empties to its one newest
    /// sample within Lifetime, and draws nothing.
    /// @param trail  The trail to advance.
    /// @param head   The trail's entity's world position this frame.
    /// @param delta  Seconds since the previous advance.
    VE_API void AdvanceTrail(Trail& trail, const vec3& head, f32 delta);

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
