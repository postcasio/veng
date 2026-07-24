#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    struct Skeleton;
    struct Animation;

    /// @brief One bone's local transform as decomposed TRS — the poseable form used for blending.
    ///
    /// Rotations blend by slerp and translations/scales by lerp, so a bone's pose is carried as its
    /// components rather than a composed matrix (which cannot be blended correctly). ComposeLocalPose
    /// turns a per-bone JointPose list back into the local matrices the skinning palette needs.
    struct JointPose
    {
        /// @brief Local translation in parent space.
        vec3 Translation{0.0f};
        /// @brief Local rotation in parent space.
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Local scale in parent space.
        vec3 Scale{1.0f};
    };

    /// @brief The bracketing pair and blend weight a 1-D blend space resolves a parameter to.
    ///
    /// Lo/Hi index the two samples straddling the parameter (Lo == Hi at or beyond an end), and
    /// Weight is the fraction toward Hi in [0, 1] — so the blended pose is
    /// mix(pose[Lo], pose[Hi], Weight).
    struct BlendBracket
    {
        /// @brief Index of the lower bracketing sample.
        usize Lo = 0;
        /// @brief Index of the upper bracketing sample (equals Lo at or beyond an end).
        usize Hi = 0;
        /// @brief Blend weight toward Hi, in [0, 1].
        f32 Weight = 0.0f;
    };

    /// @brief Resolves a 1-D blend space's bracketing samples and weight for a parameter.
    ///
    /// The samples' thresholds must be sorted ascending. Below the first threshold or above the last,
    /// Lo == Hi is the end sample and Weight is 0 (that clip at full weight); between two thresholds
    /// Lo/Hi straddle the parameter and Weight is the normalized distance between them. A parameter
    /// exactly equal to a threshold resolves to that sample at full weight. Pure, so the animation
    /// system and its tests drive the identical bracketing math.
    /// @param thresholds  The samples' thresholds, sorted ascending (at least one).
    /// @param parameter   The value to resolve.
    /// @return The bracketing indices and the weight toward Hi.
    [[nodiscard]] BlendBracket FindBlendBracket(std::span<const f32> thresholds, f32 parameter);

    /// @brief Advances a crossfade weight toward 1 over a fade duration, clamped and monotonic.
    ///
    /// The per-tick step of a state crossfade: the weight rises by delta / fadeIn each tick and
    /// clamps at 1, so it is monotonic with no overshoot and settles exactly at 1. A zero fade
    /// completes in one tick.
    /// @param weight   The current weight in [0, 1].
    /// @param fadeIn   The fade duration in seconds; <= 0 completes immediately.
    /// @param delta    Time in seconds since the previous tick.
    /// @return The advanced weight in [0, 1].
    [[nodiscard]] f32 AdvanceCrossfade(f32 weight, f32 fadeIn, f32 delta);

    /// @brief Samples an animation into per-bone local TRS poses at a given time.
    ///
    /// The decomposed companion to SampleAnimationPose: each bone starts at its skeleton bind-pose
    /// local transform, a bone with an animation channel taking the channel's interpolated
    /// translation/rotation/scale at the (looped or clamped) time. Emitting TRS rather than composed
    /// matrices is what lets poses be blended per component. Pure — no scene, no device.
    /// @param skeleton  The skeleton whose bone order the channels target.
    /// @param animation The clip to sample.
    /// @param time      Playback time in seconds.
    /// @param loop      Whether to wrap time into [0, Duration) or clamp it.
    /// @param out       Receives one local TRS pose per skeleton bone.
    void SampleAnimationLocalPose(const Skeleton& skeleton, const Animation& animation, f32 time,
                                  bool loop, vector<JointPose>& out);

    /// @brief Blends two equal-length local poses per bone: slerp rotation, lerp translation/scale.
    ///
    /// out[i] is the blend of a[i] and b[i] at the given weight — rotation by slerp, translation and
    /// scale by lerp — the pose-space operation both the blend space and the state crossfade compose
    /// with. A weight of 0 yields a, 1 yields b. Pure.
    /// @param a       The pose at weight 0.
    /// @param b       The pose at weight 1 (must match a's length).
    /// @param weight  The blend fraction toward b, in [0, 1].
    /// @param out     Receives the blended pose (resized to a's length).
    void BlendLocalPoses(const vector<JointPose>& a, const vector<JointPose>& b, f32 weight,
                         vector<JointPose>& out);

    /// @brief Composes per-bone local TRS poses into the local matrices the skinning palette needs.
    ///
    /// out[i] = translate(Translation) * mat4(Rotation) * scale(Scale) — the matrix form
    /// SampleAnimationPose emits directly, produced here from a blended JointPose list.
    /// @param pose  The per-bone local TRS poses.
    /// @param out   Receives one local matrix per pose entry.
    void ComposeLocalPose(const vector<JointPose>& pose, vector<mat4>& out);

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

    /// @brief Identifies the bone a clip bakes its root motion onto.
    ///
    /// Returns the topmost (lowest topological index) bone whose position track varies over the
    /// clip beyond a small epsilon — in a typical rig the hips, which carry locomotion while
    /// limbs are rotation-only and any armature node above them is static. A clip authored in
    /// place (no varying position track) has no root-motion bone.
    /// @param skeleton  The skeleton whose bone order the channels target.
    /// @param animation The clip to inspect.
    /// @return The root-motion bone index, or -1 if the clip bakes no translation.
    [[nodiscard]] i32 FindRootMotionBone(const Skeleton& skeleton, const Animation& animation);

    /// @brief Samples a bone's animated local position at a time, falling back to its bind value.
    ///
    /// Raw sample with no loop/clamp wrapping — the caller passes the already-resolved time. Used
    /// to extract the per-tick root-motion delta.
    /// @param skeleton  The skeleton supplying the bind-pose fallback.
    /// @param animation The clip whose channel is sampled.
    /// @param bone      The bone index whose position track is read.
    /// @param time      Playback time in seconds.
    /// @return The bone's local-space position at time.
    [[nodiscard]] vec3 SampleBoneLocalPosition(const Skeleton& skeleton, const Animation& animation,
                                               i32 bone, f32 time);

    /// @brief View-phase system that advances Animators and writes each entity's SkinnedPose.
    ///
    /// For every entity with (MeshRenderer, Animator) whose mesh is a resident skinned mesh, it
    /// advances the Animator's time (when Playing), samples the clip against the mesh's skeleton,
    /// computes the skinning palette, and stores it in the entity's SkinnedPose (added on first
    /// run). Runs in the View phase so it poses against finalized Sim state; the renderer uploads
    /// the resulting palette. A skinned mesh with no Animator is posed at its bind pose by the
    /// renderer, so it needs no SkinnedPose.
    ///
    /// An Animator with no AnimationBlend and no AnimationStateSet plays its single Clip exactly as
    /// described above. An entity carrying an AnimationBlend (Veng/Scene/AnimationBlend.h) instead
    /// plays a phase-synchronized 1-D blend of the bracketing samples for the blend Parameter, and an
    /// AnimationStateSet crossfades a named state over that blend while one is requested. Both are
    /// composed in pose space (slerp rotation, lerp translation/scale) into the same SkinnedPose the
    /// renderer already consumes, so nothing downstream changes.
    ///
    /// It also handles the single Clip's baked root motion per its RootMotionMode: every mode strips
    /// the root bone's translation from the pose, then discards the extracted delta, applies it to
    /// the entity Transform (Presentation), or publishes it as a RootMotionDelta (Drive) for the
    /// Sim-phase RootMotionDriveSystem to consume. A blend or state pose strips the root translation
    /// (the character controller owns position) and publishes no delta.
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
}

VE_SYSTEM(::Veng::AnimationSystem, 0xAA7E24568E6FBCB6ULL, "Animation");
