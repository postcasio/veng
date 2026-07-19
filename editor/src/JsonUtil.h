#pragma once

#include <Veng/Result.h>
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

    /// @brief Writes @p text to @p file, reporting a stream failure at open or at close.
    ///
    /// The close is checked as well as the open: a buffered write only reaches the filesystem when
    /// the stream is flushed, so a full disk or a revoked permission surfaces at close and a save
    /// that reported success would have left a truncated source behind.
    /// @param file  The file to overwrite.
    /// @param text  The exact bytes to write.
    /// @return Empty on success; an error naming the file otherwise.
    [[nodiscard]] inline Veng::VoidResult WriteTextFile(const Veng::path& file,
                                                        Veng::string_view text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return std::unexpected(fmt::format("failed to write {}", file.string()));
        }

        out << text;
        out.close();
        if (!out)
        {
            return std::unexpected(fmt::format("failed to write {}", file.string()));
        }
        return {};
    }

    /// @brief Merge-writes an asset source's JSON, preserving every key the editor does not own.
    ///
    /// Reads the existing document, hands it to @p patch to set the keys the caller owns, and
    /// rewrites it — so hand-authored structure, comments-as-keys, and settings no panel exposes
    /// survive a save. A missing or malformed source starts from an empty object.
    /// @param file    The JSON source to rewrite.
    /// @param indent  Indent width passed to the dump, matching the file's authored style.
    /// @param patch   Sets the keys the caller owns on the document read from disk.
    /// @return Empty on success; an error naming the file otherwise.
    [[nodiscard]] inline Veng::VoidResult
    MergeWriteJsonObject(const Veng::path& file, const int indent,
                         const Veng::function<void(nlohmann::json&)>& patch)
    {
        nlohmann::json doc = ReadJsonObject(file).value_or(nlohmann::json::object());
        patch(doc);
        return WriteTextFile(file, doc.dump(indent) + "\n");
    }
}
