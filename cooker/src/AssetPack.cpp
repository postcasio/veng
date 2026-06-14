#include <Veng/Cook/AssetPack.h>

#include <algorithm>
#include <random>

namespace Veng::Cook
{
    const AssetPackEntry* AssetPack::FindById(AssetId id) const
    {
        const auto it = std::find_if(Entries.begin(), Entries.end(),
            [id](const AssetPackEntry& e) { return e.Id == id; });
        return it != Entries.end() ? &*it : nullptr;
    }

    AssetId GenerateAssetId(std::span<const AssetPack* const> packs)
    {
        std::random_device rd;
        std::mt19937_64 rng(rd());
        std::uniform_int_distribution<u64> dist(1, UINT64_MAX);

        while (true)
        {
            const u64 candidate = dist(rng);
            const AssetId id{.Value = candidate};

            bool collision = false;
            for (const AssetPack* pack : packs)
            {
                if (pack && pack->FindById(id) != nullptr)
                {
                    collision = true;
                    break;
                }
            }

            if (!collision)
                return id;
        }
    }
}
