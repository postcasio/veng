#pragma once

#include <string_view>
#include <string>
#include <utility>
#include <functional>
#include <fmt/format.h>

namespace Veng::Log
{
    enum class Level { Info, Warn, Error };

    // A sink receives the level and the message body (no timestamp/level prefix
    // — the sink decides how to present it). Called on whatever thread logs;
    // veng is single-threaded; no synchronization is provided.
    using Sink = std::function<void(Level, std::string_view)>;

    // Replace the log sink. Passing nullptr restores the default stdout sink.
    void SetSink(Sink sink);

    // Messages below this level are dropped. Defaults to Level::Info.
    void SetMinimumLevel(Level level);

    // Core entry point: filters by minimum level, then forwards to the sink.
    void LogMessage(Level level, std::string_view message);

    // Convenience helpers with fmt-style formatting
    // Only enable these overloads when at least one formatting argument is provided
    template <typename... Args>
        requires (sizeof...(Args) > 0)
    inline void Info(fmt::format_string<Args...> fmtStr, Args&&... args)
    {
        auto msg = fmt::format(fmtStr, std::forward<Args>(args)...);
        LogMessage(Level::Info, msg);
    }

    template <typename... Args>
        requires (sizeof...(Args) > 0)
    inline void Warn(fmt::format_string<Args...> fmtStr, Args&&... args)
    {
        auto msg = fmt::format(fmtStr, std::forward<Args>(args)...);
        LogMessage(Level::Warn, msg);
    }

    template <typename... Args>
        requires (sizeof...(Args) > 0)
    inline void Error(fmt::format_string<Args...> fmtStr, Args&&... args)
    {
        auto msg = fmt::format(fmtStr, std::forward<Args>(args)...);
        LogMessage(Level::Error, msg);
    }

    // Overloads for pre-formatted strings
    inline void Info(std::string_view msg) { LogMessage(Level::Info, msg); }
    inline void Warn(std::string_view msg) { LogMessage(Level::Warn, msg); }
    inline void Error(std::string_view msg) { LogMessage(Level::Error, msg); }
}
