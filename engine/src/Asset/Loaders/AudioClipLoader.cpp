#include "AudioClipLoader.h"

#include <Veng/Audio/AudioClip.h>

namespace Veng
{
    AssetResult<Detail::LoadJob>
    AudioClipLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                          TaskSystem& /*tasks*/, TypeRegistry& /*types*/, const AssetId id,
                          const std::span<const u8> cooked, bool /*async*/) const
    {
        Result<Ref<Audio::AudioClip>> clip = Audio::AudioClip::Decode(cooked);
        if (!clip)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(clip.error())});
        }
        return Detail::LoadJob{.Resource = Detail::RefAny(*clip)};
    }
}
