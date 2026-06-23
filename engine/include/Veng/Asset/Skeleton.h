#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

namespace Veng
{
    /// @brief One bone in a skeleton: its parent, inverse-bind matrix, and local bind pose.
    ///
    /// Bones are stored in topological order (every bone precedes its children). The
    /// inverse-bind matrix maps a vertex from mesh space into this bone's space at bind pose;
    /// the local bind transform is the bone's pose relative to its parent, used when an
    /// animation has no track for the bone.
    struct Bone
    {
        /// @brief Index of the parent bone, or -1 for a root.
        i32 Parent = -1;
        /// @brief Bone name; the key animation channels are matched against at cook time.
        string Name;
        /// @brief Inverse bind-pose matrix (mesh space → bone space).
        mat4 InverseBind{1.0f};
        /// @brief Local bind-pose translation in parent space.
        vec3 LocalPosition{0.0f};
        /// @brief Local bind-pose rotation in parent space.
        quat LocalRotation{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Local bind-pose scale in parent space.
        vec3 LocalScale{1.0f};
    };

    /// @brief A bone hierarchy with inverse-bind matrices, loaded by AssetId.
    ///
    /// A CPU-only asset (no GPU resource): a skinned Mesh references one, and the animation
    /// system poses it each frame into the GPU skinning palette. Bones are in topological
    /// order; GlobalInverse folds the model's root transform out of the skin formula
    /// skin(bone) = GlobalInverse * modelBone(bone) * InverseBind(bone).
    struct Skeleton
    {
        /// @brief Bones in topological (parent-before-child) order.
        vector<Bone> Bones;
        /// @brief Inverse of the source model's root transform; folded into the skin formula.
        mat4 GlobalInverse{1.0f};

        /// @brief Returns the number of bones.
        [[nodiscard]] usize GetBoneCount() const { return Bones.size(); }

        /// @brief Returns a bone's local bind-pose transform as a matrix.
        [[nodiscard]] mat4 BindLocalMatrix(usize bone) const;

        /// @brief Composes the skinning palette from per-bone local pose matrices.
        ///
        /// out[b] = GlobalInverse * modelBone(b) * InverseBind(b), where modelBone composes the
        /// localPose matrices down the parent chain. The vertex shader multiplies a vertex by the
        /// weighted sum of its bones' palette matrices.
        /// @param localPose  Per-bone local transform matrices, one per bone (size GetBoneCount()).
        /// @param out        Receives GetBoneCount() skinning matrices.
        void ComputeSkinningMatrices(std::span<const mat4> localPose, vector<mat4>& out) const;

        /// @brief Composes the skinning palette for the bind pose (every bone at its bind local).
        ///
        /// The pose a skinned mesh shows with no animation; used by the renderer when an entity
        /// has no computed pose (e.g. in the editor with systems paused).
        /// @param out  Receives GetBoneCount() skinning matrices.
        void ComputeBindPoseMatrices(vector<mat4>& out) const;
    };

    /// @brief AssetTypeTrait specialization mapping Skeleton to AssetType::Skeleton.
    template <>
    struct AssetTypeTrait<Skeleton>
    {
        /// @brief The asset type tag for Skeleton.
        static constexpr AssetType Type = AssetType::Skeleton;
    };
}
