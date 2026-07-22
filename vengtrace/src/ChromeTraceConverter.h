#pragma once

#include <Veng/Veng.h>

#include "TraceDecoder.h"

// Projects a decoded veng capture onto Chrome Trace Event JSON — the format Perfetto
// (ui.perfetto.dev) and speedscope (speedscope.app) both read. This is a lossy, viewer-facing
// projection, not a second source of truth: the binary capture is the native form, nothing in veng
// or viz ever reads this JSON, and a field the JSON cannot carry is recovered by reading the binary,
// never by extending the projection.
//
// The mapping is fixed: scopes become complete (X) events, counters become C events, instants become
// thread-scoped i events, each track becomes a process thread with a name and a role-ordered sort
// index, the GPU virtual track becomes its own pseudo-thread carrying the back-dated passes, every
// frame becomes a complete event on a dedicated frame track (so a back-dated GPU pass sits under the
// frame it measured), and drop/truncation accounting travels into the output as process metadata.

namespace Veng::VengTrace
{
    /// @brief How scope spans are emitted.
    enum class EventForm : u8
    {
        /// @brief One complete (X) event per scope, carrying ts and dur. The default: half the JSON,
        /// and the form both viewers prefer.
        Complete = 0,
        /// @brief A separate begin (B) and end (E) event per scope.
        Pair = 1,
    };

    /// @brief Options controlling the JSON projection.
    struct ConvertOptions
    {
        /// @brief Emit indented, human-readable JSON rather than the compact default.
        bool Pretty = false;
        /// @brief The scope-span event form.
        EventForm Events = EventForm::Complete;
    };

    /// @brief Converts a decoded capture to a Chrome Trace Event JSON document.
    ///
    /// The output is always well-formed even for a truncated capture: the truncation is recorded in
    /// the process metadata and the trailing (open) frame is emitted as a bare B running to the end
    /// of the trace, rather than being dropped.
    /// @param trace    The decoded capture.
    /// @param options  The projection options.
    /// @return The JSON document text.
    [[nodiscard]] string ConvertToChromeTrace(const DecodedTrace& trace,
                                              const ConvertOptions& options);
}
