#pragma once

// Path.h — assetpack's filesystem-path alias, in a header of its own.
//
// std::filesystem::path is a non-template class whose non-template inline members
// odr-use member templates (wstring/u16string/u32string, root_path, and
// filesystem_error's constructors), so every translation unit that merely parses the
// class definition instantiates them at end of translation. Keeping <filesystem> out
// of Types.h — which AssetId.h pulls into most of the tree — confines that cost to the
// units that actually name a path.
//
// The alias matches engine/include/Veng/Path.h exactly, for the reason Types.h gives:
// assetpack must not pull in the engine's include directory.

#include <filesystem>

namespace Veng
{
    /// @brief Alias for std::filesystem::path.
    using path = std::filesystem::path;
}
