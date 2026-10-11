#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief The most levels of detail a mesh carries, level 0 included.
    inline constexpr u32 MaxMeshLods = 8;

    /// @brief A forced-level value meaning "no override": the level is selected from screen size.
    inline constexpr i32 AutomaticLod = -1;

    /// @brief A forced-level value meaning "the coarsest level the mesh carries".
    ///
    /// A forced level clamps to the mesh's own coarsest, so this selects it for every mesh.
    inline constexpr i32 CoarsestLod = static_cast<i32>(MaxMeshLods) - 1;
}
