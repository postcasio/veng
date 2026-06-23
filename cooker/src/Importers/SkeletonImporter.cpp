#include "SkeletonImporter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Veng/Asset/CookedBlobs.h>

#include "SkeletonSource.h"

namespace Veng::Cook
{
    namespace
    {
        void SetBoneName(char (&dest)[ShaderNameCapacity], const std::string& name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }
    }

    Result<vector<u8>> SkeletonImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("skeleton importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const std::ifstream sourceFile(sourcePath, std::ios::binary);
        if (!sourceFile)
        {
            return std::unexpected(
                fmt::format("skeleton importer: failed to open '{}'", sourcePath.string()));
        }

        std::ostringstream contentStream;
        contentStream << sourceFile.rdbuf();
        const json skeletonJson = json::parse(contentStream.str(), nullptr, false);
        if (skeletonJson.is_discarded() || !skeletonJson.is_object())
        {
            return std::unexpected(
                fmt::format("skeleton importer: '{}': invalid JSON", sourcePath.string()));
        }

        if (!skeletonJson.contains("model") || !skeletonJson["model"].is_string())
        {
            return std::unexpected(fmt::format(
                "skeleton importer: '{}': missing or invalid 'model'", sourcePath.string()));
        }

        const path modelPath = sourcePath.parent_path() / skeletonJson["model"].get<string>();
        context.RecordDependency(modelPath);

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(modelPath.string(), 0);
        if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 ||
            scene->mRootNode == nullptr)
        {
            return std::unexpected(
                fmt::format("skeleton importer: '{}': assimp failed to import '{}': {}",
                            sourcePath.string(), modelPath.string(), importer.GetErrorString()));
        }

        const Result<ImportedSkeleton> imported = BuildImportedSkeleton(scene);
        if (!imported)
        {
            return std::unexpected(
                fmt::format("skeleton importer: '{}': {}", sourcePath.string(), imported.error()));
        }

        if (!imported->HasSkinningBones)
        {
            return std::unexpected(
                fmt::format("skeleton importer: '{}': model '{}' has no skinning bones",
                            sourcePath.string(), modelPath.string()));
        }

        CookedSkeletonHeader header{};
        header.Version = CookedSkeletonVersion;
        header.BoneCount = static_cast<u32>(imported->Bones.size());
        ToColumnMajor(imported->GlobalInverse, header.GlobalInverse);

        vector<CookedBone> bones(imported->Bones.size());
        for (usize i = 0; i < imported->Bones.size(); ++i)
        {
            const ImportedBone& src = imported->Bones[i];
            CookedBone& dst = bones[i];
            dst.Parent = src.Parent;
            SetBoneName(dst.Name, src.Name);
            ToColumnMajor(src.Offset, dst.InverseBind);
            dst.LocalPosition[0] = src.LocalPosition.x;
            dst.LocalPosition[1] = src.LocalPosition.y;
            dst.LocalPosition[2] = src.LocalPosition.z;
            ToXyzw(src.LocalRotation, dst.LocalRotation);
            dst.LocalScale[0] = src.LocalScale.x;
            dst.LocalScale[1] = src.LocalScale.y;
            dst.LocalScale[2] = src.LocalScale.z;
        }

        const usize boneBytes = bones.size() * sizeof(CookedBone);
        vector<u8> blob(sizeof(CookedSkeletonHeader) + boneBytes);
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), bones.data(), boneBytes);

        return blob;
    }
}
