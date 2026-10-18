#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a particle-effect source (`*.particles.json`) into a CookedParticleEffectHeader,
    ///        its emitters, its flashes and their key pool.
    ///
    /// The pack entry's "source" names the effect file itself. The format, every default and every
    /// refusal are documented in `cooker/PARTICLE_EFFECT_SCHEMA.md`. A billboard emitter's
    /// `flipbook` must resolve, through the cook's reference packs or its own, to a Flipbook; each
    /// refusal is a located error naming the file and the field. An effect whose particles can
    /// outnumber the renderer's default capacity at once cooks with a warning.
    class ParticleEffectImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::ParticleEffect.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::ParticleEffect; }

        /// @brief Runs concurrently: the work is a JSON parse and id resolution, both worker-safe.
        [[nodiscard]] ImporterConcurrency Concurrency() const override
        {
            return ImporterConcurrency::Parallel;
        }

        /// @brief Cooks the effect described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
