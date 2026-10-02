#pragma once

#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Result.h>
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

    /// @brief A local rotation applied to one joint on top of the rotation its pose already carries.
    ///
    /// The resolved (by index) form of a procedural joint override: the joint's local rotation
    /// becomes Rotation(pose) * Rotation, turning the joint — and so every descendant — about its
    /// own local axes, before its local scale.
    struct JointRotation
    {
        /// @brief Index of the joint (bone) in the skeleton's topological bone order.
        usize Joint = 0;
        /// @brief Rotation post-multiplied onto the joint's local rotation.
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// @brief Composes a bone's local transform from its translation, rotation and scale.
    ///
    /// Equal to translate(translation) * mat4(rotation) * scale(scale), written directly: the
    /// rotation's columns scaled per axis, the translation in column 3, and an affine bottom row.
    /// @param translation  Local translation in parent space.
    /// @param rotation     Local rotation in parent space.
    /// @param scale        Local scale, applied before the rotation.
    /// @return The bone's local transform.
    [[nodiscard]] mat4 ComposeBoneTransform(const vec3& translation, const quat& rotation,
                                            const vec3& scale);

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

        /// @brief Returns the index of the bone with the given name, or -1 when none carries it.
        ///
        /// A linear scan comparing names exactly; a caller resolving the same name every frame
        /// caches the index.
        /// @param name  The bone name, as authored.
        /// @return The bone's index in Bones, or -1.
        [[nodiscard]] i32 FindBone(std::string_view name) const;

        /// @brief Writes every bone's local bind-pose matrix, the pose the skeleton rests in.
        /// @param out  Receives GetBoneCount() local matrices (BindLocalMatrix of each bone).
        void ComputeBindLocalPose(vector<mat4>& out) const;

        /// @brief Writes the bind pose with procedural joint rotations applied, as local matrices.
        ///
        /// Each bone takes its bind local transform; a bone named by a rotation instead takes
        /// translate(LocalPosition) * mat4(LocalRotation * rotation) * scale(LocalScale), so the
        /// joint turns about its own local axes and carries its whole subtree with it. The result
        /// is the localPose ComputeSkinningMatrices and JointModelTransform take, and is exactly
        /// what the animation system poses a JointOverrides entity with no Animator from. Two
        /// rotations naming one joint compose in order; an out-of-range joint is ignored.
        /// @param rotations  The joint rotations to apply.
        /// @param out        Receives GetBoneCount() local matrices.
        void ComputeLocalPose(std::span<const JointRotation> rotations, vector<mat4>& out) const;

        /// @brief Returns a joint's frame in mesh space for a pose.
        ///
        /// GlobalInverse * modelBone(joint), where modelBone composes the local pose down the
        /// joint's parent chain — the same composition ComputeSkinningMatrices performs, so
        /// JointModelTransform(pose, j) * Bones[j].InverseBind equals that pose's skinning matrix
        /// for j. Mesh space is the frame the mesh's vertices and sockets are authored in, so the
        /// result places the joint relative to the entity drawing the mesh; at the bind pose it is
        /// inverse(Bones[j].InverseBind). A bone missing from localPose takes its bind local.
        /// @param localPose  Per-bone local transform matrices (as ComputeSkinningMatrices takes).
        /// @param joint      The joint's bone index.
        /// @pre joint < GetBoneCount().
        /// @return The joint's transform in mesh space; its column 3 is the joint's position.
        [[nodiscard]] mat4 JointModelTransform(std::span<const mat4> localPose, usize joint) const;

        /// @brief Returns a joint's frame in world space, given the drawing entity's world matrix.
        ///
        /// entityWorld * JointModelTransform(localPose, joint): the entity's world matrix places
        /// the mesh, so this is where the posed joint is drawn.
        /// @param entityWorld  The world matrix of the entity drawing the mesh (WorldMatrix).
        /// @param localPose    Per-bone local transform matrices.
        /// @param joint        The joint's bone index.
        /// @pre joint < GetBoneCount().
        /// @return The joint's transform in world space.
        [[nodiscard]] mat4 JointWorldTransform(const mat4& entityWorld,
                                               std::span<const mat4> localPose, usize joint) const;

        /// @brief Composes the skinning palette from per-bone local pose matrices.
        ///
        /// out[b] = GlobalInverse * modelBone(b) * InverseBind(b), where modelBone composes the
        /// localPose matrices down the parent chain. The vertex shader multiplies a vertex by the
        /// weighted sum of its bones' palette matrices.
        /// @param localPose  Per-bone local transform matrices, one per bone (size GetBoneCount()).
        /// @param out        Receives GetBoneCount() skinning matrices.
        void ComputeSkinningMatrices(std::span<const mat4> localPose, vector<mat4>& out) const;

        /// @brief Composes the skinning palette into out, using model as caller-held scratch.
        ///
        /// The same palette as the two-argument overload, for a caller posing many skeletons a
        /// frame that keeps one scratch buffer rather than allocating one per call. Every matrix
        /// involved — local poses, GlobalInverse, inverse binds — is taken to be affine.
        /// @param localPose  Per-bone local transform matrices (a missing bone takes its bind local).
        /// @param out        Receives GetBoneCount() skinning matrices.
        /// @param model      Scratch; resized to GetBoneCount() and overwritten.
        void ComputeSkinningMatrices(std::span<const mat4> localPose, vector<mat4>& out,
                                     vector<mat4>& model) const;

        /// @brief Composes the skinning palette for the bind pose (every bone at its bind local).
        ///
        /// The pose a skinned mesh shows with no animation; used by the renderer when an entity
        /// has no computed pose (e.g. in the editor with systems paused).
        /// @param out  Receives GetBoneCount() skinning matrices.
        void ComputeBindPoseMatrices(vector<mat4>& out) const;
    };

    /// @brief Decodes a cooked skeleton blob into a Skeleton.
    ///
    /// Validates the header's format version and that the blob holds the whole bone table, then
    /// decodes every bone. The skeleton loader calls this, so a resident Skeleton and a CPU read
    /// of the same blob (AssetManager::ReadSkeleton) cannot disagree. Pure CPU.
    /// @param cooked  The cooked AssetTypes::Skeleton blob, as the archive stores it (inflated).
    /// @return The skeleton, or an error naming what is malformed.
    [[nodiscard]] Result<Skeleton> ParseCookedSkeleton(std::span<const u8> cooked);

    /// @brief AssetTypeTrait specialization mapping Skeleton to AssetTypes::Skeleton.
    template <>
    struct AssetTypeTrait<Skeleton>
    {
        /// @brief The asset type tag for Skeleton.
        static constexpr AssetTypeId Type = AssetTypes::Skeleton;
    };
}
