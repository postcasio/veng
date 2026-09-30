#pragma once

#include <cstring>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Veng.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Skeleton.h>

namespace Veng::TestSupport
{
    /// @brief Encodes a skeleton as the cooked AssetTypes::Skeleton blob the cooker writes.
    inline vector<u8> CookSkeleton(const Skeleton& skeleton)
    {
        CookedSkeletonHeader header;
        header.Version = CookedSkeletonVersion;
        header.BoneCount = static_cast<u32>(skeleton.Bones.size());
        std::memcpy(header.GlobalInverse, &skeleton.GlobalInverse[0][0],
                    sizeof(header.GlobalInverse));

        vector<u8> blob(sizeof(header) + (skeleton.Bones.size() * sizeof(CookedBone)));
        std::memcpy(blob.data(), &header, sizeof(header));
        for (usize i = 0; i < skeleton.Bones.size(); ++i)
        {
            const Bone& bone = skeleton.Bones[i];
            CookedBone cooked;
            cooked.Parent = bone.Parent;
            std::strncpy(cooked.Name, bone.Name.c_str(), ShaderNameCapacity - 1);
            std::memcpy(cooked.InverseBind, &bone.InverseBind[0][0], sizeof(cooked.InverseBind));
            for (int c = 0; c < 3; ++c)
            {
                cooked.LocalPosition[c] = bone.LocalPosition[c];
                cooked.LocalScale[c] = bone.LocalScale[c];
            }
            cooked.LocalRotation[0] = bone.LocalRotation.x;
            cooked.LocalRotation[1] = bone.LocalRotation.y;
            cooked.LocalRotation[2] = bone.LocalRotation.z;
            cooked.LocalRotation[3] = bone.LocalRotation.w;
            std::memcpy(blob.data() + sizeof(header) + (i * sizeof(CookedBone)), &cooked,
                        sizeof(cooked));
        }
        return blob;
    }

    /// @brief A four-joint rig: Root → Arm → Hand (a leaf), and Root → Side, Arm's sibling.
    ///
    /// Every joint rests unrotated: Arm one unit up from Root, Hand one unit along +X from Arm,
    /// Side one unit along -X from Root. The root sits off the origin and the global inverse is a
    /// non-trivial translation, so a formula dropping either is caught. The joints' mesh-space
    /// rest positions are ArmRigRestPosition; each inverse bind is written by hand from them, so
    /// the bind pose skins to identity.
    inline vec3 ArmRigRestPosition(const usize joint)
    {
        const vec3 positions[] = {
            vec3(0.0f, -0.25f, 0.0f),
            vec3(0.0f, 0.75f, 0.0f),
            vec3(1.0f, 0.75f, 0.0f),
            vec3(-1.0f, -0.25f, 0.0f),
        };
        return positions[joint];
    }

    /// @brief Builds the rig ArmRigRestPosition describes.
    inline Skeleton MakeArmRig()
    {
        Skeleton skeleton;
        skeleton.GlobalInverse = glm::translate(mat4(1.0f), vec3(0.0f, -0.5f, 0.0f));
        const auto bone =
            [](const i32 parent, const char* name, const vec3& position, const usize joint)
        {
            return Bone{.Parent = parent,
                        .Name = name,
                        .InverseBind = glm::translate(mat4(1.0f), -ArmRigRestPosition(joint)),
                        .LocalPosition = position};
        };
        skeleton.Bones = {
            bone(-1, "Root", vec3(0.0f, 0.25f, 0.0f), 0),
            bone(0, "Arm", vec3(0.0f, 1.0f, 0.0f), 1),
            bone(1, "Hand", vec3(1.0f, 0.0f, 0.0f), 2),
            bone(0, "Side", vec3(-1.0f, 0.0f, 0.0f), 3),
        };
        return skeleton;
    }
}
