#pragma once

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/Types.h>

// Shared helper: parse a .vlayout.json file (or the in-memory "elements" array
// from one) into a vector of CookedVertexLayoutElement. Used by both
// VertexLayoutImporter and ShaderImporter (for reflected-input validation).
//
// Format strings are PascalCase Renderer::Format enum spellings:
//   "R32Sfloat"    → 7   (k_FormatR32Sfloat)
//   "RG32Sfloat"   → 8   (k_FormatRG32Sfloat)
//   "RGB32Sfloat"  → 9   (k_FormatRGB32Sfloat)
//   "RGBA32Sfloat" → 10  (k_FormatRGBA32Sfloat)
// Any other string is a located error.

namespace Veng::Cook
{
    // Parse the "elements" array from an already-loaded .vlayout.json object.
    // diagnosticContext is used in error messages (e.g. the source path string).
    [[nodiscard]] Result<vector<CookedVertexLayoutElement>>
    ParseVertexLayoutElements(const json& layoutJson, const string& diagnosticContext);

    // Read and parse a .vlayout.json file, returning its elements.
    [[nodiscard]] Result<vector<CookedVertexLayoutElement>>
    ReadVertexLayoutFile(const path& filePath);
}
