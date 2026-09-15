#include "LocaleIndexLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // Reads a nul-terminated fixed field into a string, stopping at the first nul.
        string ReadFixed(const char (&field)[LocaleIdCapacity])
        {
            usize length = 0;
            while (length < LocaleIdCapacity && field[length] != '\0')
            {
                ++length;
            }
            return string(field, length);
        }
    }

    AssetResult<Detail::LoadJob>
    LocaleIndexLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                            TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                            std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedLocaleIndexHeader))
        {
            return std::unexpected(Corrupt(id, "locale index: cooked blob smaller than header"));
        }

        CookedLocaleIndexHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Magic != CookedLocaleIndexMagic)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("locale index: bad magic 0x{:08X} (expected 0x{:08X})",
                                        header.Magic, CookedLocaleIndexMagic)));
        }
        if (header.Version != CookedLocaleIndexVersion)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("locale index: blob version {} does not match version {} — re-cook "
                                "the pack",
                                header.Version, CookedLocaleIndexVersion)));
        }

        const usize entryBytes =
            static_cast<usize>(header.LocaleCount) * sizeof(CookedLocaleIndexEntry);
        const usize cursor = sizeof(CookedLocaleIndexHeader);
        if (cooked.size() < cursor + entryBytes + header.StringPoolBytes)
        {
            return std::unexpected(Corrupt(id, "locale index: cooked blob truncated"));
        }

        const std::span<const u8> pool =
            cooked.subspan(cursor + entryBytes, header.StringPoolBytes);

        Localization::LocaleIndex::Contents contents;
        contents.SourceLocale = ReadFixed(header.SourceLocaleId);
        contents.Locales.reserve(header.LocaleCount);

        for (u32 i = 0; i < header.LocaleCount; ++i)
        {
            CookedLocaleIndexEntry cookedEntry;
            std::memcpy(&cookedEntry, cooked.data() + cursor + i * sizeof(cookedEntry),
                        sizeof(cookedEntry));

            if (static_cast<usize>(cookedEntry.DisplayName.Offset) +
                    cookedEntry.DisplayName.Length >
                pool.size())
            {
                return std::unexpected(
                    Corrupt(id, "locale index: an endonym span runs past the string pool"));
            }

            Localization::LocaleEntry entry;
            entry.Id = ReadFixed(cookedEntry.LocaleId);
            entry.Fallback = ReadFixed(cookedEntry.FallbackId);
            entry.Catalog = AssetId{cookedEntry.CatalogId};
            entry.DisplayName.assign(reinterpret_cast<const char*>(pool.data()) +
                                         cookedEntry.DisplayName.Offset,
                                     cookedEntry.DisplayName.Length);
            contents.Locales.push_back(std::move(entry));
        }

        const Ref<Localization::LocaleIndex> index =
            Localization::LocaleIndex::Create(std::move(contents));
        return Detail::LoadJob{.Resource = Detail::RefAny(index)};
    }
}
