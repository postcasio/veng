#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/ParticleEffect.h>

namespace Veng
{
    /// @brief AssetTypes::ParticleEffect loader.
    ///
    /// Decodes a CookedParticleEffectHeader, its emitters, flashes, flipbook pool and key pool into
    /// the effect's descriptions, and names every flipbook a billboard emitter picks from as a hard
    /// dependency (once per distinct flipbook), so every flipbook an effect draws is resident before
    /// any instance plays it. The effect holds no GPU resource of its own, so nothing is finalized.
    class ParticleEffectLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::ParticleEffect.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::ParticleEffect; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked effect, naming its flipbooks as dependencies.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
