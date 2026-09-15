#include "LocaleIndexImporter.h"

#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <Veng/Asset/HexId.h>
#include <Veng/Asset/Path.h>
#include <Veng/Cook/JsonFile.h>

#include "LocaleSource.h"

namespace Veng::Cook
{
    namespace
    {
        // Collects a catalog source's message keys, resolving its id, reading its file, and parsing
        // it through the shared walk — so the coverage diff sees exactly the keys the catalog cook
        // would emit. Records the catalog source as a dependency so a catalog edit re-runs this cook.
        Result<std::unordered_set<string>> CollectKeys(const CookContext& context,
                                                       const ParsedLocaleIndexLocale& locale,
                                                       const string& indexLabel)
        {
            const optional<ResolvedSource> resolved = context.Resolve(locale.Catalog);
            if (!resolved)
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': locale '{}' names catalog {}, which no asset "
                    "provides",
                    indexLabel, locale.Id, FormatAssetId(locale.Catalog)));
            }
            if (resolved->Type != AssetTypes::LocaleCatalog)
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': locale '{}' names catalog {}, which is not a "
                    "LocaleCatalog",
                    indexLabel, locale.Id, FormatAssetId(locale.Catalog)));
            }
            context.RecordDependency(resolved->AbsolutePath);

            const Result<json> docResult =
                ReadJsonFile(resolved->AbsolutePath, "locale index importer");
            if (!docResult)
            {
                return std::unexpected(docResult.error());
            }
            const Result<ParsedLocaleCatalog> parsed =
                ParseLocaleCatalogSource(*docResult, resolved->AbsolutePath.string());
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }

            std::unordered_set<string> keys;
            for (const ParsedLocaleMessage& message : parsed->Messages)
            {
                keys.insert(message.Key);
            }
            return keys;
        }

        // Validates the fallback graph: every fallback names an indexed locale, the source's fallback
        // is itself, and every locale's chain reaches the source without a cycle.
        VoidResult ValidateFallbackGraph(const ParsedLocaleIndex& index, const string& label)
        {
            std::unordered_map<string, const ParsedLocaleIndexLocale*> byId;
            for (const ParsedLocaleIndexLocale& locale : index.Locales)
            {
                byId.emplace(locale.Id, &locale);
            }

            const auto source = byId.find(index.Source);
            if (source == byId.end())
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': the source locale '{}' is not among the locales",
                    label, index.Source));
            }
            if (source->second->Fallback != index.Source)
            {
                return std::unexpected(fmt::format(
                    "locale index importer: '{}': the source locale '{}' must fall back to itself, "
                    "not '{}'",
                    label, index.Source, source->second->Fallback));
            }

            for (const ParsedLocaleIndexLocale& locale : index.Locales)
            {
                if (!byId.contains(locale.Fallback))
                {
                    return std::unexpected(fmt::format(
                        "locale index importer: '{}': locale '{}' falls back to '{}', which is not "
                        "in the index",
                        label, locale.Id, locale.Fallback));
                }

                std::unordered_set<string> visited;
                string current = locale.Id;
                while (current != index.Source)
                {
                    if (!visited.insert(current).second)
                    {
                        return std::unexpected(fmt::format(
                            "locale index importer: '{}': the fallback chain from '{}' cycles",
                            label, locale.Id));
                    }
                    const ParsedLocaleIndexLocale* entry = byId.at(current);
                    if (entry->Fallback == current)
                    {
                        return std::unexpected(fmt::format(
                            "locale index importer: '{}': locale '{}' is terminal but is not the "
                            "source '{}' — its fallback chain never reaches the source",
                            label, current, index.Source));
                    }
                    current = entry->Fallback;
                }
            }

            return {};
        }
    }

    Result<vector<u8>> LocaleIndexImporter::Cook(const CookContext& context,
                                                 const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("locale index importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        context.RecordDependency(sourcePath);
        const string label = sourcePath.string();

        const Result<json> docResult = ReadJsonFile(sourcePath, "locale index importer");
        if (!docResult)
        {
            return std::unexpected(docResult.error());
        }

        const Result<ParsedLocaleIndex> parsed = ParseLocaleIndexSource(*docResult, label);
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        const ParsedLocaleIndex& index = *parsed;

        if (const VoidResult graph = ValidateFallbackGraph(index, label); !graph)
        {
            return std::unexpected(graph.error());
        }

        // Resolve every catalog (a dangling reference is a cook error) and collect its keys. The
        // source locale's keys are the coverage baseline.
        std::unordered_map<string, std::unordered_set<string>> keysByLocale;
        for (const ParsedLocaleIndexLocale& locale : index.Locales)
        {
            const Result<std::unordered_set<string>> keys = CollectKeys(context, locale, label);
            if (!keys)
            {
                return std::unexpected(keys.error());
            }
            keysByLocale.emplace(locale.Id, *keys);
        }

        // Translation-coverage: diff each non-source locale against the source. A missing key falls
        // back to the source at runtime; declaring "coverage": "complete" makes that a cook error.
        const std::unordered_set<string>& sourceKeys = keysByLocale.at(index.Source);
        for (const ParsedLocaleIndexLocale& locale : index.Locales)
        {
            if (locale.Id == index.Source)
            {
                continue;
            }
            const std::unordered_set<string>& localeKeys = keysByLocale.at(locale.Id);

            vector<string> missing;
            for (const string& key : sourceKeys)
            {
                if (!localeKeys.contains(key))
                {
                    missing.push_back(key);
                }
            }
            vector<string> extra;
            for (const string& key : localeKeys)
            {
                if (!sourceKeys.contains(key))
                {
                    extra.push_back(key);
                }
            }

            if (!missing.empty())
            {
                if (index.CoverageComplete)
                {
                    return std::unexpected(fmt::format(
                        "locale index importer: '{}': locale '{}' is missing {} of the {} source "
                        "keys (coverage is 'complete'); first missing: '{}'",
                        label, locale.Id, missing.size(), sourceKeys.size(), missing.front()));
                }
                fmt::print(
                    stderr,
                    "locale index importer: '{}': locale '{}' is missing {} of the {} source "
                    "keys (silent source fallback at runtime)\n",
                    label, locale.Id, missing.size(), sourceKeys.size());
            }
            if (!extra.empty())
            {
                fmt::print(stderr,
                           "locale index importer: '{}': locale '{}' defines {} keys the source "
                           "does not; first: '{}'\n",
                           label, locale.Id, extra.size(), extra.front());
            }
        }

        return EncodeLocaleIndexBlob(index);
    }
}
