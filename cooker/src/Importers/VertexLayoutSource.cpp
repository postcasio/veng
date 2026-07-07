#include "VertexLayoutSource.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>

#include <fmt/format.h>

#include <Veng/Cook/JsonFile.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Cook
{
    namespace
    {
        void SetName(char (&dest)[ShaderNameCapacity], std::string_view name)
        {
            const usize n = std::min(name.size(), static_cast<usize>(ShaderNameCapacity) - 1);
            std::memcpy(dest, name.data(), n);
            dest[n] = '\0';
        }

        // The four float formats, the RGBA16Uint bone-index format, and the R32Uint scalar-index
        // format are the valid vertex-layout formats; the cooked element stores the Renderer::Format
        // ordinal.
        optional<u32> ParseFormatString(const string& name)
        {
            if (name == "R32Sfloat")
            {
                return static_cast<u32>(Renderer::Format::R32Sfloat);
            }
            if (name == "R32Uint")
            {
                return static_cast<u32>(Renderer::Format::R32Uint);
            }
            if (name == "RG32Sfloat")
            {
                return static_cast<u32>(Renderer::Format::RG32Sfloat);
            }
            if (name == "RGB32Sfloat")
            {
                return static_cast<u32>(Renderer::Format::RGB32Sfloat);
            }
            if (name == "RGBA32Sfloat")
            {
                return static_cast<u32>(Renderer::Format::RGBA32Sfloat);
            }
            if (name == "RGBA16Uint")
            {
                return static_cast<u32>(Renderer::Format::RGBA16Uint);
            }
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
                                "(valid: R32Sfloat, RG32Sfloat, RGB32Sfloat, RGBA32Sfloat, "
                                "RGBA16Uint)",
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
        const Result<json> parsed = ReadJsonFile(filePath, "vertex layout");
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }

        return ParseVertexLayoutElements(*parsed, filePath.string());
    }
}
