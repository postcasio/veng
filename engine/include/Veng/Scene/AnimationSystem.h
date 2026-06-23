#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    struct Skeleton;
    struct Animation;

    /// @brief Samples an animation into per-bone local pose matrices at a given time.
    ///
    /// Each bone starts at its skeleton bind-pose local transform; a bone with an animation
    /// channel is overridden by the channel's interpolated translation/rotation/scale at the
    /// (looped or clamped) time. Pure math — no scene, no device — so it is the deterministic
    /// core the animation system and its tests drive.
    /// @param skeleton  The skeleton whose bone order the channels target.
    /// @param animation The clip to sample.
    /// @param time      Playback time in seconds.
    /// @param loop      Whether to wrap time into [0, Duration) or clamp it.
    /// @param out       Receives one local transform matrix per skeleton bone.
    void SampleAnimationPose(const Skeleton& skeleton, const Animation& animation, f32 time,
                             bool loop, vector<mat4>& out);

    /// @brief View-phase system that advances Animators and writes each entity's SkinnedPose.
    ///
    /// For every entity with (MeshRenderer, Animator) whose mesh is a resident skinned mesh, it
    /// advances the Animator's time (when Playing), samples the clip against the mesh's skeleton,
    /// computes the skinning palette, and stores it in the entity's SkinnedPose (added on first
    /// run). Runs in the View phase so it poses against finalized Sim state; the renderer uploads
    /// the resulting palette. A skinned mesh with no Animator is posed at its bind pose by the
    /// renderer, so it needs no SkinnedPose.
    class AnimationSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — posing is presentation derived after the Sim phase.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Advances each Animator and writes its entity's SkinnedPose.
        /// @param scene    The scene whose animators are updated.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };

    VE_SYSTEM(AnimationSystem, 0xAA7E24568E6FBCB6ULL, "Animation");
}
