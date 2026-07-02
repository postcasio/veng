#include <Veng/Cook/JsonFile.h>

#include <fstream>
#include <sstream>

#include <fmt/format.h>

namespace Veng::Cook
{
    Result<json> ReadJsonFile(const path& file, std::string_view what)
    {
        const std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            return std::unexpected(fmt::format("{} '{}': failed to open", what, file.string()));
        }

        std::ostringstream contents;
        contents << stream.rdbuf();
        json parsed = json::parse(contents.str(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            return std::unexpected(fmt::format("{} '{}': invalid JSON", what, file.string()));
        }
        return parsed;
    }
}
