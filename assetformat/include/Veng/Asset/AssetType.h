#pragma once

#include <Veng/Asset/Types.h>

namespace Veng
{
    // What kind of cooked blob a TOC entry points at. Stored in the archive as
    // its underlying u32 (see Archive.h); new types are appended, never
    // renumbered, so old archives keep decoding correctly.
    enum class AssetType : u32
    {
        Raw = 0,
        Texture,
        Mesh,
        Shader,
        Material,
    };
}
