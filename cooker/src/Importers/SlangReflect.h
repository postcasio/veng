#pragma once

#include <string_view>

#include <Veng/Asset/Types.h>
#include <Veng/Cook/Types.h>

// Cooker-internal Slang reflection helpers. MaterialImporter cooks shaders
// independently as their own Shader pack entries, but must know the layout of
// the shared MaterialParams struct to validate a material's textures/params and
// pack their values at the right offsets. These functions compile a .slang source
// and reflect either a named struct's field layout or a fragment entry's render-target
// outputs, keeping Slang out of MaterialImporter.cpp.

namespace Veng::Cook
{
    /// @brief One reflected field of a struct: name, byte offset/size within the struct,
    /// component count (1 = scalar, 2/3/4 = vector), and scalar type (float vs. uint).
    struct ReflectedStructField
    {
        /// @brief Field name as declared in the Slang source.
        string Name;
        /// @brief Byte offset of the field within its containing struct.
        u32 Offset = 0;
        /// @brief Byte size of the field.
        u32 Size = 0;
        /// @brief Component count: 1 for scalar, 2/3/4 for vector.
        u32 ComponentCount = 1;
        /// @brief True if the scalar type is float; false if uint.
        bool IsFloat = true;
    };

    /// @brief A reflected struct: total byte size and fields in declaration order.
    struct ReflectedStruct
    {
        /// @brief Total byte size of the struct under uniform layout.
        u32 Size = 0;
        /// @brief Fields in declaration order.
        vector<ReflectedStructField> Fields;
    };

    /// @brief Compiles `slangSource` and reflects the named struct's field layout.
    ///
    /// Returns a located error ("material importer: ...") on compile failure, a
    /// missing struct (unless `optional` is true), or an unsupported field type.
    /// When `optional` is true and the struct is absent, returns an empty
    /// ReflectedStruct (Size 0, no fields) — an author may omit a struct when a
    /// material has no MaterialParams (e.g. a handles-only material).
    /// @param slangSource Path to the .slang source file.
    /// @param structName  Name of the struct to reflect.
    /// @param optional    If true, a missing struct is not an error.
    [[nodiscard]] Result<ReflectedStruct> ReflectStructLayout(const path& slangSource,
                                                              std::string_view structName,
                                                              bool optional = false);

    /// @brief One fragment render target reflected from an entry point's result:
    /// the SV_TargetN index and its scalar/vector component count.
    ///
    /// A material's domain contract compares the collected set against the domain's
    /// expected targets.
    struct ReflectedFragmentOutput
    {
        /// @brief N in SV_TargetN.
        u32 TargetIndex = 0;
        /// @brief Component count: 1 = scalar, 2/3/4 = vector.
        u32 ComponentCount = 1;
        /// @brief True if the scalar type is float; false if uint.
        bool IsFloat = true;
    };

    /// @brief Compiles `slangSource` and reflects the render-target outputs of
    /// fragment entry point `entry`.
    ///
    /// Each SV_TargetN semantic on the result is collected with its scalar/vector
    /// type. Returns a located error ("material importer: ...") on compile failure,
    /// a missing or non-fragment entry point, or an output carrying no SV_Target
    /// semantic. Targets are returned sorted by TargetIndex.
    /// @param slangSource Path to the .slang source file.
    /// @param entry       Name of the fragment entry point to reflect.
    [[nodiscard]] Result<vector<ReflectedFragmentOutput>>
    ReflectFragmentOutputs(const path& slangSource, std::string_view entry);
}
