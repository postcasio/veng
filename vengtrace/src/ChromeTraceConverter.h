#pragma once

#include <Veng/Veng.h>

#include "TraceDecoder.h"

// Projects a decoded veng capture onto Chrome Trace Event JSON — the format Perfetto
// (ui.perfetto.dev) and speedscope (speedscope.app) both read. This is a lossy, viewer-facing
// projection, not a second source of truth: the binary capture is the native form, nothing in veng
// or viz ever reads this JSON, and a field the JSON cannot carry is recovered by reading the binary,
// never by extending the projection.
//
// The mapping is fixed: a thread lane's scopes become complete (X) events, counters become C events,
// instants become thread-scoped i events, each track becomes a process thread with a name and a
// role-ordered sort index, the GPU virtual track becomes its own pseudo-thread carrying the
// back-dated passes, every frame becomes a span on a dedicated frame track (so a back-dated GPU pass
// sits under the frame it measured), and drop/truncation accounting travels into the output as
// process metadata.
//
// The frame ruler and the virtual lanes are emitted as nestable async slices (b/e) rather than
// complete events. A complete event carries no parenthood, so a viewer infers nesting from timestamp
// containment and cannot resolve it where two spans on one lane partially overlap — which they do on
// both of those lanes: pipelining makes consecutive frames and consecutive GPU frames overlap, and
// sibling passes with no barrier between them interleave on hardware. An async cookie states the
// parenthood instead, so overlap between cookies is legal rather than ambiguous.

namespace Veng::VengTrace
{
    /// @brief How a thread lane's scope spans are emitted.
    ///
    /// Applies to thread lanes only. The frame ruler and the virtual lanes are always async slices,
    /// which already carry a separate begin and end.
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
        /// @brief The thread-lane scope-span event form.
        EventForm Events = EventForm::Complete;
    };

    /// @brief Converts a decoded capture to a Chrome Trace Event JSON document.
    ///
    /// The output is always well-formed even for a truncated capture: the truncation is recorded in
    /// the process metadata and the trailing (open) frame is emitted as an async begin with no
    /// matching end, running to the end of the trace, rather than being dropped.
    /// @param trace    The decoded capture.
    /// @param options  The projection options.
    /// @return The JSON document text.
    [[nodiscard]] string ConvertToChromeTrace(const DecodedTrace& trace,
                                              const ConvertOptions& options);
}
