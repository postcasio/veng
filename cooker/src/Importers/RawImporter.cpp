#include "RawImporter.h"

#include <fstream>

#include <fmt/format.h>

namespace Veng::Cook
{
    Result<vector<u8>> RawImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
            return std::unexpected("raw importer: missing or invalid 'source'");

        const path source = context.PackDir / entry["source"].get<string>();

        std::ifstream file(source, std::ios::binary);
        if (!file)
            return std::unexpected(fmt::format("raw importer: failed to open '{}'", source.string()));

        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size < 0)
            return std::unexpected(fmt::format("raw importer: failed to determine size of '{}'", source.string()));
        file.seekg(0, std::ios::beg);

        vector<u8> bytes(static_cast<usize>(size));
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file)
            return std::unexpected(fmt::format("raw importer: failed reading '{}'", source.string()));

        return bytes;
    }
}
