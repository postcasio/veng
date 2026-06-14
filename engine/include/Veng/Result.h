#pragma once

#include <expected>
#include <string>

namespace Veng
{
    // Return type for operations that can fail in a way an application may
    // sensibly recover from at runtime (e.g. loading a shader file that may not
    // exist). The error is a human-readable string for now; a structured error
    // type can be introduced if a caller ever needs to branch on error kind.
    //
    // Everything that is *not* recoverable — API misuse, device loss, OOM,
    // unsupported enum/format — is fatal via VE_ASSERT instead. See Assert.h.
    template <typename T>
    using Result = std::expected<T, std::string>;

    using VoidResult = std::expected<void, std::string>;
}
