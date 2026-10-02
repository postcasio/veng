#include "AudioClipLoader.h"

#include <Veng/Audio/AudioClip.h>

namespace Veng
{
    AssetResult<Detail::ParsedAsset> AudioClipLoader::Parse(const AssetParseContext& /*context*/,
                                                            const AssetId id,
                                                            const std::span<const u8> cooked) const
    {
        Result<Ref<Audio::AudioClip>> clip = Audio::AudioClip::Decode(cooked);
        if (!clip)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(clip.error())});
        }
        return Detail::ParsedJob(Detail::LoadJob{.Resource = Detail::RefAny(*clip)});
    }
}
