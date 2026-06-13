# 03 — Error handling & logging

**Goal:** remove exceptions from veng entirely. Asserts become fatal
(log + debug-break/abort); genuinely recoverable operations return a
`Result<T>`. Logging grows a sink callback + minimum level so the library stops
owning stdout.

**Dependencies:** none, but do it *early* — every later plan writes error paths,
and they should be written in the new style once, not migrated.

## Current state

- `include/Veng/Assert.h` — `VE_ASSERT` logs then
  `throw std::runtime_error(fmt::format(...))`.
- `include/Veng/Renderer/Backend/Vulkan.h:17-36` — `VK_RAW_ASSERT` / `VK_ASSERT`
  likewise throw `std::runtime_error`.
- `src/Application.cpp:39` — `Run` throws on an invalid working-directory
  argument.
- `src/Log.cpp` — `LogMessage` prints to stdout unconditionally, no level
  filter, no redirection.
- File dialogs (`Window::OpenFileDialog`/`SaveFileDialog`, `Window.h:35-38`)
  already use a `bool` return — the one place with a sane recoverable shape.

## Design

### Fatal path: asserts that don't throw

```cpp
// Assert.h
#define VE_ASSERT(condition, ...)                                  \
    do {                                                           \
        if (!(condition)) {                                        \
            ::Veng::Detail::FatalAssert(__FILE__, __LINE__,        \
                #condition, fmt::format(__VA_ARGS__));             \
        }                                                          \
    } while (false)
```

- `FatalAssert` logs at `Level::Error` (through the sink, below), then
  `VE_DEBUG_BREAK()` (`__builtin_debugtrap()` on Clang/AppleClang,
  `__debugbreak()` on MSVC) when a debugger is attached / in `VE_DEBUG` builds,
  then `std::abort()`. Marked `[[noreturn]]` so control-flow analysis still
  works at call sites that relied on `throw` not returning.
- `VK_ASSERT` / `VK_RAW_ASSERT` in `Vulkan.h` route to the same `FatalAssert`
  with the `string_VkResult` text folded into the message.
- Add `VE_VERIFY` (checked in all builds — this is what `VE_ASSERT` becomes) if
  a debug-only variant is wanted later; not required now since current
  `VE_ASSERT` semantics are always-checked.
- Build with `-fno-exceptions` once no `throw` remains
  (`target_compile_options(veng PRIVATE -fno-exceptions)`) — this is the
  enforcement mechanism, not just a flag. Check vendor TUs tolerate it
  (tinyexr has `TINYEXR_USE_EXCEPTIONS=0`; stb is fine; ImGui is fine).

### Recoverable path: `Result<T>`

```cpp
// Veng/Result.h
template <typename T> using Result = std::expected<T, string>;
using VoidResult = std::expected<void, string>;
```

- `std::expected` (C++23, available under the project's C++26 mode) with a
  string error is enough for now; introduce a structured error type only when a
  caller needs to branch on error kind.
- Apply to operations that are genuinely recoverable today:
  - `Shader::Create(const ShaderInfo&)` — file-not-found / bad SPIR-V should
    not abort the process once hot-reload exists. Return
    `Result<Unique<Shader>>`. (Callers that want the old behavior write
    `VE_ASSERT(result, "{}", result.error())`.)
  - `Window::OpenFileDialog`/`SaveFileDialog` — convert `bool + out-param` to
    `Result<string>` (or keep as-is and convert in plan 06 when their
    signatures change anyway — implementer's choice, note it in the PR).
  - Image file loading paths in `Image.cpp` (stb/tinyexr decode failures).
- Everything else — device loss, OOM, allocation failure, API misuse — is
  fatal by policy. Document the policy in `Assert.h`.

### Logging

```cpp
// Log.h additions
namespace Veng::Log {
    using Sink = function<void(Level, std::string_view)>;
    void SetSink(Sink sink);      // nullptr restores the default stdout sink
    void SetMinimumLevel(Level level);
}
```

- `src/Log.cpp` — `LogMessage` filters by minimum level, then forwards to the
  installed sink; default sink is the current stdout print. Document that the
  sink is called on whatever thread logs (single-threaded contract for now).
- `FatalAssert` logs through the sink *before* breaking, so a consumer's
  future console window sees assert messages too.

## Migration mechanics

- `grep -rn "throw\|std::runtime_error\|stdexcept" src/ include/` and convert
  every site: most become `VE_ASSERT(false, ...)`/`VK_ASSERT`; `Shader` and
  image loading become `Result`.
- `Application::Run`'s invalid-directory throw (`src/Application.cpp:39`)
  becomes a `VE_ASSERT` (bad CLI arg = fatal, fine).
- Remove `#include <stdexcept>` from `Assert.h` and `Vulkan.h`.

## Acceptance

- `grep -rn "throw " src/ include/` → no hits; veng compiles with
  `-fno-exceptions`.
- A failed `VK_ASSERT` logs through a custom sink installed via `SetSink`, then
  aborts (manually verified by forcing a failure).
- `Shader::Create` with a missing file returns an error `Result` instead of
  terminating.
