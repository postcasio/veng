#include "AnimationLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/CookedBlobs.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::LoadJob>
    AnimationLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                          TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                          std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedAnimationHeader))
        {
            return std::unexpected(
                Corrupt(id, "animation: cooked blob smaller than CookedAnimationHeader"));
        }

        CookedAnimationHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedAnimationVersion)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("animation: version {} != expected {}", header.Version,
                                        CookedAnimationVersion)));
        }

        const usize channelBytes =
            static_cast<usize>(header.ChannelCount) * sizeof(CookedAnimChannel);
        const usize keyRegionStart = sizeof(CookedAnimationHeader) + channelBytes;
        if (cooked.size() < keyRegionStart + header.KeyRegionBytes)
        {
            return std::unexpected(Corrupt(id, "animation: cooked blob smaller than key region"));
        }

        const u8* keyRegion = cooked.data() + keyRegionStart;

        const Ref<Animation> animation = CreateRef<Animation>();
        animation->Duration = header.Duration;
        animation->Channels.resize(header.ChannelCount);

        for (u32 c = 0; c < header.ChannelCount; ++c)
        {
            CookedAnimChannel cookedChannel;
            std::memcpy(&cookedChannel,
                        cooked.data() + sizeof(CookedAnimationHeader) +
                            c * sizeof(CookedAnimChannel),
                        sizeof(cookedChannel));

            AnimationChannel& channel = animation->Channels[c];
            channel.BoneIndex = cookedChannel.BoneIndex;

            const auto inRegion = [&](u32 offset, u32 count, usize keySize) -> bool
            {
                return static_cast<usize>(offset) + static_cast<usize>(count) * keySize <=
                       header.KeyRegionBytes;
            };

            if (!inRegion(cookedChannel.PositionKeyOffset, cookedChannel.PositionKeyCount,
                          sizeof(CookedVec3Key)) ||
                !inRegion(cookedChannel.RotationKeyOffset, cookedChannel.RotationKeyCount,
                          sizeof(CookedQuatKey)) ||
                !inRegion(cookedChannel.ScaleKeyOffset, cookedChannel.ScaleKeyCount,
                          sizeof(CookedVec3Key)))
            {
                return std::unexpected(Corrupt(id, "animation: channel key offset out of range"));
            }

            channel.Position.resize(cookedChannel.PositionKeyCount);
            for (u32 k = 0; k < cookedChannel.PositionKeyCount; ++k)
            {
                CookedVec3Key key;
                std::memcpy(&key,
                            keyRegion + cookedChannel.PositionKeyOffset + k * sizeof(CookedVec3Key),
                            sizeof(key));
                channel.Position[k] = Vec3Key{
                    .Time = key.Time, .Value = vec3(key.Value[0], key.Value[1], key.Value[2])};
            }

            channel.Rotation.resize(cookedChannel.RotationKeyCount);
            for (u32 k = 0; k < cookedChannel.RotationKeyCount; ++k)
            {
                CookedQuatKey key;
                std::memcpy(&key,
                            keyRegion + cookedChannel.RotationKeyOffset + k * sizeof(CookedQuatKey),
                            sizeof(key));
                channel.Rotation[k] =
                    QuatKey{.Time = key.Time,
                            .Value = quat(key.Value[3], key.Value[0], key.Value[1], key.Value[2])};
            }

            channel.Scale.resize(cookedChannel.ScaleKeyCount);
            for (u32 k = 0; k < cookedChannel.ScaleKeyCount; ++k)
            {
                CookedVec3Key key;
                std::memcpy(&key,
                            keyRegion + cookedChannel.ScaleKeyOffset + k * sizeof(CookedVec3Key),
                            sizeof(key));
                channel.Scale[k] = Vec3Key{.Time = key.Time,
                                           .Value = vec3(key.Value[0], key.Value[1], key.Value[2])};
            }
        }

        return Detail::LoadJob{.Resource = Detail::RefAny(animation)};
    }
}
