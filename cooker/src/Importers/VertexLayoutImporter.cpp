#include "VertexLayoutImporter.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>

#include "VertexLayoutSource.h"

namespace Veng::Cook
{
    Result<vector<u8>> VertexLayoutImporter::Cook(const CookContext& context,
                                                  const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
            return std::unexpected("vertex layout importer: missing or invalid 'source'");

        const path sourcePath = context.PackDir / entry["source"].get<string>();

        const Result<vector<CookedVertexLayoutElement>> elementsResult =
            ReadVertexLayoutFile(sourcePath);
        if (!elementsResult)
            return std::unexpected(elementsResult.error());

        const vector<CookedVertexLayoutElement>& elements = *elementsResult;

        CookedVertexLayoutHeader header{};
        header.ElementCount = static_cast<u32>(elements.size());

        const usize elementBytes = elements.size() * sizeof(CookedVertexLayoutElement);
        vector<u8> blob(sizeof(CookedVertexLayoutHeader) + elementBytes);

        usize cursor = 0;
        std::memcpy(blob.data() + cursor, &header, sizeof(header));
        cursor += sizeof(header);
        std::memcpy(blob.data() + cursor, elements.data(), elementBytes);

        return blob;
    }
}
