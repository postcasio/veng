#include "SkeletonLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Skeleton.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        mat4 ReadMatrix(const f32 (&columnMajor)[16])
        {
            mat4 m{1.0f};
            std::memcpy(&m[0][0], columnMajor, sizeof(columnMajor));
            return m;
        }
    }

    AssetResult<Detail::LoadJob>
    SkeletonLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                         TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                         std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedSkeletonHeader))
        {
            return std::unexpected(
                Corrupt(id, "skeleton: cooked blob smaller than CookedSkeletonHeader"));
        }

        CookedSkeletonHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedSkeletonVersion)
        {
            return std::unexpected(Corrupt(id, fmt::format("skeleton: version {} != expected {}",
                                                           header.Version, CookedSkeletonVersion)));
        }

        const usize cursor = sizeof(CookedSkeletonHeader);
        const usize boneBytes = static_cast<usize>(header.BoneCount) * sizeof(CookedBone);
        if (cooked.size() < cursor + boneBytes)
        {
            return std::unexpected(Corrupt(id, "skeleton: cooked blob smaller than bone table"));
        }

        const Ref<Skeleton> skeleton = CreateRef<Skeleton>();
        skeleton->GlobalInverse = ReadMatrix(header.GlobalInverse);
        skeleton->Bones.resize(header.BoneCount);

        for (u32 i = 0; i < header.BoneCount; ++i)
        {
            CookedBone cooked_bone;
            std::memcpy(&cooked_bone, cooked.data() + cursor + i * sizeof(CookedBone),
                        sizeof(cooked_bone));

            Bone& bone = skeleton->Bones[i];
            bone.Parent = cooked_bone.Parent;
            bone.Name = string(cooked_bone.Name, strnlen(cooked_bone.Name, ShaderNameCapacity));
            bone.InverseBind = ReadMatrix(cooked_bone.InverseBind);
            bone.LocalPosition = vec3(cooked_bone.LocalPosition[0], cooked_bone.LocalPosition[1],
                                      cooked_bone.LocalPosition[2]);
            bone.LocalRotation = quat(cooked_bone.LocalRotation[3], cooked_bone.LocalRotation[0],
                                      cooked_bone.LocalRotation[1], cooked_bone.LocalRotation[2]);
            bone.LocalScale = vec3(cooked_bone.LocalScale[0], cooked_bone.LocalScale[1],
                                   cooked_bone.LocalScale[2]);
        }

        return Detail::LoadJob{.Resource = Detail::RefAny(skeleton)};
    }
}
