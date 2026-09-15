#include "LocaleCatalogLoader.h"

#include <array>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>

namespace Veng
{
    namespace
    {
        static_assert(static_cast<usize>(Localization::PluralCategoryCount) ==
                          CookedLocalePluralCategoryCount,
                      "the cooked catalog's plural-variant width must match the message format's "
                      "PluralCategoryCount");

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
    LocaleCatalogLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                              TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                              std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedLocaleCatalogHeader))
        {
            return std::unexpected(Corrupt(id, "locale catalog: cooked blob smaller than header"));
        }

        CookedLocaleCatalogHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Magic != CookedLocaleCatalogMagic)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("locale catalog: bad magic 0x{:08X} (expected 0x{:08X})",
                                        header.Magic, CookedLocaleCatalogMagic)));
        }
        if (header.Version != CookedLocaleCatalogVersion)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("locale catalog: blob version {} does not match version {} — "
                                "re-cook the pack",
                                header.Version, CookedLocaleCatalogVersion)));
        }

        const usize entryBytes = static_cast<usize>(header.EntryCount) * sizeof(CookedLocaleEntry);
        usize cursor = sizeof(CookedLocaleCatalogHeader);
        if (cooked.size() < cursor + entryBytes + header.StringPoolBytes)
        {
            return std::unexpected(Corrupt(id, "locale catalog: cooked blob truncated"));
        }

        const std::span<const u8> pool =
            cooked.subspan(cursor + entryBytes, header.StringPoolBytes);

        // Resolves a span into the string pool, rejecting one that runs past it.
        const auto readSpan = [&](const CookedLocaleStringSpan& span,
                                  string& out) -> optional<AssetLoadError>
        {
            if (static_cast<usize>(span.Offset) + span.Length > pool.size())
            {
                return Corrupt(id, "locale catalog: a string span runs past the string pool");
            }
            out.assign(reinterpret_cast<const char*>(pool.data()) + span.Offset, span.Length);
            return std::nullopt;
        };

        Localization::LocaleCatalog::Contents contents;
        contents.LocaleId = ReadFixed(header.LocaleId);
        contents.FallbackId = ReadFixed(header.FallbackId);
        contents.PluralRuleId = ReadFixed(header.PluralRule);
        contents.Numbers = Localization::NumberFormat{
            .Decimal = static_cast<char32_t>(header.Decimal),
            .Grouping = static_cast<char32_t>(header.Grouping),
        };
        contents.Entries.reserve(header.EntryCount);

        for (u32 i = 0; i < header.EntryCount; ++i)
        {
            CookedLocaleEntry cookedEntry;
            std::memcpy(&cookedEntry, cooked.data() + cursor, sizeof(cookedEntry));
            cursor += sizeof(cookedEntry);

            Localization::LocaleCatalogEntry entry;
            entry.KeyHash = cookedEntry.KeyHash;
            if (const optional<AssetLoadError> error = readSpan(cookedEntry.Key, entry.Key))
            {
                return std::unexpected(*error);
            }

            if (cookedEntry.IsPlural != 0)
            {
                std::array<string, Localization::PluralCategoryCount> variants;
                for (usize category = 0; category < Localization::PluralCategoryCount; ++category)
                {
                    if ((cookedEntry.PresentMask & (1u << category)) != 0)
                    {
                        if (const optional<AssetLoadError> error =
                                readSpan(cookedEntry.Variants[category], variants[category]))
                        {
                            return std::unexpected(*error);
                        }
                    }
                }
                entry.Message.Plurals = std::move(variants);
            }
            else
            {
                const auto other = static_cast<usize>(Localization::PluralCategory::Other);
                if (const optional<AssetLoadError> error =
                        readSpan(cookedEntry.Variants[other], entry.Message.Template))
                {
                    return std::unexpected(*error);
                }
            }

            contents.Entries.push_back(std::move(entry));
        }

        const Ref<Localization::LocaleCatalog> catalog =
            Localization::LocaleCatalog::Create(std::move(contents));
        return Detail::LoadJob{.Resource = Detail::RefAny(catalog)};
    }
}
