#pragma once

#include <string_view>
#include <string>
#include <utility>
#include <fmt/format.h>

namespace Veng::Log
{
    enum class Level { Info, Warn, Error };

    // Core sink that emits a fully formatted message
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
