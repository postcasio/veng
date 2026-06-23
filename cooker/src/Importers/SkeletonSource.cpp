#include "SkeletonSource.h"

#include <fmt/format.h>

#include <assimp/scene.h>

namespace Veng::Cook
{
    namespace
    {
        // DFS-emits one ImportedBone per node, parent-before-child, recording each node's
        // index by name and its decomposed local transform. The bone's inverse-bind matrix
        // is the collected aiBone offset for skin bones, identity otherwise.
        void EmitNode(const aiNode* node, int parentIndex,
                      const std::unordered_map<std::string, aiMatrix4x4>& offsets,
                      ImportedSkeleton& skeleton)
        {
            const int index = static_cast<int>(skeleton.Bones.size());

            ImportedBone bone;
            bone.Name = node->mName.C_Str();
            bone.Parent = parentIndex;

            node->mTransformation.Decompose(bone.LocalScale, bone.LocalRotation,
                                            bone.LocalPosition);

            const auto offset = offsets.find(bone.Name);
            if (offset != offsets.end())
            {
                bone.Offset = offset->second;
            }

            skeleton.Bones.push_back(bone);
            skeleton.NameToIndex[skeleton.Bones[index].Name] = index;

            for (unsigned int i = 0; i < node->mNumChildren; ++i)
            {
                EmitNode(node->mChildren[i], index, offsets, skeleton);
            }
        }
    }

    Result<ImportedSkeleton> BuildImportedSkeleton(const aiScene* scene)
    {
        if (scene == nullptr || scene->mRootNode == nullptr)
        {
            return std::unexpected("skeleton source: assimp scene has no root node");
        }

        // Collect every mesh bone's inverse-bind (offset) matrix, keyed by bone name.
        std::unordered_map<std::string, aiMatrix4x4> offsets;
        bool hasBones = false;
        for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
        {
            const aiMesh* mesh = scene->mMeshes[m];
            for (unsigned int b = 0; b < mesh->mNumBones; ++b)
            {
                const aiBone* bone = mesh->mBones[b];
                offsets[bone->mName.C_Str()] = bone->mOffsetMatrix;
                hasBones = true;
            }
        }

        ImportedSkeleton skeleton;
        skeleton.HasSkinningBones = hasBones;
        skeleton.GlobalInverse = scene->mRootNode->mTransformation;
        skeleton.GlobalInverse.Inverse();

        EmitNode(scene->mRootNode, -1, offsets, skeleton);

        if (skeleton.Bones.empty())
        {
            return std::unexpected("skeleton source: scene node hierarchy is empty");
        }

        return skeleton;
    }
}
