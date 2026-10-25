#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    struct Animation;
    struct Skeleton;

    /// @brief How an Animator treats a clip's baked root motion.
    ///
    /// Root motion is the locomotion translation a clip bakes onto its root bone, which would
    /// otherwise slide the whole skeleton out from under the entity. Every mode strips that
    /// translation from the rendered pose; they differ only in where the extracted per-tick
    /// delta goes. Integer values are stable — persisted in prefabs.
    enum class RootMotionMode : u32
    {
        /// @brief Strip the root translation and discard it: the entity stays in place.
        Discard = 0,
        /// @brief Strip it and apply the delta to the entity's own Transform (View-phase, cosmetic).
        Presentation = 1,
        /// @brief Strip it and publish the delta as a RootMotionDelta for a Sim mover to consume.
        Drive = 2,
    };

    /// @brief Plays an Animation clip on a skinned-mesh entity.
    ///
    /// The animation system advances Time each tick (when Playing), samples Clip against the
    /// entity mesh's Skeleton, and writes the result into the entity's SkinnedPose for the
    /// renderer to upload. A skinned mesh with no Animator shows its bind pose.
    struct Animator
    {
        /// @brief The animation clip to play.
        AssetHandle<Animation> Clip;
        /// @brief Current playback time in seconds (advanced by the system).
        f32 Time = 0.0f;
        /// @brief Playback rate multiplier.
        f32 Speed = 1.0f;
        /// @brief Whether playback loops at the clip's end.
        bool Loop = true;
        /// @brief Whether playback is advancing.
        bool Playing = true;
        /// @brief How the clip's baked root motion is handled.
        RootMotionMode RootMotion = RootMotionMode::Discard;
    };

    /// @brief Runtime-only skinning palette for a skinned-mesh entity.
    ///
    /// Holds the bone matrices the animation system computes and the renderer uploads into the GPU
    /// skinning palette. Never serialized (a derived product); added automatically to a skinned
    /// entity by the animation system. The system recomputes Skinning only on a frame whose pose
    /// inputs differ from the ones it was last computed from, and bumps Version when it does, so a
    /// reader comparing Version against the value it last saw knows whether the palette moved.
    struct SkinnedPose
    {
        /// @brief Per-bone skinning matrices (GlobalInverse * modelBone * InverseBind).
        vector<mat4> Skinning;
        /// @brief Incremented each time the animation system rewrites Skinning; 0 before the first.
        u64 Version = 0;
        /// @brief The animation system's encoding of the inputs Skinning was computed from.
        ///
        /// Opaque to every other reader. Clearing it forces the next update to recompute.
        vector<u64> InputKey;
    };

    /// @brief One procedural joint rotation: a joint named as authored, and the local turn it takes.
    struct JointOverride
    {
        /// @brief The joint (bone) name, matched exactly against the mesh's skeleton.
        string Joint;
        /// @brief Rotation post-multiplied onto the joint's local rotation, about its own axes.
        quat LocalRotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// @brief The resolution of one JointOverride name against a skeleton, cached on the component.
    struct ResolvedJointOverride
    {
        /// @brief The name this entry was resolved from.
        string Joint;
        /// @brief The joint's bone index, or -1 when the skeleton has no joint by that name.
        i32 Index = -1;
    };

    /// @brief Local rotations applied by joint name to a skinned mesh's pose, with or without a clip.
    ///
    /// Sits on an entity whose MeshRenderer draws a skinned mesh, and turns named joints — a
    /// swivelling mount, a spinning part, a head tracking a target — from code, without an
    /// animation clip. The animation system (the one writer of SkinnedPose) applies it:
    /// - **Without an Animator**, the pose is the skeleton's bind pose with each named joint's
    ///   rotation post-multiplied onto its bind rotation (Skeleton::ComputeLocalPose).
    /// - **With an Animator**, each rotation is post-multiplied onto the joint's sampled rotation
    ///   (single clip, blend or state), so a clip and a procedural turn compose.
    ///
    /// A joint turns about its own local axes and carries its whole subtree; joints above and
    /// beside it are untouched. Entries are applied in order, so two naming one joint compose.
    /// Names are resolved against the skeleton once and cached (Resolved); the cache re-resolves
    /// only when the skeleton or the set of names changes, so rewriting LocalRotation every frame
    /// costs no lookup. The pose is recomputed only when a resolved rotation's value differs from
    /// the one last posed, so leaving the entries alone and rewriting them with identical values
    /// are equally free. A name the skeleton lacks is ignored, with one warning per resolution.
    /// Clearing Entries returns a clip-less entity to its bind pose; removing the component leaves
    /// its last SkinnedPose standing. There are no limits and no blending: the caller clamps.
    struct JointOverrides
    {
        /// @brief The joint rotations, applied in order.
        vector<JointOverride> Entries;
        /// @brief Runtime cache of Entries' names resolved to bone indices; never authored.
        vector<ResolvedJointOverride> Resolved;
        /// @brief The skeleton Resolved was computed against; compared, never dereferenced.
        const Skeleton* ResolvedSkeleton = nullptr;
    };

    /// @brief This tick's root-motion displacement, published by an Animator in Drive mode.
    ///
    /// Written by the View-phase animation system each tick for an Animator whose RootMotion is
    /// Drive, in the entity's model-local frame (the character's own forward/right/up). A
    /// Sim-phase mover rotates it by the entity's orientation and adds it to the entity's
    /// position. Because the producer runs in the View phase after the Sim phase, a Sim consumer
    /// reads the value one tick late. Never serialized (a derived, per-frame product).
    struct RootMotionDelta
    {
        /// @brief Model-local translation extracted from the root bone this tick.
        vec3 Translation{0.0f};
    };
}

VE_ENUM(::Veng::RootMotionMode, 0x2F4A31CEE94569AFULL)
VE_ENUMERATOR(Discard)
VE_ENUMERATOR(Presentation)
VE_ENUMERATOR(Drive)
VE_ENUM_END();

VE_REFLECT(::Veng::Animator, 0x2B56DF7335B89F8DULL)
VE_FIELD(Clip, .DisplayName = "Clip")
VE_FIELD(Speed, .DisplayName = "Speed", .Display = {.Min = 0.0})
VE_FIELD(Loop, .DisplayName = "Loop")
VE_FIELD(Playing, .DisplayName = "Playing")
VE_FIELD(RootMotion, .DisplayName = "Root Motion")
VE_FIELD(Time, .DisplayName = "Time", .Display = {.Min = 0.0}, .ReadOnly = true)
VE_REFLECT_END();

VE_TYPE(::Veng::SkinnedPose, 0x063C1245B8912FC3ULL);

VE_TYPE(::Veng::RootMotionDelta, 0x10C7034D936A12CEULL);

VE_REFLECT(::Veng::JointOverride, 0x496BFCBF59AF8418ULL)
VE_FIELD(Joint, .DisplayName = "Joint", .Tooltip = "Joint (bone) name in the mesh's skeleton")
VE_FIELD(LocalRotation, .DisplayName = "Local Rotation",
         .Tooltip = "Applied after the joint's own local rotation")
VE_REFLECT_END();

VE_REFLECT(::Veng::JointOverrides, 0xE7C932E50B8041B4ULL)
VE_ARRAY_FIELD(Entries, .DisplayName = "Entries")
VE_REFLECT_END();
