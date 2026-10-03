#include <Veng/Renderer/QueueSubmitMode.h>

#include <cstdlib>

namespace Veng::Renderer
{
    ResolvedQueueSubmitMode ResolveQueueSubmitMode(const QueueSubmitMode requested,
                                                   const char* environmentValue,
                                                   const bool driverConfigurable)
    {
        if (environmentValue != nullptr)
        {
            // Parsed as MoltenVK parses it, so the mode reported is the one the driver runs in.
            const bool synchronous = std::strtod(environmentValue, nullptr) != 0.0;
            return {
                .Mode = synchronous ? QueueSubmitMode::Synchronous : QueueSubmitMode::Asynchronous,
                .Source = QueueSubmitModeSource::Environment,
                .ApplySetting = false,
            };
        }
        if (driverConfigurable)
        {
            return {
                .Mode = requested,
                .Source = QueueSubmitModeSource::Requested,
                .ApplySetting = true,
            };
        }
        return {
            .Mode = QueueSubmitMode::Synchronous,
            .Source = QueueSubmitModeSource::DriverDefault,
            .ApplySetting = false,
        };
    }

    string_view QueueSubmitModeName(const QueueSubmitMode mode)
    {
        switch (mode)
        {
        case QueueSubmitMode::Asynchronous:
            return "asynchronous";
        case QueueSubmitMode::Synchronous:
            return "synchronous";
        }
        return "unknown";
    }

    string_view QueueSubmitModeSourceName(const QueueSubmitModeSource source)
    {
        switch (source)
        {
        case QueueSubmitModeSource::Requested:
            return "requested";
        case QueueSubmitModeSource::Environment:
            return "environment";
        case QueueSubmitModeSource::DriverDefault:
            return "driver default";
        }
        return "unknown";
    }
}
