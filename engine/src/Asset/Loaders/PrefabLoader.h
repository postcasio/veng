#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Prefab.h>

namespace Veng
{
    /// @brief Decodes a cooked prefab blob's entity and component tables, reflecting nothing.
    ///
    /// Validates the header version and every table and record range, and returns each entity's
    /// nested-prefab id and component records verbatim. The prefab loader and the CPU-only prefab
    /// readers both decode through it, so neither can accept a blob the other rejects. Component
    /// records are not parsed: that needs a TypeRegistry, and the caller decides which it reads.
    /// @param cooked  The cooked AssetTypes::Prefab blob.
    /// @return The authored entities, in cooked order, or an error naming what is malformed.
    [[nodiscard]] Result<vector<Prefab::PrefabEntity>>
    DecodeCookedPrefab(std::span<const u8> cooked);

    /// @brief AssetTypes::Prefab loader.
    ///
    /// Decodes a CookedPrefabHeader + entity/component table + concatenated WriteFields
    /// records into a Veng::Prefab holding the decoded value tree. Embedded AssetHandle
    /// fields are surfaced as LoadJob dependencies so they finalize before the prefab
    /// and stay resident for its lifetime. The prefab carries no GPU resource; the
    /// finalize exists solely to order dependencies before the prefab becomes resident.
    class PrefabLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Prefab.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Prefab; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked prefab blob and reflects each component record to name the
        ///        assets its handle fields reference, which become the prefab's dependencies.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
