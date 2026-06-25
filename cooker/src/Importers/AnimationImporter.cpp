#include "AnimationImporter.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include <fmt/format.h>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/CookedBlobs.h>

#include "SkeletonSource.h"

namespace Veng::Cook
{
    namespace
    {
        // Clamp-and-lerp sample of a vec3 track, matching the runtime SampleVec3. Used only to
        // synthesize a boundary key when trimming a track that has no key exactly at the cut.
        vec3 SampleVec3At(const vector<Vec3Key>& keys, f32 t)
        {
            if (keys.empty())
            {
                return vec3(0.0f);
            }
            if (t <= keys.front().Time)
            {
                return keys.front().Value;
            }
            if (t >= keys.back().Time)
            {
                return keys.back().Value;
            }
            for (usize i = 0; i + 1 < keys.size(); ++i)
            {
                if (t < keys[i + 1].Time)
                {
                    const f32 span = keys[i + 1].Time - keys[i].Time;
                    const f32 alpha = span > 0.0f ? (t - keys[i].Time) / span : 0.0f;
                    return glm::mix(keys[i].Value, keys[i + 1].Value, alpha);
                }
            }
            return keys.back().Value;
        }

        // Clamp-and-slerp sample of a rotation track, matching the runtime SampleQuat.
        quat SampleQuatAt(const vector<QuatKey>& keys, f32 t)
        {
            if (keys.empty())
            {
                return quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            if (t <= keys.front().Time)
            {
                return keys.front().Value;
            }
            if (t >= keys.back().Time)
            {
                return keys.back().Value;
            }
            for (usize i = 0; i + 1 < keys.size(); ++i)
            {
                if (t < keys[i + 1].Time)
                {
                    const f32 span = keys[i + 1].Time - keys[i].Time;
                    const f32 alpha = span > 0.0f ? (t - keys[i].Time) / span : 0.0f;
                    return glm::slerp(keys[i].Value, keys[i + 1].Value, alpha);
                }
            }
            return keys.back().Value;
        }

        // Restricts a track to [startTime, endTime] and re-bases it to start at 0, preserving the
        // sampled motion across the window: a key at each boundary is synthesized when the track
        // has none there, so a constant or sparsely-keyed track keeps its value.
        void TrimVec3(vector<Vec3Key>& keys, f32 startTime, f32 endTime)
        {
            if (keys.empty())
            {
                return;
            }
            constexpr f32 Epsilon = 1e-6f;
            vector<Vec3Key> trimmed;
            bool hasStart = false;
            bool hasEnd = false;
            for (const Vec3Key& key : keys)
            {
                if (key.Time >= startTime - Epsilon && key.Time <= endTime + Epsilon)
                {
                    trimmed.push_back(key);
                    hasStart = hasStart || std::fabs(key.Time - startTime) <= Epsilon;
                    hasEnd = hasEnd || std::fabs(key.Time - endTime) <= Epsilon;
                }
            }
            if (!hasStart)
            {
                const Vec3Key boundary{.Time = startTime, .Value = SampleVec3At(keys, startTime)};
                trimmed.insert(trimmed.begin(), boundary);
            }
            if (!hasEnd)
            {
                const Vec3Key boundary{.Time = endTime, .Value = SampleVec3At(keys, endTime)};
                trimmed.push_back(boundary);
            }
            for (Vec3Key& key : trimmed)
            {
                key.Time -= startTime;
            }
            keys.swap(trimmed);
        }

        // The rotation-track sibling of TrimVec3.
        void TrimQuat(vector<QuatKey>& keys, f32 startTime, f32 endTime)
        {
            if (keys.empty())
            {
                return;
            }
            constexpr f32 Epsilon = 1e-6f;
            vector<QuatKey> trimmed;
            bool hasStart = false;
            bool hasEnd = false;
            for (const QuatKey& key : keys)
            {
                if (key.Time >= startTime - Epsilon && key.Time <= endTime + Epsilon)
                {
                    trimmed.push_back(key);
                    hasStart = hasStart || std::fabs(key.Time - startTime) <= Epsilon;
                    hasEnd = hasEnd || std::fabs(key.Time - endTime) <= Epsilon;
                }
            }
            if (!hasStart)
            {
                const QuatKey boundary{.Time = startTime, .Value = SampleQuatAt(keys, startTime)};
                trimmed.insert(trimmed.begin(), boundary);
            }
            if (!hasEnd)
            {
                const QuatKey boundary{.Time = endTime, .Value = SampleQuatAt(keys, endTime)};
                trimmed.push_back(boundary);
            }
            for (QuatKey& key : trimmed)
            {
                key.Time -= startTime;
            }
            keys.swap(trimmed);
        }
    }

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
        // Collapse FBX pivots so each bone is a single node — see MeshImporter. All three
        // importers must set this identically to keep one canonical bone order.
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
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

        // Optional lead-in/out trim, counted in whole keyframes of the clip's densest track. Some
        // mocap exports (RenderPeople among them) bake a neutral bind-pose frame at time 0; left
        // in, a looped clip snaps to that pose for one frame on every wrap. Trimming drops those
        // frames and re-bases the timeline so the loop is seamless.
        const u32 trimStart =
            animJson.contains("trimStart") && animJson["trimStart"].is_number_unsigned()
                ? animJson["trimStart"].get<u32>()
                : 0u;
        const u32 trimEnd = animJson.contains("trimEnd") && animJson["trimEnd"].is_number_unsigned()
                                ? animJson["trimEnd"].get<u32>()
                                : 0u;

        // Decode every in-skeleton channel's tracks into seconds-keyed lists up front, so the
        // optional trim resamples them before they are serialized into the key region.
        struct ChannelTracks
        {
            u32 BoneIndex = 0;
            vector<Vec3Key> Position;
            vector<QuatKey> Rotation;
            vector<Vec3Key> Scale;
        };

        vector<ChannelTracks> tracks;
        vector<f32> referenceTimes;

        for (unsigned int c = 0; c < anim->mNumChannels; ++c)
        {
            const aiNodeAnim* channel = anim->mChannels[c];
            const auto bone = imported->NameToIndex.find(channel->mNodeName.C_Str());
            if (bone == imported->NameToIndex.end())
            {
                // A channel targeting a node outside the skeleton (e.g. a camera) is skipped.
                continue;
            }

            ChannelTracks record;
            record.BoneIndex = static_cast<u32>(bone->second);

            record.Position.resize(channel->mNumPositionKeys);
            for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k)
            {
                const aiVector3D v = channel->mPositionKeys[k].mValue;
                record.Position[k] = Vec3Key{.Time = toSeconds(channel->mPositionKeys[k].mTime),
                                             .Value = vec3(v.x, v.y, v.z)};
            }
            record.Rotation.resize(channel->mNumRotationKeys);
            for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k)
            {
                const aiQuaternion q = channel->mRotationKeys[k].mValue;
                record.Rotation[k] = QuatKey{.Time = toSeconds(channel->mRotationKeys[k].mTime),
                                             .Value = quat(q.w, q.x, q.y, q.z)};
            }
            record.Scale.resize(channel->mNumScalingKeys);
            for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k)
            {
                const aiVector3D v = channel->mScalingKeys[k].mValue;
                record.Scale[k] = Vec3Key{.Time = toSeconds(channel->mScalingKeys[k].mTime),
                                          .Value = vec3(v.x, v.y, v.z)};
            }

            // The trim's frame grid is the longest track seen — a per-frame-keyed bone, not a
            // constant one whose value is held by a single key.
            const auto consider = [&](const auto& keys)
            {
                if (keys.size() > referenceTimes.size())
                {
                    referenceTimes.clear();
                    referenceTimes.reserve(keys.size());
                    for (const auto& key : keys)
                    {
                        referenceTimes.push_back(key.Time);
                    }
                }
            };
            consider(record.Position);
            consider(record.Rotation);
            consider(record.Scale);

            tracks.push_back(std::move(record));
        }

        if (tracks.empty())
        {
            return std::unexpected(
                fmt::format("animation importer: '{}': clip {} animates no skeleton bones",
                            sourcePath.string(), clipIndex));
        }

        f32 clipDuration = toSeconds(anim->mDuration);

        if (trimStart != 0 || trimEnd != 0)
        {
            const usize frameCount = referenceTimes.size();
            if (static_cast<usize>(trimStart) + trimEnd + 1 > frameCount)
            {
                return std::unexpected(fmt::format(
                    "animation importer: '{}': trimStart {} + trimEnd {} leaves no frames of {}",
                    sourcePath.string(), trimStart, trimEnd, frameCount));
            }
            const f32 startTime = referenceTimes[trimStart];
            const f32 endTime = referenceTimes[frameCount - 1 - trimEnd];

            for (ChannelTracks& record : tracks)
            {
                TrimVec3(record.Position, startTime, endTime);
                TrimQuat(record.Rotation, startTime, endTime);
                TrimVec3(record.Scale, startTime, endTime);
            }
            clipDuration = endTime - startTime;
        }

        // Serialize the (possibly trimmed) tracks: each channel's position, rotation, then scale
        // keys are appended to the key region and the channel records their byte offsets within it.
        vector<CookedAnimChannel> channels;
        vector<u8> keyRegion;

        const auto appendVec3Keys = [&](const vector<Vec3Key>& keys) -> u32
        {
            const u32 offset = static_cast<u32>(keyRegion.size());
            for (const Vec3Key& key : keys)
            {
                CookedVec3Key out{};
                out.Time = key.Time;
                out.Value[0] = key.Value.x;
                out.Value[1] = key.Value.y;
                out.Value[2] = key.Value.z;
                const usize at = keyRegion.size();
                keyRegion.resize(at + sizeof(CookedVec3Key));
                std::memcpy(keyRegion.data() + at, &out, sizeof(out));
            }
            return offset;
        };

        const auto appendQuatKeys = [&](const vector<QuatKey>& keys) -> u32
        {
            const u32 offset = static_cast<u32>(keyRegion.size());
            for (const QuatKey& key : keys)
            {
                CookedQuatKey out{};
                out.Time = key.Time;
                out.Value[0] = key.Value.x;
                out.Value[1] = key.Value.y;
                out.Value[2] = key.Value.z;
                out.Value[3] = key.Value.w;
                const usize at = keyRegion.size();
                keyRegion.resize(at + sizeof(CookedQuatKey));
                std::memcpy(keyRegion.data() + at, &out, sizeof(out));
            }
            return offset;
        };

        for (const ChannelTracks& record : tracks)
        {
            CookedAnimChannel out{};
            out.BoneIndex = record.BoneIndex;
            out.PositionKeyCount = static_cast<u32>(record.Position.size());
            out.PositionKeyOffset = appendVec3Keys(record.Position);
            out.RotationKeyCount = static_cast<u32>(record.Rotation.size());
            out.RotationKeyOffset = appendQuatKeys(record.Rotation);
            out.ScaleKeyCount = static_cast<u32>(record.Scale.size());
            out.ScaleKeyOffset = appendVec3Keys(record.Scale);
            channels.push_back(out);
        }

        CookedAnimationHeader header{};
        header.Version = CookedAnimationVersion;
        header.ChannelCount = static_cast<u32>(channels.size());
        header.KeyRegionBytes = static_cast<u32>(keyRegion.size());
        header.Duration = clipDuration;

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
