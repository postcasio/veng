#pragma once

#include <Veng/Veng.h>

#include <string_view>
#include <type_traits>

namespace Veng
{
    /// @brief Stable, authored type identifier — the AssetId discipline applied to C++ types.
    ///
    /// Engine builtins carry a hardcoded 0x…ULL literal checked into the source;
    /// game types mint their own with `vengc generate-id`. Because the id is a
    /// literal, not a compiler type-hash, it is a compile-time constant and
    /// byte-identical across the dlopen boundary.
    using TypeId = u64;

    /// @brief Reserved invalid id; every minted id is non-zero.
    inline constexpr TypeId InvalidTypeId = 0;

    /// @brief The closed meta-kind a generic walker switches on.
    ///
    /// Unlike the open TypeId space (any game type adds a new id with no engine
    /// change), this set is fixed: the serializer and editor inspector share
    /// exactly these cases. Reference is an intra-scene Entity reference the
    /// prefab loader remaps; Struct recurses into the field type's own Fields.
    enum class FieldClass : u8
    {
        /// @brief A plain scalar (bool, f32, i32, u32, u64).
        Scalar,
        /// @brief A glm vector (vec2, vec3, vec4).
        Vector,
        /// @brief A glm quaternion (quat).
        Quaternion,
        /// @brief A glm matrix (mat4).
        Matrix,
        /// @brief A std::string / Veng::string.
        String,
        /// @brief An AssetHandle\<T\> — the serialized form is its leading u64 AssetId.
        AssetHandle,
        /// @brief An intra-scene Entity reference remapped by the prefab loader.
        Reference,
        /// @brief A nested struct — recurses into the field type's own Fields.
        Struct,
        /// @brief An enum whose underlying type is a registered leaf.
        Enum,
        /// @brief A tagged union — at most one of several registered alternatives, reached through the TypeInfo variant ops.
        Variant,
        /// @brief A dynamic array (a `vector<T>`) of a single registered element type, walked through the FieldDescriptor's array shims.
        Array,
    };

    /// @brief The closed presentation kind a resolved field maps onto in the editor.
    ///
    /// Orthogonal to FieldClass (which fixes the data shape): a Scalar can be a Drag
    /// or a Slider, a Vector can be a Color. The set is closed, exhaustively switched
    /// like FieldClass; anything outside it is a custom RegisterFieldWidget. It rides
    /// FieldDisplay on both TypeInfo (a type default) and FieldDescriptor (a field
    /// override), merged by ResolveFieldDisplay.
    enum class WidgetKind : u8
    {
        /// @brief Unset/inherit; the editor infers the widget from the field's FieldClass.
        Auto,
        /// @brief A drag editor (the FieldClass default for scalars and vectors).
        Drag,
        /// @brief A bounded slider editor for a scalar or vector.
        Slider,
        /// @brief A color picker for a vec3/vec4.
        Color,
        /// @brief A multi-line text editor for a string.
        Multiline,
    };

    /// @brief A registered type's spelling split into its enclosing namespace and bare name.
    struct QualifiedTypeName
    {
        /// @brief The enclosing namespace path (e.g. "Veng"); empty for a global-namespace type.
        string Namespace;
        /// @brief The bare type name with every namespace qualifier removed (e.g. "vec3", "AssetHandle<Texture>").
        string Name;
    };

    /// @brief Splits a (possibly qualified) C++ type spelling into its namespace and bare name.
    ///
    /// The namespace is the qualifier of the top-level type — the text before the last
    /// "::" that lies outside any template-argument list — with a leading "::" stripped.
    /// The name is the spelling with every namespace qualifier removed, recursing into
    /// template arguments, so "::Veng::AssetHandle<::Veng::Texture>" yields
    /// { Namespace = "Veng", Name = "AssetHandle<Texture>" }. A spelling carrying no
    /// qualifier yields an empty namespace.
    /// @param spelling  The type spelling, e.g. a stringised reflection-macro token.
    /// @return The namespace/name split.
    inline QualifiedTypeName SplitQualifiedTypeName(std::string_view spelling)
    {
        // A leading "::" is the global-scope marker, not a namespace separator.
        if (spelling.size() >= 2 && spelling[0] == ':' && spelling[1] == ':')
        {
            spelling.remove_prefix(2);
        }

        const auto isIdent = [](char c)
        {
            return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9');
        };

        QualifiedTypeName out;

        // Namespace: the qualifier before the last depth-0 "::" (depth counts <...> nesting).
        int depth = 0;
        usize lastSep = std::string_view::npos;
        for (usize i = 0; i + 1 < spelling.size(); ++i)
        {
            const char c = spelling[i];
            if (c == '<')
            {
                ++depth;
            }
            else if (c == '>')
            {
                --depth;
            }
            else if (depth == 0 && c == ':' && spelling[i + 1] == ':')
            {
                lastSep = i;
            }
        }
        if (lastSep != std::string_view::npos)
        {
            out.Namespace = string{spelling.substr(0, lastSep)};
        }

        // Name: drop every "qualifier::" run, keeping the final identifier of each chain.
        out.Name.reserve(spelling.size());
        usize i = 0;
        while (i < spelling.size())
        {
            const char c = spelling[i];
            if (c == ':' && i + 1 < spelling.size() && spelling[i + 1] == ':')
            {
                // A bare separator whose preceding qualifier identifier was already dropped.
                i += 2;
                continue;
            }
            if (isIdent(c))
            {
                usize j = i;
                while (j < spelling.size() && isIdent(spelling[j]))
                {
                    ++j;
                }
                const bool qualifier =
                    j + 1 < spelling.size() && spelling[j] == ':' && spelling[j + 1] == ':';
                if (!qualifier)
                {
                    out.Name.append(spelling.substr(i, j - i));
                }
                i = j;
                continue;
            }
            out.Name.push_back(c);
            ++i;
        }
        return out;
    }

    namespace Detail
    {
        /// @brief True when a type spelling is written fully qualified from global scope (leading "::").
        ///
        /// The reflection macros static_assert on this so every reflected type carries its
        /// namespace. Fundamental types (e.g. `bool`) have no namespace and cannot be
        /// `::`-prefixed, so the macros exempt them via `std::is_fundamental_v`.
        /// @param spelling  The stringised type token.
        /// @return True when the spelling begins with "::".
        consteval bool IsFullyQualifiedSpelling(std::string_view spelling)
        {
            return spelling.size() >= 2 && spelling[0] == ':' && spelling[1] == ':';
        }
    }
}
