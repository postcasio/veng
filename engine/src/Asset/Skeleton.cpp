#include <Veng/Asset/Skeleton.h>

#include <cstring>

#include <fmt/format.h>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng
{
    namespace
    {
        mat4 ReadMatrix(const f32 (&columnMajor)[16])
        {
            mat4 m{1.0f};
            std::memcpy(&m[0][0], columnMajor, sizeof(columnMajor));
            return m;
        }

        // a * b for affine matrices (bottom row 0,0,0,1): the terms the bottom row zeroes are
        // skipped, so the result is exactly the full product's.
        mat4 AffineMultiply(const mat4& a, const mat4& b)
        {
            const vec4 a0 = a[0];
            const vec4 a1 = a[1];
            const vec4 a2 = a[2];
            return mat4((a0 * b[0].x) + (a1 * b[0].y) + (a2 * b[0].z),
                        (a0 * b[1].x) + (a1 * b[1].y) + (a2 * b[1].z),
                        (a0 * b[2].x) + (a1 * b[2].y) + (a2 * b[2].z),
                        (a0 * b[3].x) + (a1 * b[3].y) + (a2 * b[3].z) + a[3]);
        }
    }

    mat4 ComposeBoneTransform(const vec3& translation, const quat& rotation, const vec3& scale)
    {
        const mat3 basis = glm::mat3_cast(rotation);
        return mat4(vec4(basis[0] * scale.x, 0.0f), vec4(basis[1] * scale.y, 0.0f),
                    vec4(basis[2] * scale.z, 0.0f), vec4(translation, 1.0f));
    }

    mat4 Skeleton::BindLocalMatrix(const usize bone) const
    {
        const Bone& b = Bones[bone];
        return ComposeBoneTransform(b.LocalPosition, b.LocalRotation, b.LocalScale);
    }

    i32 Skeleton::FindBone(const std::string_view name) const
    {
        for (usize i = 0; i < Bones.size(); ++i)
        {
            if (Bones[i].Name == name)
            {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }

    void Skeleton::ComputeBindLocalPose(vector<mat4>& out) const
    {
        out.resize(Bones.size());
        for (usize i = 0; i < Bones.size(); ++i)
        {
            out[i] = BindLocalMatrix(i);
        }
    }

    void Skeleton::ComputeLocalPose(const std::span<const JointRotation> rotations,
                                    vector<mat4>& out) const
    {
        if (rotations.empty())
        {
            ComputeBindLocalPose(out);
            return;
        }

        vector<quat> rotation(Bones.size());
        for (usize i = 0; i < Bones.size(); ++i)
        {
            rotation[i] = Bones[i].LocalRotation;
        }
        for (const JointRotation& applied : rotations)
        {
            if (applied.Joint < Bones.size())
            {
                rotation[applied.Joint] = rotation[applied.Joint] * applied.Rotation;
            }
        }

        out.resize(Bones.size());
        for (usize i = 0; i < Bones.size(); ++i)
        {
            out[i] = ComposeBoneTransform(Bones[i].LocalPosition, rotation[i], Bones[i].LocalScale);
        }
    }

    mat4 Skeleton::JointModelTransform(const std::span<const mat4> localPose,
                                       const usize joint) const
    {
        VE_ASSERT(joint < Bones.size(), "Skeleton::JointModelTransform: joint {} out of range ({})",
                  joint, Bones.size());
        mat4 model(1.0f);
        for (i32 b = static_cast<i32>(joint); b >= 0; b = Bones[static_cast<usize>(b)].Parent)
        {
            const auto index = static_cast<usize>(b);
            model = (index < localPose.size() ? localPose[index] : BindLocalMatrix(index)) * model;
        }
        return GlobalInverse * model;
    }

    mat4 Skeleton::JointWorldTransform(const mat4& entityWorld,
                                       const std::span<const mat4> localPose,
                                       const usize joint) const
    {
        return entityWorld * JointModelTransform(localPose, joint);
    }

    void Skeleton::ComputeSkinningMatrices(std::span<const mat4> localPose, vector<mat4>& out) const
    {
        vector<mat4> model;
        ComputeSkinningMatrices(localPose, out, model);
    }

    void Skeleton::ComputeSkinningMatrices(std::span<const mat4> localPose, vector<mat4>& out,
                                           vector<mat4>& model) const
    {
        const usize count = Bones.size();
        out.resize(count);
        model.resize(count);

        // model[b] is GlobalInverse * modelBone(b): folding GlobalInverse into each root makes it
        // ride down the chain, so a bone pays one product for its chain and one for its inverse
        // bind. Bones are topological, so a parent's entry is always written before its children's.
        for (usize i = 0; i < count; ++i)
        {
            const mat4 local = i < localPose.size() ? localPose[i] : BindLocalMatrix(i);
            const i32 parent = Bones[i].Parent;
            model[i] = AffineMultiply(
                parent >= 0 ? model[static_cast<usize>(parent)] : GlobalInverse, local);
            out[i] = AffineMultiply(model[i], Bones[i].InverseBind);
        }
    }

    void Skeleton::ComputeBindPoseMatrices(vector<mat4>& out) const
    {
        vector<mat4> local;
        ComputeBindLocalPose(local);
        ComputeSkinningMatrices(local, out);
    }

    Result<Skeleton> ParseCookedSkeleton(const std::span<const u8> cooked)
    {
        if (cooked.size() < sizeof(CookedSkeletonHeader))
        {
            return std::unexpected(
                string("skeleton: cooked blob smaller than CookedSkeletonHeader"));
        }

        CookedSkeletonHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));
        if (header.Version != CookedSkeletonVersion)
        {
            return std::unexpected(fmt::format("skeleton: version {} != expected {}",
                                               header.Version, CookedSkeletonVersion));
        }

        const usize cursor = sizeof(CookedSkeletonHeader);
        const usize boneBytes = static_cast<usize>(header.BoneCount) * sizeof(CookedBone);
        if (cooked.size() < cursor + boneBytes)
        {
            return std::unexpected(string("skeleton: cooked blob smaller than bone table"));
        }

        Skeleton skeleton;
        skeleton.GlobalInverse = ReadMatrix(header.GlobalInverse);
        skeleton.Bones.resize(header.BoneCount);
        for (u32 i = 0; i < header.BoneCount; ++i)
        {
            CookedBone cookedBone;
            std::memcpy(&cookedBone, cooked.data() + cursor + (i * sizeof(CookedBone)),
                        sizeof(cookedBone));

            Bone& bone = skeleton.Bones[i];
            bone.Parent = cookedBone.Parent;
            bone.Name = string(cookedBone.Name, strnlen(cookedBone.Name, ShaderNameCapacity));
            bone.InverseBind = ReadMatrix(cookedBone.InverseBind);
            bone.LocalPosition = vec3(cookedBone.LocalPosition[0], cookedBone.LocalPosition[1],
                                      cookedBone.LocalPosition[2]);
            bone.LocalRotation = quat(cookedBone.LocalRotation[3], cookedBone.LocalRotation[0],
                                      cookedBone.LocalRotation[1], cookedBone.LocalRotation[2]);
            bone.LocalScale =
                vec3(cookedBone.LocalScale[0], cookedBone.LocalScale[1], cookedBone.LocalScale[2]);
        }
        return skeleton;
    }
}
