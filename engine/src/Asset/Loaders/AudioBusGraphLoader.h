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

        /// @brief Decodes the cooked graph blob into a LoadJob producing a resident AudioBusGraph.
        [[nodiscard]] AssetResult<Detail::LoadJob> Load(AssetManager& manager,
                                                        Renderer::Context& context,
                                                        TaskSystem& tasks, TypeRegistry& types,
                                                        AssetId id, std::span<const u8> cooked,
                                                        bool async) const override;
    };
}
