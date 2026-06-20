#include "VertexLayoutSource.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

#include <fmt/format.h>

namespace Veng::Cook
{
    namespace
    {
        // Underlying-integer ordinals mirroring Veng::Renderer::Format
        // (cycle-avoidance rule, CookedBlobs.h). Only the four float formats
        // that VertexBufferElement supports are valid vertex-layout formats.
        constexpr u32 FormatR32Sfloat = 7;
        constexpr u32 FormatRG32Sfloat = 8;
        constexpr u32 FormatRGB32Sfloat = 9;
        constexpr u32 FormatRGBA32Sfloat = 10;

        void SetName(char (&dest)[ShaderNameCapacity], std::string_view name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        optional<u32> ParseFormatString(const string& name)
        {
            if (name == "R32Sfloat")
                return FormatR32Sfloat;
            if (name == "RG32Sfloat")
                return FormatRG32Sfloat;
            if (name == "RGB32Sfloat")
                return FormatRGB32Sfloat;
            if (name == "RGBA32Sfloat")
                return FormatRGBA32Sfloat;
            return std::nullopt;
        }
    }

    Result<vector<CookedVertexLayoutElement>>
    ParseVertexLayoutElements(const json& layoutJson, const string& diagnosticContext)
    {
        if (!layoutJson.is_object() || !layoutJson.contains("elements") ||
            !layoutJson["elements"].is_array())
        {
            return std::unexpected(fmt::format(
                "vertex layout '{}': missing or invalid 'elements' array", diagnosticContext));
        }

        const json& elements = layoutJson["elements"];
        vector<CookedVertexLayoutElement> result;
        result.reserve(elements.size());

        for (usize i = 0; i < elements.size(); ++i)
        {
            const json& e = elements[i];
            if (!e.is_object() || !e.contains("format") || !e["format"].is_string() ||
                !e.contains("name") || !e["name"].is_string())
            {
                return std::unexpected(fmt::format(
                    "vertex layout '{}': element[{}]: must have string 'format' and 'name'",
                    diagnosticContext, i));
            }

            const string formatStr = e["format"].get<string>();
            const optional<u32> format = ParseFormatString(formatStr);
            if (!format)
            {
                return std::unexpected(
                    fmt::format("vertex layout '{}': element[{}]: unrecognized format '{}' "
                                "(valid: R32Sfloat, RG32Sfloat, RGB32Sfloat, RGBA32Sfloat)",
                                diagnosticContext, i, formatStr));
            }

            CookedVertexLayoutElement elem{};
            elem.Format = *format;
            SetName(elem.Name, e["name"].get<string>());
            result.push_back(elem);
        }

        return result;
    }

    Result<vector<CookedVertexLayoutElement>> ReadVertexLayoutFile(const path& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file)
        {
            return std::unexpected(
                fmt::format("vertex layout '{}': failed to open", filePath.string()));
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        const string content = ss.str();

        const json parsed = json::parse(content, nullptr, false);
        if (parsed.is_discarded())
        {
            return std::unexpected(
                fmt::format("vertex layout '{}': invalid JSON", filePath.string()));
        }

        return ParseVertexLayoutElements(parsed, filePath.string());
    }
}
