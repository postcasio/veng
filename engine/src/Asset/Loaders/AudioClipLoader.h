#pragma once

#include <Veng/Asset/AssetLoader.h>

namespace Veng
{
    /// @brief Loads a CookedAudioHeader blob into a CPU-only Audio::AudioClip asset.
    ///
    /// No GPU resource and no dependencies: a Pcm clip's samples become a resident AudioBuffer, and
    /// an Encoded clip keeps its Vorbis bytes for incremental decode at play time. The decode is CPU
    /// work off the audio callback thread; the clip is handed to the device as a mixer-ready buffer.
    class AudioClipLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::AudioClip.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::AudioClip; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes a cooked audio-clip blob into a Ref<Audio::AudioClip>.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
