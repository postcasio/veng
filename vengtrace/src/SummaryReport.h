#pragma once

#include <Veng/Veng.h>

#include "Summary.h"

// Renders a summary and a comparison as plain text for a terminal, or as a JSON document carrying
// the same figures for a script. The two forms carry the same content; the text form only trims
// the ranked tables to the summary's own row counts, which the JSON form already obeys.

namespace Veng::VengTrace
{
    /// @brief Renders a summary as aligned plain-text tables.
    /// @param summary  The summary.
    /// @param capture  The capture's label (its path), printed in the header.
    /// @return The text, newline-terminated.
    [[nodiscard]] string FormatSummary(const CaptureSummary& summary, string_view capture);

    /// @brief Renders a summary as a JSON document; durations are milliseconds.
    /// @param summary  The summary.
    /// @param capture  The capture's label (its path).
    /// @return The document text, indented.
    [[nodiscard]] string SummaryToJson(const CaptureSummary& summary, string_view capture);

    /// @brief Renders a comparison as a plain-text table.
    /// @param comparison  The comparison.
    /// @param a           The first capture's label.
    /// @param b           The second capture's label.
    /// @param top         The rows to print, or 0 for every common scope.
    /// @return The text, newline-terminated.
    [[nodiscard]] string FormatComparison(const Comparison& comparison, string_view a,
                                          string_view b, u32 top);

    /// @brief Renders a comparison as a JSON document; durations are milliseconds.
    /// @param comparison  The comparison.
    /// @param a           The first capture's label.
    /// @param b           The second capture's label.
    /// @return The document text, indented.
    [[nodiscard]] string ComparisonToJson(const Comparison& comparison, string_view a,
                                          string_view b);
}
