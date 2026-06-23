#include "AnimationImporter.h"

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
    Result<vector<u8>> AnimationImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("animation importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const std::ifstream sourceFile(sourcePath, std::ios::binary);
        if (!sourceFile)
        {
            return std::unexpected(
                fmt::format("animation importer: failed to open '{}'", sourcePath.string()));
        }

        std::ostringstream contentStream;
        contentStream << sourceFile.rdbuf();
        const json animJson = json::parse(contentStream.str(), nullptr, false);
        if (animJson.is_discarded() || !animJson.is_object())
        {
            return std::unexpected(
                fmt::format("animation importer: '{}': invalid JSON", sourcePath.string()));
        }

        if (!animJson.contains("model") || !animJson["model"].is_string())
        {
            return std::unexpected(fmt::format(
                "animation importer: '{}': missing or invalid 'model'", sourcePath.string()));
        }

        const path modelPath = sourcePath.parent_path() / animJson["model"].get<string>();
        context.RecordDependency(modelPath);

        const u32 clipIndex = animJson.contains("clip") && animJson["clip"].is_number_unsigned()
                                  ? animJson["clip"].get<u32>()
                                  : 0u;

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(modelPath.string(), 0);
        if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 ||
            scene->mRootNode == nullptr)
        {
            return std::unexpected(
                fmt::format("animation importer: '{}': assimp failed to import '{}': {}",
                            sourcePath.string(), modelPath.string(), importer.GetErrorString()));
        }

        if (scene->mNumAnimations <= clipIndex)
        {
            return std::unexpected(fmt::format(
                "animation importer: '{}': model '{}' has {} animation(s), clip {} requested",
                sourcePath.string(), modelPath.string(), scene->mNumAnimations, clipIndex));
        }

        const Result<ImportedSkeleton> imported = BuildImportedSkeleton(scene);
        if (!imported)
        {
            return std::unexpected(
                fmt::format("animation importer: '{}': {}", sourcePath.string(), imported.error()));
        }

        const aiAnimation* anim = scene->mAnimations[clipIndex];
        const double ticksPerSecond = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 25.0;
        const auto toSeconds = [&](double ticks)
        { return static_cast<f32>(ticks / ticksPerSecond); };

        // Build the channel table and the contiguous key region in one pass: each channel's
        // position, rotation, then scale keys are appended to the key blob and the channel
        // records their byte offsets within it.
        vector<CookedAnimChannel> channels;
        vector<u8> keyRegion;

        const auto appendVec3Keys = [&](u32 count, auto&& valueAt, auto&& timeAt) -> u32
        {
            const u32 offset = static_cast<u32>(keyRegion.size());
            for (u32 k = 0; k < count; ++k)
            {
                CookedVec3Key key{};
                key.Time = toSeconds(timeAt(k));
                const aiVector3D v = valueAt(k);
                key.Value[0] = v.x;
                key.Value[1] = v.y;
                key.Value[2] = v.z;
                const usize at = keyRegion.size();
                keyRegion.resize(at + sizeof(CookedVec3Key));
                std::memcpy(keyRegion.data() + at, &key, sizeof(key));
            }
            return offset;
        };

        const auto appendQuatKeys = [&](u32 count, auto&& valueAt, auto&& timeAt) -> u32
        {
            const u32 offset = static_cast<u32>(keyRegion.size());
            for (u32 k = 0; k < count; ++k)
            {
                CookedQuatKey key{};
                key.Time = toSeconds(timeAt(k));
                const aiQuaternion q = valueAt(k);
                key.Value[0] = q.x;
                key.Value[1] = q.y;
                key.Value[2] = q.z;
                key.Value[3] = q.w;
                const usize at = keyRegion.size();
                keyRegion.resize(at + sizeof(CookedQuatKey));
                std::memcpy(keyRegion.data() + at, &key, sizeof(key));
            }
            return offset;
        };

        for (unsigned int c = 0; c < anim->mNumChannels; ++c)
        {
            const aiNodeAnim* channel = anim->mChannels[c];
            const auto bone = imported->NameToIndex.find(channel->mNodeName.C_Str());
            if (bone == imported->NameToIndex.end())
            {
                // A channel targeting a node outside the skeleton (e.g. a camera) is skipped.
                continue;
            }

            CookedAnimChannel record{};
            record.BoneIndex = static_cast<u32>(bone->second);

            record.PositionKeyCount = channel->mNumPositionKeys;
            record.PositionKeyOffset = appendVec3Keys(
                channel->mNumPositionKeys, [&](u32 k) { return channel->mPositionKeys[k].mValue; },
                [&](u32 k) { return channel->mPositionKeys[k].mTime; });

            record.RotationKeyCount = channel->mNumRotationKeys;
            record.RotationKeyOffset = appendQuatKeys(
                channel->mNumRotationKeys, [&](u32 k) { return channel->mRotationKeys[k].mValue; },
                [&](u32 k) { return channel->mRotationKeys[k].mTime; });

            record.ScaleKeyCount = channel->mNumScalingKeys;
            record.ScaleKeyOffset = appendVec3Keys(
                channel->mNumScalingKeys, [&](u32 k) { return channel->mScalingKeys[k].mValue; },
                [&](u32 k) { return channel->mScalingKeys[k].mTime; });

            channels.push_back(record);
        }

        if (channels.empty())
        {
            return std::unexpected(
                fmt::format("animation importer: '{}': clip {} animates no skeleton bones",
                            sourcePath.string(), clipIndex));
        }

        CookedAnimationHeader header{};
        header.Version = CookedAnimationVersion;
        header.ChannelCount = static_cast<u32>(channels.size());
        header.KeyRegionBytes = static_cast<u32>(keyRegion.size());
        header.Duration = toSeconds(anim->mDuration);

        const usize channelBytes = channels.size() * sizeof(CookedAnimChannel);
        vector<u8> blob(sizeof(CookedAnimationHeader) + channelBytes + keyRegion.size());
        usize cursor = 0;
        std::memcpy(blob.data() + cursor, &header, sizeof(header));
        cursor += sizeof(header);
        std::memcpy(blob.data() + cursor, channels.data(), channelBytes);
        cursor += channelBytes;
        std::memcpy(blob.data() + cursor, keyRegion.data(), keyRegion.size());

        return blob;
    }
}
