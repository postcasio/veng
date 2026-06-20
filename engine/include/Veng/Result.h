#pragma once

#include <expected>
#include <string>

namespace Veng
{
    /// @brief Return type for operations that can fail recoverably at runtime.
    ///
    /// Used when a failure (e.g. loading a shader file that may not exist) is
    /// something an application can reasonably handle. The error is a
    /// human-readable string. Non-recoverable conditions (API misuse, device
    /// loss, OOM, unsupported enum/format) are fatal via VE_ASSERT instead.
    /// @tparam T  The success value type.
    /// @see Assert.h
    template <typename T>
    using Result = std::expected<T, std::string>;

    /// @brief Result specialization for operations that succeed with no value.
    using VoidResult = std::expected<void, std::string>;
}
