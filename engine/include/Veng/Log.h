#pragma once

#include <string_view>
#include <string>
#include <utility>
#include <functional>
#include <fmt/format.h>

namespace Veng::Log
{
    /// @brief Severity levels for log messages.
    enum class Level
    {
        Info,
        Warn,
        Error
    };

    /// @brief Callback that receives a log level and the pre-formatted message body.
    ///
    /// The sink decides how to present it (timestamp, prefix, etc.). Called on
    /// whatever thread logs; veng is single-threaded; no synchronization is provided.
    using Sink = std::function<void(Level, std::string_view)>;

    /// @brief Replaces the active log sink. Passing nullptr restores the default stdout sink.
    /// @param sink  New sink, or nullptr to restore the default.
    void SetSink(Sink sink);

    /// @brief Sets the minimum level; messages below it are dropped. Defaults to Level::Info.
    /// @param level  Minimum level to forward to the sink.
    void SetMinimumLevel(Level level);

    /// @brief Filters by minimum level, then forwards to the active sink.
    /// @param level    Severity of the message.
    /// @param message  Pre-formatted message body.
    void LogMessage(Level level, std::string_view message);

    /// @brief Logs a formatted Info message. Only enabled when at least one argument is provided.
    template <typename... Args>
        requires(sizeof...(Args) > 0)
    inline void Info(fmt::format_string<Args...> fmtStr, Args&&... args)
    {
        auto msg = fmt::format(fmtStr, std::forward<Args>(args)...);
        LogMessage(Level::Info, msg);
    }

    /// @brief Logs a formatted Warn message. Only enabled when at least one argument is provided.
    template <typename... Args>
        requires(sizeof...(Args) > 0)
    inline void Warn(fmt::format_string<Args...> fmtStr, Args&&... args)
    {
        auto msg = fmt::format(fmtStr, std::forward<Args>(args)...);
        LogMessage(Level::Warn, msg);
    }

    /// @brief Logs a formatted Error message. Only enabled when at least one argument is provided.
    template <typename... Args>
        requires(sizeof...(Args) > 0)
    inline void Error(fmt::format_string<Args...> fmtStr, Args&&... args)
    {
        auto msg = fmt::format(fmtStr, std::forward<Args>(args)...);
        LogMessage(Level::Error, msg);
    }

    /// @brief Logs a pre-formatted Info message.
    inline void Info(std::string_view msg)
    {
        LogMessage(Level::Info, msg);
    }
    /// @brief Logs a pre-formatted Warn message.
    inline void Warn(std::string_view msg)
    {
        LogMessage(Level::Warn, msg);
    }
    /// @brief Logs a pre-formatted Error message.
    inline void Error(std::string_view msg)
    {
        LogMessage(Level::Error, msg);
    }
}
