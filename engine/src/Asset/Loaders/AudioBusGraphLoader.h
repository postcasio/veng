#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Audio/AudioBusGraph.h>

namespace Veng
{
    /// @brief AssetTypes::AudioBusGraph loader.
    ///
    /// Decodes a CookedAudioBusGraphHeader + the tolerant WriteFields graph record into a
    /// Veng::Audio::AudioBusGraph. The graph carries no GPU resource and no dependencies; a
    /// stale/foreign or truncated blob surfaces as AssetError::Corrupt rather than a crash.
    class AudioBusGraphLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::AudioBusGraph.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::AudioBusGraph; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked graph blob into a resident AudioBusGraph.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
