#pragma once

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/Types.h>

// Shared helper: parse a .vlayout.json file (or the in-memory "elements" array
// from one) into a vector of CookedVertexLayoutElement. Used by both
// VertexLayoutImporter and ShaderImporter (for reflected-input validation).
//
// Format strings are PascalCase Renderer::Format enum spellings:
//   "R32Sfloat"    → 7   (FormatR32Sfloat)
//   "RG32Sfloat"   → 8   (FormatRG32Sfloat)
//   "RGB32Sfloat"  → 9   (FormatRGB32Sfloat)
//   "RGBA32Sfloat" → 10  (FormatRGBA32Sfloat)
// Any other string is a located error.

namespace Veng::Cook
{
    /// @brief Parses the "elements" array from an already-loaded .vlayout.json object.
    ///
    /// @param layoutJson        The parsed JSON object containing the "elements" array.
    /// @param diagnosticContext Context string used in located error messages (e.g. source path).
    [[nodiscard]] Result<vector<CookedVertexLayoutElement>>
    ParseVertexLayoutElements(const json& layoutJson, const string& diagnosticContext);

    /// @brief Reads and parses a .vlayout.json file, returning its elements.
    ///
    /// @param filePath Absolute path to the .vlayout.json file.
    [[nodiscard]] Result<vector<CookedVertexLayoutElement>>
    ReadVertexLayoutFile(const path& filePath);
}
