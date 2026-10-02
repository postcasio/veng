#pragma once

#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/Environment.h>

namespace Veng
{
    /// @brief AssetTypes::Environment loader.
    ///
    /// Decodes a CookedEnvironmentHeader + HDR panorama pixels into a Veng::EnvironmentMap.
    /// Image creation and upload are worker-legal; bindless registration is deferred to the
    /// main-thread Finalize so the handle is assigned on the correct thread.
    class EnvironmentLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::Environment.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::Environment; }

        /// @brief Returns true: the decode, image creation and upload submit run on a worker.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes the cooked environment map into its image, whose Finalize registers it.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
