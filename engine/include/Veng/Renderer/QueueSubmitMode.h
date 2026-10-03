#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief Whether a queue submit returns before the driver has consumed the command buffer.
    ///
    /// Meaningful on a translation layer that builds the native command stream inside the submit
    /// (MoltenVK encodes each Vulkan command buffer into Metal there). Asynchronous hands that
    /// encoding to the driver's own serial queue, so vkQueueSubmit and vkQueuePresentKHR return
    /// at once and the render thread starts the next frame while the last one encodes.
    /// Synchronous encodes inside the call. Either way the work executes in submission order and
    /// signals its fence only on completion, so everything keyed to a fence or a device wait is
    /// unaffected; what changes is that nothing may rely on a submit having *returned* for the
    /// driver to be done reading what the command buffer references.
    enum class QueueSubmitMode : u8
    {
        /// @brief The submit queues the command buffer for encoding and returns.
        Asynchronous,
        /// @brief The submit encodes the command buffer before it returns.
        Synchronous,
    };

    /// @brief Where a context's resolved QueueSubmitMode came from.
    enum class QueueSubmitModeSource : u8
    {
        /// @brief The context's requested mode, applied at instance creation.
        Requested,
        /// @brief The user's environment variable, which overrides the request.
        Environment,
        /// @brief The driver's own default, because the driver offers no way to choose.
        DriverDefault,
    };

    /// @brief A resolved queue-submit mode and the source that decided it.
    struct ResolvedQueueSubmitMode
    {
        /// @brief The mode the driver runs in.
        QueueSubmitMode Mode = QueueSubmitMode::Synchronous;
        /// @brief What decided it.
        QueueSubmitModeSource Source = QueueSubmitModeSource::DriverDefault;
        /// @brief Whether the context should pass Mode to the driver at instance creation.
        ///
        /// False when the environment decides (the driver reads it itself, and a setting passed at
        /// instance creation would override it) and when the driver cannot be told.
        bool ApplySetting = false;
    };

    /// @brief The environment variable a user sets to choose the mode, which wins over the request.
    ///
    /// MoltenVK's own: any value parsing as a non-zero number selects synchronous submission.
    inline constexpr const char* QueueSubmitModeEnvironmentVariable =
        "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS";

    /// @brief Resolves the mode a context runs in, device-free.
    ///
    /// Precedence: a set environment variable wins (a user's debugging choice for the session);
    /// otherwise the request applies when the driver can be told; otherwise the driver's default,
    /// which is synchronous.
    /// @param requested         The mode the context was created asking for.
    /// @param environmentValue  The environment variable's value, or null when it is unset.
    /// @param driverConfigurable Whether the driver accepts the setting at instance creation.
    /// @return The resolved mode, its source, and whether to pass it to the driver.
    [[nodiscard]] VE_API ResolvedQueueSubmitMode ResolveQueueSubmitMode(
        QueueSubmitMode requested, const char* environmentValue, bool driverConfigurable);

    /// @brief Returns the mode's lowercase name ("asynchronous" / "synchronous").
    [[nodiscard]] VE_API string_view QueueSubmitModeName(QueueSubmitMode mode);

    /// @brief Returns the source's lowercase name ("requested" / "environment" / "driver default").
    [[nodiscard]] VE_API string_view QueueSubmitModeSourceName(QueueSubmitModeSource source);
}
