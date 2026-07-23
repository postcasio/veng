#pragma once

// Path.h — the house filesystem-path alias, in a header of its own.
//
// std::filesystem::path is a non-template class whose non-template inline members
// odr-use member templates (wstring/u16string/u32string, root_path, and
// filesystem_error's constructors), so every translation unit that merely parses the
// class definition instantiates them at end of translation. Keeping <filesystem> out
// of Veng.h — the precompiled header for every target, and the header a downstream
// project's own precompiled header carries — confines that cost to the units that
// actually name a path.

#include <filesystem>

namespace Veng
{
    /// @brief House alias for std::filesystem::path.
    using path = std::filesystem::path;
}
