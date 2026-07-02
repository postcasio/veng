#pragma once

#include <Veng/Veng.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    /// @brief Reads a JSON file into a parsed object, or nullopt on missing/malformed input.
    ///
    /// The shared read → parse → validate preamble of every editor JSON load: the
    /// caller decides what a missing document means (log and bail, or start from
    /// an empty object for a round-trip save). A non-object root is malformed.
    /// @param file  The JSON file to read.
    /// @return The parsed object document, or nullopt.
    inline Veng::optional<nlohmann::json> ReadJsonObject(const Veng::path& file)
    {
        const std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            return std::nullopt;
        }

        std::ostringstream contents;
        contents << stream.rdbuf();
        nlohmann::json parsed = nlohmann::json::parse(contents.str(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            return std::nullopt;
        }
        return parsed;
    }
}
