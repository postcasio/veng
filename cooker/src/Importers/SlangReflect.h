#pragma once

#include <string_view>

#include <Veng/Asset/Types.h>
#include <Veng/Cook/Types.h>

// Cooker-internal Slang reflection helper. The
// MaterialImporter does not compile shaders — they are cooked independently as
// their own Shader pack entries — but it must know the layout of
// the shared MaterialParams struct to validate a material's textures/params and
// pack their values at the right offsets. This compiles a .slang source and
// reflects one named struct's fields with their byte offsets, keeping Slang out
// of MaterialImporter.cpp.

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

    // A reflected struct: its total byte size and its fields in declaration order.
    struct ReflectedStruct
    {
        u32 Size = 0;
        vector<ReflectedStructField> Fields;
    };

    // Compiles `slangSource` and reflects struct `structName`. Located error
    // ("material importer: ...") on a compile failure, a missing struct, or an
    // unsupported field type. When `optional` is true a missing struct is not an
    // error — it returns an empty ReflectedStruct (Size 0, no fields) — so an
    // author may omit a struct (e.g. a handles-only material has no MaterialParams).
    [[nodiscard]] Result<ReflectedStruct> ReflectStructLayout(
        const path& slangSource, std::string_view structName, bool optional = false);

    // One fragment render target reflected from an entry point's result: the
    // SV_TargetN index its semantic names and its scalar/vector component count
    // (1 = scalar, 4 = float4). A material's domain↔output contract compares the
    // collected set against the domain's expected targets.
    struct ReflectedFragmentOutput
    {
        u32 TargetIndex = 0;    // N in SV_TargetN
        u32 ComponentCount = 1; // 1 = scalar, 2/3/4 = vector
        bool IsFloat = true;
    };

    // Compiles `slangSource` and reflects the render-target outputs of fragment
    // entry point `entry` — each SV_TargetN semantic on its result, collected with
    // its scalar/vector type. Located error ("material importer: ...") on a compile
    // failure, a missing or non-fragment entry point, or an output carrying no
    // SV_Target semantic. The targets come back sorted by TargetIndex.
    [[nodiscard]] Result<vector<ReflectedFragmentOutput>> ReflectFragmentOutputs(
        const path& slangSource, std::string_view entry);
}
