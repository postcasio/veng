#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks an audio-bus-graph source (a complete mixer topology) into the
    /// CookedAudioBusGraph blob.
    ///
    /// Binds the whole graph through the shared JsonReadFields walker against the reflected
    /// AudioBusGraphData descriptor, validates it with the engine's ValidateAudioBusGraph (single
    /// root Master, every parent resolves, acyclic, within the depth/count caps, no interned-hash
    /// collision), and emits the tolerant WriteFields record so the cooker and the runtime loader
    /// share one encoder. References only engine builtins, so it needs no --module.
    class AudioBusGraphImporter : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::AudioBusGraph.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::AudioBusGraph; }

        /// @brief Pure CPU over its own buffers with no shared state, so the cook may parallelize it.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the audio bus graph described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
