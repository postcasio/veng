#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    // Reads "source" (relative to CookContext::PackDir) verbatim into
    // AssetType::Raw blob bytes — the end-to-end proof for the cooker shell,
    // with zero type-specific logic.
    class RawImporter final : public AssetImporter
    {
    public:
        [[nodiscard]] AssetType Type() const override { return AssetType::Raw; }

        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context, const json& entry) const override;
    };
}
