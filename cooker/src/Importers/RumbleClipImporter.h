#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.rumble.json source into a CookedRumbleClipHeader plus a reflection record.
    ///
    /// The source is the reflected Haptics::RumbleClipData document — its `Duration`, `Loop` and the
    /// four channel curves `LowFrequency`, `HighFrequency`, `LeftTrigger` and `RightTrigger`, each a
    /// `{ "Keys": [{ "Time", "Value", "Interp" }] }` — bound strictly through the shared JSON walker.
    /// The importer then validates the clip through Haptics::CheckRumbleClip (a positive duration,
    /// a non-empty channel, sorted keys, times within the duration, values within [0, 1]) and fails
    /// the cook with the asset id and the offending key. It emits the record through libveng's
    /// WriteFields, so the cook and the runtime loader share one encoder, and needs no game module.
    class RumbleClipImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::RumbleClip.
        [[nodiscard]] AssetTypeId Type() const override { return AssetTypes::RumbleClip; }

        /// @brief Cooks the rumble clip described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
