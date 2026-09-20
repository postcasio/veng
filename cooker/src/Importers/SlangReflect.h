#pragma once

#include <string_view>

#include <Veng/Asset/Types.h>
#include <Veng/Cook/Types.h>

#include "SlangSession.h"

// Cooker-internal Slang reflection helpers. MaterialImporter cooks shaders
// independently as their own Shader pack entries, but must know the layout of
// the shared MaterialParams struct to validate a material's textures/params and
// pack their values at the right offsets. These functions compile a Slang source
// (a .slang file or graph-generated text, via SlangModuleSource) and reflect
// either a named struct's field layout or a fragment entry's render-target
// outputs, keeping Slang out of MaterialImporter.cpp.

namespace Veng::Cook
{
    /// @brief One reflected field of a struct: name, byte offset/size within the struct,
    /// element count and stride, component count (1 = scalar, 2/3/4 = vector), and scalar type
    /// (float vs. uint).
    struct ReflectedStructField
    {
        /// @brief Field name as declared in the Slang source.
        ///
        /// A member reached through a nested struct carries its dotted path, and an element of a
        /// struct array carries its subscript: `Bands[1].Low`. A leaf array keeps its plain member
        /// name and expresses its arity in ElementCount instead.
        string Name;
        /// @brief Byte offset of the field within its containing struct, in scalar/tight layout
        /// (the layout a shader's ByteAddressBuffer.Load<T> reads — 4-byte packed, vectors not
        /// 16-aligned).
        u32 Offset = 0;
        /// @brief Byte size of the whole field: ElementCount * ElementStride, no padding.
        u32 Size = 0;
        /// @brief Number of array elements; 1 for a plain scalar or vector member.
        u32 ElementCount = 1;
        /// @brief Byte stride between array elements: ComponentCount 4-byte components.
        ///
        /// The tight stride, which is what Load<T> reads — a `float3[3]` strides by 12, not by a
        /// std430-style 16. Equals Size for a non-array member.
        u32 ElementStride = 0;
        /// @brief Component count of one element: 1 for scalar, 2/3/4 for vector.
        u32 ComponentCount = 1;
        /// @brief True if the scalar type is float; false if uint.
        bool IsFloat = true;
    };

    /// @brief A reflected struct: total byte size and fields in declaration order.
    struct ReflectedStruct
    {
        /// @brief Total byte size of the struct in scalar/tight layout — the sum of the fields'
        /// component spans, matching the byte extent a ByteAddressBuffer.Load<T> reads.
        u32 Size = 0;
        /// @brief Fields in declaration order, nested structs flattened into it.
        vector<ReflectedStructField> Fields;
    };

    /// @brief Compiles `slangSource` and reflects the named struct's field layout.
    ///
    /// A member that is an array of a scalar or vector is one field carrying its element count and
    /// stride. A nested struct is flattened into dotted leaves, and a struct array is flattened per
    /// element, so the returned list holds only scalar/vector leaves and leaf arrays.
    ///
    /// Returns a located error ("material importer: ...") on compile failure, a
    /// missing struct (unless `optional` is true), or an unsupported field type.
    /// When `optional` is true and the struct is absent, returns an empty
    /// ReflectedStruct (Size 0, no fields) — an author may omit a struct when a
    /// material has no MaterialParams (e.g. a handles-only material).
    /// @param slangSource The Slang module source (a .slang file or graph-generated text).
    /// @param structName  Name of the struct to reflect.
    /// @param shaderIncludeDir Engine core shader dir added to the Slang search path so the
    ///                         source resolves `#include "Veng/surface.slang"`; empty to skip it.
    /// @param optional    If true, a missing struct is not an error.
    [[nodiscard]] Result<ReflectedStruct> ReflectStructLayout(const SlangModuleSource& slangSource,
                                                              std::string_view structName,
                                                              const path& shaderIncludeDir = {},
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
    /// type. An SV_Depth member is skipped — it writes the depth attachment rather than a
    /// color one, so it belongs to no domain's target set. Returns a located error
    /// ("material importer: ...") on compile failure, a missing or non-fragment entry
    /// point, or an output carrying neither semantic. Targets are returned sorted by
    /// TargetIndex.
    /// @param slangSource The Slang module source (a .slang file or graph-generated text).
    /// @param entry       Name of the fragment entry point to reflect.
    /// @param shaderIncludeDir Engine core shader dir added to the Slang search path so the
    ///                         source resolves `#include "Veng/surface.slang"`; empty to skip it.
    [[nodiscard]] Result<vector<ReflectedFragmentOutput>>
    ReflectFragmentOutputs(const SlangModuleSource& slangSource, std::string_view entry,
                           const path& shaderIncludeDir = {});
}
