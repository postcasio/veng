#include "LocaleSource.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/HexId.h>

namespace Veng::Cook
{
    namespace
    {
        // Maps a CLDR plural-category name to its PluralCategory ordinal, or nullopt for an unknown
        // name. The order matches the runtime enum: Zero, One, Two, Few, Many, Other.
        optional<usize> PluralCategoryIndex(std::string_view name)
        {
            if (name == "zero")
            {
                return 0;
            }
            if (name == "one")
            {
                return 1;
            }
            if (name == "two")
            {
                return 2;
            }
            if (name == "few")
            {
                return 3;
            }
            if (name == "many")
            {
                return 4;
            }
            if (name == "other")
            {
                return 5;
            }
            return std::nullopt;
        }

        // The Other slot index, the terminal fallback every pluralized message must define.
        constexpr usize OtherIndex = 5;

        // Decodes the first UTF-8 codepoint of a string, or the fallback when it is empty.
        char32_t FirstCodepoint(std::string_view text, char32_t fallback)
        {
            if (text.empty())
            {
                return fallback;
            }
            const auto byte = [&](usize i) { return static_cast<u8>(text[i]); };
            const u8 lead = byte(0);
            if (lead < 0x80)
            {
                return static_cast<char32_t>(lead);
            }
            if ((lead & 0xE0) == 0xC0 && text.size() >= 2)
            {
                return static_cast<char32_t>(((lead & 0x1F) << 6) | (byte(1) & 0x3F));
            }
            if ((lead & 0xF0) == 0xE0 && text.size() >= 3)
            {
                return static_cast<char32_t>(((lead & 0x0F) << 12) | ((byte(1) & 0x3F) << 6) |
                                             (byte(2) & 0x3F));
            }
            if ((lead & 0xF8) == 0xF0 && text.size() >= 4)
            {
                return static_cast<char32_t>(((lead & 0x07) << 18) | ((byte(1) & 0x3F) << 12) |
                                             ((byte(2) & 0x3F) << 6) | (byte(3) & 0x3F));
            }
            return fallback;
        }

        // Appends a string to the pool and returns its span.
        CookedLocaleStringSpan Append(vector<u8>& pool, std::string_view text)
        {
            const CookedLocaleStringSpan span{.Offset = static_cast<u32>(pool.size()),
                                              .Length = static_cast<u32>(text.size())};
            pool.insert(pool.end(), text.begin(), text.end());
            return span;
        }

        // Writes a nul-terminated fixed field, or returns false when the id exceeds the capacity.
        bool WriteFixed(char (&dst)[LocaleIdCapacity], std::string_view text)
        {
            if (text.size() >= LocaleIdCapacity)
            {
                return false;
            }
            std::memcpy(dst, text.data(), text.size());
            dst[text.size()] = '\0';
            return true;
        }

        template <class T>
        void AppendPod(vector<u8>& out, const T& value)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(T));
        }
    }

    Result<ParsedLocaleCatalog> ParseLocaleCatalogSource(const json& doc,
                                                         const std::string_view label)
    {
        if (!doc.is_object())
        {
            return std::unexpected(fmt::format(
                "locale catalog importer: '{}': the source is not a JSON object", label));
        }
        if (!doc.contains("locale") || !doc["locale"].is_string() ||
            doc["locale"].get<string>().empty())
        {
            return std::unexpected(
                fmt::format("locale catalog importer: '{}': missing or empty 'locale'", label));
        }

        ParsedLocaleCatalog catalog;
        catalog.LocaleId = doc["locale"].get<string>();
        catalog.FallbackId = doc.value("fallback", catalog.LocaleId);
        catalog.PluralRuleId = doc.value("pluralRule", catalog.LocaleId);
        catalog.Decimal = FirstCodepoint(doc.value("decimal", string(".")), U'.');
        // An empty "grouping" string disables grouping (U'\0'); an omitted key keeps the comma.
        catalog.Grouping = doc.contains("grouping")
                               ? FirstCodepoint(doc.value("grouping", string()), U'\0')
                               : U',';

        if (catalog.LocaleId.size() >= LocaleIdCapacity ||
            catalog.FallbackId.size() >= LocaleIdCapacity ||
            catalog.PluralRuleId.size() >= LocaleIdCapacity)
        {
            return std::unexpected(
                fmt::format("locale catalog importer: '{}': a locale id exceeds {} bytes", label,
                            LocaleIdCapacity - 1));
        }

        if (doc.contains("elision"))
        {
            const json& elision = doc["elision"];
            if (!elision.is_object())
            {
                return std::unexpected(fmt::format(
                    "locale catalog importer: '{}': 'elision' is not a JSON object", label));
            }
            if (elision.contains("initials"))
            {
                if (!elision["initials"].is_string())
                {
                    return std::unexpected(fmt::format(
                        "locale catalog importer: '{}': 'elision.initials' is not a string",
                        label));
                }
                catalog.ElisionInitials = elision["initials"].get<string>();
            }
            if (elision.contains("words"))
            {
                if (!elision["words"].is_object())
                {
                    return std::unexpected(fmt::format(
                        "locale catalog importer: '{}': 'elision.words' is not a JSON object",
                        label));
                }
                for (const auto& [word, elided] : elision["words"].items())
                {
                    if (word.empty() || word.find(' ') != string::npos)
                    {
                        return std::unexpected(fmt::format(
                            "locale catalog importer: '{}': elision word '{}' is empty or holds a "
                            "space",
                            label, word));
                    }
                    if (!elided.is_string())
                    {
                        return std::unexpected(fmt::format(
                            "locale catalog importer: '{}': elision word '{}' maps to a non-string",
                            label, word));
                    }
                    catalog.ElisionRules.push_back({.Word = word, .Elided = elided.get<string>()});
                }
            }
            if (!catalog.ElisionRules.empty() && catalog.ElisionInitials.empty())
            {
                return std::unexpected(fmt::format(
                    "locale catalog importer: '{}': 'elision.words' needs 'elision.initials'",
                    label));
            }
        }

        if (doc.contains("messages"))
        {
            if (!doc["messages"].is_object())
            {
                return std::unexpected(fmt::format(
                    "locale catalog importer: '{}': 'messages' is not a JSON object", label));
            }
            for (const auto& [key, value] : doc["messages"].items())
            {
                ParsedLocaleMessage message;
                message.Key = key;

                if (value.is_string())
                {
                    message.Variants[OtherIndex] = value.get<string>();
                }
                else if (value.is_object())
                {
                    message.IsPlural = true;
                    for (const auto& [category, variant] : value.items())
                    {
                        const optional<usize> index = PluralCategoryIndex(category);
                        if (!index)
                        {
                            return std::unexpected(fmt::format(
                                "locale catalog importer: '{}': message '{}' names unknown plural "
                                "category '{}'",
                                label, key, category));
                        }
                        if (!variant.is_string())
                        {
                            return std::unexpected(fmt::format(
                                "locale catalog importer: '{}': message '{}' plural '{}' is not a "
                                "string",
                                label, key, category));
                        }
                        message.Variants[*index] = variant.get<string>();
                    }
                    if (!message.Variants[OtherIndex].has_value())
                    {
                        return std::unexpected(fmt::format(
                            "locale catalog importer: '{}': pluralized message '{}' is missing the "
                            "required 'other' variant",
                            label, key));
                    }
                }
                else
                {
                    return std::unexpected(fmt::format(
                        "locale catalog importer: '{}': message '{}' is neither a string nor a "
                        "plural object",
                        label, key));
                }

                catalog.Messages.push_back(std::move(message));
            }
        }

        return catalog;
    }

    vector<u8> EncodeLocaleCatalogBlob(const ParsedLocaleCatalog& catalog)
    {
        // Build the entry table and the pool together, then sort the entries by (hash, key) so the
        // runtime binary search sees the order this cook wrote.
        struct BuiltEntry
        {
            CookedLocaleEntry Entry;
            const string* Key;
        };
        vector<u8> pool;
        vector<BuiltEntry> built;
        built.reserve(catalog.Messages.size());

        for (const ParsedLocaleMessage& message : catalog.Messages)
        {
            CookedLocaleEntry entry;
            entry.KeyHash = HashLocaleKey(message.Key);
            entry.Key = Append(pool, message.Key);
            entry.IsPlural = message.IsPlural ? 1u : 0u;
            entry.PresentMask = 0;
            for (usize i = 0; i < CookedLocalePluralCategoryCount; ++i)
            {
                if (message.Variants[i].has_value())
                {
                    entry.Variants[i] = Append(pool, *message.Variants[i]);
                    entry.PresentMask |= (1u << i);
                }
            }
            built.push_back({.Entry = entry, .Key = &message.Key});
        }

        std::ranges::sort(built,
                          [](const BuiltEntry& a, const BuiltEntry& b)
                          {
                              if (a.Entry.KeyHash != b.Entry.KeyHash)
                              {
                                  return a.Entry.KeyHash < b.Entry.KeyHash;
                              }
                              return *a.Key < *b.Key;
                          });

        CookedLocaleCatalogHeader header;
        header.Magic = CookedLocaleCatalogMagic;
        header.Version = CookedLocaleCatalogVersion;
        WriteFixed(header.LocaleId, catalog.LocaleId);
        WriteFixed(header.FallbackId, catalog.FallbackId);
        WriteFixed(header.PluralRule, catalog.PluralRuleId);
        header.Decimal = static_cast<u32>(catalog.Decimal);
        header.Grouping = static_cast<u32>(catalog.Grouping);
        header.EntryCount = static_cast<u32>(built.size());

        header.ElisionInitials = Append(pool, catalog.ElisionInitials);
        vector<CookedLocaleElisionRule> rules;
        rules.reserve(catalog.ElisionRules.size());
        for (const ParsedElisionRule& rule : catalog.ElisionRules)
        {
            rules.push_back({.Word = Append(pool, rule.Word), .Elided = Append(pool, rule.Elided)});
        }
        header.ElisionRuleCount = static_cast<u32>(rules.size());
        header.StringPoolBytes = static_cast<u32>(pool.size());

        vector<u8> blob;
        AppendPod(blob, header);
        for (const BuiltEntry& entry : built)
        {
            AppendPod(blob, entry.Entry);
        }
        for (const CookedLocaleElisionRule& rule : rules)
        {
            AppendPod(blob, rule);
        }
        blob.insert(blob.end(), pool.begin(), pool.end());
        return blob;
    }

    Result<ParsedLocaleIndex> ParseLocaleIndexSource(const json& doc, const std::string_view label)
    {
        if (!doc.is_object())
        {
            return std::unexpected(
                fmt::format("locale index importer: '{}': the source is not a JSON object", label));
        }
        if (!doc.contains("source") || !doc["source"].is_string() ||
            doc["source"].get<string>().empty())
        {
            return std::unexpected(
                fmt::format("locale index importer: '{}': missing or empty 'source'", label));
        }
        if (!doc.contains("locales") || !doc["locales"].is_array() || doc["locales"].empty())
        {
            return std::unexpected(
                fmt::format("locale index importer: '{}': missing or empty 'locales'", label));
        }

        ParsedLocaleIndex index;
        index.Source = doc["source"].get<string>();

        const string coverage = doc.value("coverage", string("warn"));
        if (coverage == "complete")
        {
            index.CoverageComplete = true;
        }
        else if (coverage != "warn")
        {
            return std::unexpected(fmt::format(
                "locale index importer: '{}': 'coverage' is '{}' (expected 'warn' or 'complete')",
                label, coverage));
        }

        for (const json& localeJson : doc["locales"])
        {
            if (!localeJson.is_object())
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': a 'locales' entry is not a JSON object", label));
            }
            if (!localeJson.contains("id") || !localeJson["id"].is_string() ||
                localeJson["id"].get<string>().empty())
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': a 'locales' entry is missing 'id'", label));
            }
            if (!localeJson.contains("catalog") || !localeJson["catalog"].is_string())
            {
                return std::unexpected(
                    fmt::format("locale index importer: '{}': locale '{}' is missing 'catalog'",
                                label, localeJson["id"].get<string>()));
            }

            ParsedLocaleIndexLocale locale;
            locale.Id = localeJson["id"].get<string>();
            locale.DisplayName = localeJson.value("displayName", locale.Id);
            locale.Fallback = localeJson.value("fallback", locale.Id);

            const optional<AssetId> catalogId = ParseAssetId(localeJson["catalog"].get<string>());
            if (!catalogId)
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': locale '{}' has an unparseable catalog id '{}'",
                    label, locale.Id, localeJson["catalog"].get<string>()));
            }
            locale.Catalog = *catalogId;

            if (locale.Id.size() >= LocaleIdCapacity || locale.Fallback.size() >= LocaleIdCapacity)
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': locale '{}' or its fallback exceeds {} bytes",
                    label, locale.Id, LocaleIdCapacity - 1));
            }

            index.Locales.push_back(std::move(locale));
        }

        if (index.Source.size() >= LocaleIdCapacity)
        {
            return std::unexpected(
                fmt::format("locale index importer: '{}': the source locale id exceeds {} bytes",
                            label, LocaleIdCapacity - 1));
        }

        return index;
    }

    Result<vector<u8>> EncodeLocaleIndexBlob(const ParsedLocaleIndex& index)
    {
        vector<u8> pool;
        vector<CookedLocaleIndexEntry> entries;
        entries.reserve(index.Locales.size());

        for (const ParsedLocaleIndexLocale& locale : index.Locales)
        {
            CookedLocaleIndexEntry entry;
            if (!WriteFixed(entry.LocaleId, locale.Id) ||
                !WriteFixed(entry.FallbackId, locale.Fallback))
            {
                return std::unexpected(fmt::format(
                    "locale index importer: locale '{}' or its fallback exceeds {} bytes",
                    locale.Id, LocaleIdCapacity - 1));
            }
            entry.CatalogId = locale.Catalog.Value;
            entry.DisplayName = Append(pool, locale.DisplayName);
            entries.push_back(entry);
        }

        CookedLocaleIndexHeader header;
        header.Magic = CookedLocaleIndexMagic;
        header.Version = CookedLocaleIndexVersion;
        if (!WriteFixed(header.SourceLocaleId, index.Source))
        {
            return std::unexpected(
                fmt::format("locale index importer: the source locale id exceeds {} bytes",
                            LocaleIdCapacity - 1));
        }
        header.LocaleCount = static_cast<u32>(entries.size());
        header.StringPoolBytes = static_cast<u32>(pool.size());

        vector<u8> blob;
        AppendPod(blob, header);
        for (const CookedLocaleIndexEntry& entry : entries)
        {
            AppendPod(blob, entry);
        }
        blob.insert(blob.end(), pool.begin(), pool.end());
        return blob;
    }
}
