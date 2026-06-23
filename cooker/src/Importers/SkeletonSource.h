#pragma once

#include <string>
#include <unordered_map>

#include <assimp/matrix4x4.h>
#include <assimp/quaternion.h>
#include <assimp/vector3.h>

#include <Veng/Cook/Types.h>

struct aiScene;

namespace Veng::Cook
{
    /// @brief One bone in the canonical import order: a flattened assimp node.
    ///
    /// Every node in the scene's hierarchy becomes a bone so the parent chain composes
    /// exactly as assimp's global transform does; nodes that are not skin bones carry an
    /// identity Offset and are simply unused entries in the palette. The MeshImporter,
    /// SkeletonImporter, and AnimationImporter all derive their bone indices from this one
    /// ordering, so a vertex's bone index, the skeleton's bone array, and an animation's
    /// channel target all agree.
    struct ImportedBone
    {
        /// @brief Node/bone name (the key animation channels and mesh bones reference).
        std::string Name;
        /// @brief Index of the parent bone in the bone array, or -1 for a root.
        int Parent = -1;
        /// @brief Inverse bind matrix (aiBone::mOffsetMatrix), identity for a non-skin node.
        aiMatrix4x4 Offset;
        /// @brief Local bind-pose translation, decomposed from the node transform.
        aiVector3D LocalPosition;
        /// @brief Local bind-pose rotation, decomposed from the node transform.
        aiQuaternion LocalRotation;
        /// @brief Local bind-pose scale, decomposed from the node transform.
        aiVector3D LocalScale{1.0f, 1.0f, 1.0f};
    };

    /// @brief The canonical bone table flattened from an assimp scene.
    struct ImportedSkeleton
    {
        /// @brief Bones in topological (parent-before-child) DFS order.
        vector<ImportedBone> Bones;
        /// @brief Bone name → index in Bones.
        std::unordered_map<std::string, int> NameToIndex;
        /// @brief Inverse of the scene root node transform; folded into the runtime skin formula.
        aiMatrix4x4 GlobalInverse;
        /// @brief Whether any mesh in the scene carried skinning bones.
        bool HasSkinningBones = false;
    };

    /// @brief Flattens an assimp scene into the canonical bone table.
    ///
    /// Collects every mesh bone's inverse-bind (offset) matrix by name, then DFS-walks the
    /// node hierarchy from the root node (inclusive), emitting one ImportedBone per node in
    /// parent-before-child order. GlobalInverse is the inverse of the root node transform,
    /// so the runtime applies the canonical assimp skinning formula
    /// skin = GlobalInverse * global(bone) * offset.
    /// @param scene  The imported assimp scene (must be non-null with a root node).
    /// @return The canonical bone table, or an error string.
    Result<ImportedSkeleton> BuildImportedSkeleton(const aiScene* scene);

    /// @brief Writes an assimp (row-major) matrix into a column-major f32[16] (glm layout).
    inline void ToColumnMajor(const aiMatrix4x4& m, f32 (&out)[16])
    {
        for (int col = 0; col < 4; ++col)
        {
            for (int row = 0; row < 4; ++row)
            {
                out[col * 4 + row] = m[row][col];
            }
        }
    }

    /// @brief Writes an assimp quaternion (wxyz) into an f32[4] in xyzw order (glm layout).
    inline void ToXyzw(const aiQuaternion& q, f32 (&out)[4])
    {
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    }
}
