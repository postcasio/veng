#pragma once

#include <Veng/Asset/AssetLoader.h>

namespace Veng
{
    /// @brief Loads a CookedRumbleClipHeader blob into a CPU-only Haptics::RumbleClip asset.
    ///
    /// No GPU resource and no dependencies: the clip record is decoded through the shared
    /// reflection reader into a Ref<Haptics::RumbleClip>.
    class RumbleClipLoader final : public AssetLoader
    {
    public:
        /// @brief Returns AssetTypes::RumbleClip.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::RumbleClip; }

        /// @brief Returns true: the decode runs on a worker for an asynchronous load.
        [[nodiscard]] bool ParsesOffThread() const override { return true; }

        /// @brief Decodes a cooked rumble-clip blob into a Ref<Haptics::RumbleClip>.
        [[nodiscard]] AssetResult<Detail::ParsedAsset>
        Parse(const AssetParseContext& context, AssetId id,
              std::span<const u8> cooked) const override;
    };
}
