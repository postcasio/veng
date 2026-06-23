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
    };

    /// @brief AssetTypeTrait specialization mapping Animation to AssetType::Animation.
    template <>
    struct AssetTypeTrait<Animation>
    {
        /// @brief The asset type tag for Animation.
        static constexpr AssetType Type = AssetType::Animation;
    };
}
