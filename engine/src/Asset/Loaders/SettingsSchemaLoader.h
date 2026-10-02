#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Settings/SettingsSchema.h>

namespace Veng
{
    /// @brief AssetTypes::SettingsSchema loader.
    ///
    /// Decodes a CookedSettingsSchemaHeader + the tolerant WriteFields schema record into a
    /// Veng::SettingsSchema. The schema carries no GPU resource and no dependencies; a
    /// stale/foreign or truncated blob surfaces as AssetError::Corrupt rather than a crash.
    class SettingsSchemaLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::SettingsSchema.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::SettingsSchema; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked schema blob into a resident SettingsSchema.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
