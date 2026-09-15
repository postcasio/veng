#include "LocaleCatalogImporter.h"

#include <Veng/Asset/Path.h>
#include <Veng/Cook/JsonFile.h>

#include "LocaleSource.h"

namespace Veng::Cook
{
    Result<vector<u8>> LocaleCatalogImporter::Cook(const CookContext& context,
                                                   const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("locale catalog importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        context.RecordDependency(sourcePath);

        const Result<json> docResult = ReadJsonFile(sourcePath, "locale catalog importer");
        if (!docResult)
        {
            return std::unexpected(docResult.error());
        }

        const Result<ParsedLocaleCatalog> parsed =
            ParseLocaleCatalogSource(*docResult, sourcePath.string());
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }

        return EncodeLocaleCatalogBlob(*parsed);
    }
}
