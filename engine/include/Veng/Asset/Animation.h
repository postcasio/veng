#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

namespace Veng
{
    /// @brief One timed vec3 key (position or scale) in an animation track.
    struct Vec3Key
    {
        /// @brief Key time in seconds.
        f32 Time = 0.0f;
        /// @brief Keyed value.
        vec3 Value{0.0f};
    };

    /// @brief One timed quaternion key (rotation) in an animation track.
    struct QuatKey
    {
        /// @brief Key time in seconds.
        f32 Time = 0.0f;
        /// @brief Keyed rotation.
        quat Value{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// @brief One bone's animation track: position/rotation/scale keyframe lists.
    ///
    /// An empty list for a component means the bone holds its skeleton bind-pose value for
    /// that component. BoneIndex targets a bone in the paired Skeleton's bone order.
    struct AnimationChannel
    {
        /// @brief Target bone index in the skeleton's bone array.
        u32 BoneIndex = 0;
        /// @brief Position keyframes, ascending in time.
        vector<Vec3Key> Position;
        /// @brief Rotation keyframes, ascending in time.
        vector<QuatKey> Rotation;
        /// @brief Scale keyframes, ascending in time.
        vector<Vec3Key> Scale;
    };

    /// @brief The RootMotionBone value of a clip whose root-motion bone has not been computed.
    inline constexpr i32 RootMotionBoneUnknown = -2;

    /// @brief A set of per-bone keyframe tracks animating a skeleton, loaded by AssetId.
    ///
    /// A CPU-only asset (no GPU resource): the animation system samples it against a Skeleton
    /// each frame to drive the GPU skinning palette. Channels index the skeleton's bone order.
    struct Animation
    {
        /// @brief Total duration in seconds.
        f32 Duration = 0.0f;
        /// @brief Per-bone animation tracks.
        vector<AnimationChannel> Channels;
        /// @brief The clip's root-motion bone (FindAnimatedRootBone), computed once at load.
        ///
        /// -1 when the clip bakes no translation. A clip built in code rather than loaded holds
        /// RootMotionBoneUnknown until a caller sets it, and is then scanned on each use.
        i32 RootMotionBone = RootMotionBoneUnknown;
    };

    /// @brief Returns the topmost bone whose position track varies over the clip.
    ///
    /// The lowest bone index among channels whose position keys span more than a small epsilon on
    /// some axis: bones are topological, so that is the highest varying bone in the hierarchy (in a
    /// typical rig the hips). Independent of any skeleton, so a loader can cache it on the clip.
    /// @param animation  The clip to inspect.
    /// @param boneLimit  Channels targeting a bone index at or past this are ignored.
    /// @return The bone index, or -1 when no in-range channel's position varies.
    [[nodiscard]] i32 FindAnimatedRootBone(const Animation& animation,
                                           usize boneLimit = static_cast<usize>(-1));

    /// @brief AssetTypeTrait specialization mapping Animation to AssetTypes::Animation.
    template <>
    struct AssetTypeTrait<Animation>
    {
        /// @brief The asset type tag for Animation.
        static constexpr AssetTypeId Type = AssetTypes::Animation;
    };
}
