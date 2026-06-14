#pragma once

#include <string_view>

#include <Veng/Asset/Types.h>
#include <Veng/Cook/Types.h>

// Cooker-internal Slang reflection helper (planset-5 plan 09). The
// MaterialImporter does not compile shaders — they are cooked independently as
// their own Shader pack entries (decision 3) — but it must know the layout of
// the shared MaterialData struct to validate a material's textures/params and
// pack their values at the right offsets. This compiles a .slang source and
// reflects one named struct's fields with their byte offsets (the std430/std140
// image agree for the padded v1 MaterialData), keeping Slang out of
// MaterialImporter.cpp.

namespace Veng::Cook
{
    // One reflected field of a struct: its name, byte offset/size within the
    // struct, component count (1 = scalar, 2/3/4 = vector), and whether it is a
    // float (vs. a u32 — a texture/sampler handle or a uint param).
    struct ReflectedStructField
    {
        string Name;
        u32 Offset = 0;
        u32 Size = 0;
        u32 ComponentCount = 1;
        bool IsFloat = true;
    };

    // A reflected struct: its total byte size (the param-block size the cooker
    // writes as CookedMaterialHeader::ParamBytes, which the engine asserts
    // equals sizeof(its MaterialData mirror)) and its fields in declaration order.
    struct ReflectedStruct
    {
        u32 Size = 0;
        vector<ReflectedStructField> Fields;
    };

    // Compiles `slangSource` and reflects struct `structName`. Located error
    // ("material importer: ...") on a compile failure, a missing struct, or an
    // unsupported field type.
    [[nodiscard]] Result<ReflectedStruct> ReflectStructLayout(
        const path& slangSource, std::string_view structName);
}
