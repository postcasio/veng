#pragma once

#include <Veng/Log.h>
#include <string_view>
#include <fmt/format.h>

/// @brief Breaks into the debugger if one is attached; otherwise traps.
///
/// Invoked only from the fatal assert path under VE_DEBUG.
#if defined(__clang__)
#define VE_DEBUG_BREAK() __builtin_debugtrap()
#elif defined(_MSC_VER)
#define VE_DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__)
#define VE_DEBUG_BREAK() __builtin_trap()
#else
#include <cstdlib>
#define VE_DEBUG_BREAK() std::abort()
#endif

namespace Veng::Detail
{
    /// @brief Fatal error handler for unrecoverable conditions.
    ///
    /// Handles anything an application cannot sensibly recover from at runtime —
    /// API misuse, device loss, OOM, unsupported enum/format, a failed Vulkan
    /// call. Genuinely recoverable operations (e.g. loading a shader file) return
    /// Veng::Result<T> instead (see Result.h). veng raises no exceptions and
    /// builds with -fno-exceptions.
    ///
    /// Logs the failure through the active log sink, breaks into the debugger in
    /// debug builds, then calls std::abort(). Never returns.
    /// @param file  Source file name (__FILE__).
    /// @param line  Source line number (__LINE__).
    /// @param expr  Stringified condition expression.
    /// @param message  Formatted diagnostic message.
    [[noreturn]] void FatalAssert(const char* file, int line, const char* expr,
                                  std::string_view message);
}

/// @brief Asserts a condition; on failure logs, breaks into the debugger, then aborts.
///
/// Use for unrecoverable conditions (API misuse, device loss, OOM). For recoverable
/// failures, return Veng::Result<T> instead (see Result.h).
/// @param condition  Expression that must be true.
/// @param ...        fmt-style format string + arguments for the diagnostic message.
#define VE_ASSERT(condition, ...)                                                                  \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            ::Veng::Detail::FatalAssert(__FILE__, __LINE__, #condition,                            \
                                        ::fmt::format(__VA_ARGS__));                               \
        }                                                                                          \
    } while (false)
