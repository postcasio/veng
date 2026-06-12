#pragma once

#include <Veng/Log.h>
#include <string_view>
#include <fmt/format.h>

// Break into the debugger if one is attached, otherwise it is a no-op-ish trap.
// Only invoked from the fatal path in debug builds.
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
    // veng's error policy: anything an application cannot sensibly recover from
    // at runtime — API misuse, device loss, OOM, unsupported enum/format, a
    // failed Vulkan call — is fatal and routed here. Genuinely recoverable
    // operations (e.g. loading a shader file) return Veng::Result<T> instead
    // (see Result.h). veng raises no exceptions and builds with -fno-exceptions.
    //
    // FatalAssert logs the failure through the active log sink, breaks into the
    // debugger in debug builds, then calls std::abort(). It never returns, so
    // control-flow analysis at call sites that previously relied on `throw` not
    // returning still holds.
    [[noreturn]] void FatalAssert(const char* file, int line, const char* expr,
                                  std::string_view message);
}

#define VE_ASSERT(condition, ...)                                              \
    do                                                                         \
    {                                                                          \
        if (!(condition))                                                      \
        {                                                                      \
            ::Veng::Detail::FatalAssert(__FILE__, __LINE__, #condition,        \
                                        ::fmt::format(__VA_ARGS__));           \
        }                                                                      \
    } while (false)
