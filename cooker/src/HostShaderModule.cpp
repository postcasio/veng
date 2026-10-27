#include <Veng/Cook/HostShaderModule.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <span>
#include <system_error>

#include <fmt/format.h>

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include <Veng/Asset/AtomicFile.h>
#include <Veng/Cook/Cooker.h>

#include "Importers/SlangSession.h"

namespace Veng::Cook
{
    namespace
    {
        using Slang::ComPtr;

        string DiagnosticsText(slang::IBlob* diagnostics)
        {
            if (!diagnostics || diagnostics->getBufferSize() == 0)
            {
                return "";
            }
            return {static_cast<const char*>(diagnostics->getBufferPointer()),
                    diagnostics->getBufferSize()};
        }

        bool IsIdentifier(const string& name)
        {
            if (name.empty() || (std::isdigit(static_cast<unsigned char>(name[0])) != 0))
            {
                return false;
            }
            return std::ranges::all_of(
                name, [](const char c)
                { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
        }

        /// @brief A scalar type that may cross the host boundary.
        enum class HostScalar : u8
        {
            F32,
            F64,
            I32,
            U32,
        };

        /// @brief The shape of a type that may cross the host boundary.
        enum class HostKind : u8
        {
            Scalar,
            Vector,
            Matrix,
            Array,
            Struct,
        };

        /// @brief A type that may cross the host boundary.
        ///
        /// A scalar, a 2- to 4-vector or a float/double matrix of one, a fixed-size array of any
        /// of these, or a struct whose fields are any of these.
        struct HostType
        {
            /// @brief The type's shape.
            HostKind Kind = HostKind::Scalar;
            /// @brief The scalar, or the vector's or matrix's element.
            HostScalar Scalar = HostScalar::F32;
            /// @brief The vector's component count, or the array's length; 1 for a scalar.
            u32 Count = 1;
            /// @brief The matrix's row count.
            u32 Rows = 0;
            /// @brief The matrix's column count.
            u32 Columns = 0;
            /// @brief The array's element type, alone.
            vector<HostType> Element;
            /// @brief The struct's index in the module's struct table.
            u32 Struct = 0;
        };

        /// @brief One field of a struct crossing the host boundary.
        struct HostField
        {
            /// @brief The field's Slang name, kept in the C++ struct.
            string Name;
            /// @brief Its type.
            HostType Type;
        };

        /// @brief A struct crossing the host boundary, mirrored field for field in the header.
        struct HostStruct
        {
            /// @brief The struct's Slang name, which names the C++ struct.
            string Name;
            /// @brief Its fully qualified Slang name, which tells two same-named structs apart.
            string FullName;
            /// @brief The fields in declaration order.
            vector<HostField> Fields;
        };

        /// @brief How an exported function's parameter passes.
        enum class HostDirection : u8
        {
            In,
            Out,
            InOut,
        };

        /// @brief One parameter of an exported function.
        struct HostParameter
        {
            /// @brief The parameter's Slang name, kept in the C++ signature.
            string Name;
            /// @brief Its type.
            HostType Type;
            /// @brief Whether it is read, written, or both.
            HostDirection Direction = HostDirection::In;
        };

        /// @brief One exported function of a host module.
        struct HostFunction
        {
            /// @brief The function's name, unmangled in the emitted C++.
            string Name;
            /// @brief The return type, or nullopt for void.
            optional<HostType> Return;
            /// @brief The parameters in declaration order.
            vector<HostParameter> Parameters;
        };

        /// @brief Whether a type crosses as the prelude's own scalar or vector, by value.
        bool IsSimple(const HostType& type)
        {
            return type.Kind == HostKind::Scalar || type.Kind == HostKind::Vector;
        }

        optional<HostScalar> MapScalar(slang::TypeReflection::ScalarType scalar)
        {
            switch (scalar)
            {
            case slang::TypeReflection::ScalarType::Float32:
                return HostScalar::F32;
            case slang::TypeReflection::ScalarType::Float64:
                return HostScalar::F64;
            case slang::TypeReflection::ScalarType::Int32:
                return HostScalar::I32;
            case slang::TypeReflection::ScalarType::UInt32:
                return HostScalar::U32;
            default:
                return std::nullopt;
            }
        }

        string TypeDisplayName(slang::TypeReflection* type)
        {
            if (!type)
            {
                return "<unnamed type>";
            }
            if (type->getKind() == slang::TypeReflection::Kind::Array)
            {
                return fmt::format("{}[{}]", TypeDisplayName(type->getElementType()),
                                   type->getElementCount());
            }
            if (!type->getName())
            {
                return "<unnamed type>";
            }
            if (type->getKind() == slang::TypeReflection::Kind::Vector)
            {
                return fmt::format("vector<{}, {}>",
                                   type->getElementType() && type->getElementType()->getName()
                                       ? type->getElementType()->getName()
                                       : "?",
                                   type->getElementCount());
            }
            return type->getName();
        }

        string FullTypeName(slang::TypeReflection* type)
        {
            ComPtr<ISlangBlob> name;
            if (SLANG_FAILED(type->getFullName(name.writeRef())) || !name)
            {
                return type->getName() ? type->getName() : "";
            }
            return {static_cast<const char*>(name->getBufferPointer()), name->getBufferSize()};
        }

        // Maps a reflected type onto the host's, adding each struct it reaches to `structs` after
        // the structs its fields name. The error is empty when the type itself cannot cross, and
        // names the field path when something inside it cannot.
        Result<HostType> MapType(slang::TypeReflection* type, vector<HostStruct>& structs)
        {
            if (!type)
            {
                return std::unexpected("");
            }
            switch (type->getKind())
            {
            case slang::TypeReflection::Kind::Scalar:
            {
                const optional<HostScalar> scalar = MapScalar(type->getScalarType());
                if (!scalar)
                {
                    return std::unexpected("");
                }
                return HostType{.Kind = HostKind::Scalar, .Scalar = *scalar};
            }
            case slang::TypeReflection::Kind::Vector:
            {
                const usize count = type->getElementCount();
                const optional<HostScalar> scalar =
                    type->getElementType() ? MapScalar(type->getElementType()->getScalarType())
                                           : std::nullopt;
                if (count < 2 || count > 4 || !scalar)
                {
                    return std::unexpected("");
                }
                return HostType{
                    .Kind = HostKind::Vector, .Scalar = *scalar, .Count = static_cast<u32>(count)};
            }
            case slang::TypeReflection::Kind::Matrix:
            {
                const u32 rows = type->getRowCount();
                const u32 columns = type->getColumnCount();
                const optional<HostScalar> scalar =
                    type->getElementType() ? MapScalar(type->getElementType()->getScalarType())
                                           : std::nullopt;
                // glm spells float and double matrices only.
                if (rows < 2 || rows > 4 || columns < 2 || columns > 4 || !scalar ||
                    (*scalar != HostScalar::F32 && *scalar != HostScalar::F64))
                {
                    return std::unexpected("");
                }
                return HostType{
                    .Kind = HostKind::Matrix, .Scalar = *scalar, .Rows = rows, .Columns = columns};
            }
            case slang::TypeReflection::Kind::Array:
            {
                const usize count = type->getElementCount();
                if (count == 0 || count == SLANG_UNBOUNDED_SIZE || count > 0xFFFFFFFFu)
                {
                    return std::unexpected("");
                }
                Result<HostType> element = MapType(type->getElementType(), structs);
                if (!element)
                {
                    return element;
                }
                HostType array{.Kind = HostKind::Array, .Count = static_cast<u32>(count)};
                array.Element.push_back(std::move(*element));
                return array;
            }
            case slang::TypeReflection::Kind::Struct:
            {
                const string fullName = FullTypeName(type);
                for (u32 i = 0; i < structs.size(); ++i)
                {
                    if (structs[i].FullName == fullName)
                    {
                        return HostType{.Kind = HostKind::Struct, .Struct = i};
                    }
                }
                HostStruct host{.Name = type->getName() ? type->getName() : "",
                                .FullName = fullName};
                if (!IsIdentifier(host.Name))
                {
                    return std::unexpected(
                        fmt::format("struct '{}' has no C++ identifier for a name", fullName));
                }
                if (type->getFieldCount() == 0)
                {
                    return std::unexpected(fmt::format("struct '{}' has no fields", host.Name));
                }
                for (u32 f = 0; f < type->getFieldCount(); ++f)
                {
                    slang::VariableReflection* field = type->getFieldByIndex(f);
                    const string fieldName = field->getName() ? field->getName() : "";
                    Result<HostType> fieldType = MapType(field->getType(), structs);
                    if (!fieldType)
                    {
                        return std::unexpected(fieldType.error().empty()
                                                   ? fmt::format("field '{}.{}' has type '{}'",
                                                                 host.Name, fieldName,
                                                                 TypeDisplayName(field->getType()))
                                                   : fieldType.error());
                    }
                    host.Fields.push_back({.Name = fieldName, .Type = std::move(*fieldType)});
                }
                // Checked after the fields, which may add structs of their own.
                for (const HostStruct& other : structs)
                {
                    if (other.Name == host.Name)
                    {
                        return std::unexpected(fmt::format(
                            "structs '{}' and '{}' would share the C++ name '{}'; give one of "
                            "them another name",
                            other.FullName, fullName, host.Name));
                    }
                }
                structs.push_back(std::move(host));
                return HostType{.Kind = HostKind::Struct,
                                .Struct = static_cast<u32>(structs.size() - 1)};
            }
            default:
                return std::unexpected("");
            }
        }

        bool IsVoid(slang::TypeReflection* type)
        {
            return type && type->getKind() == slang::TypeReflection::Kind::Scalar &&
                   type->getScalarType() == slang::TypeReflection::ScalarType::Void;
        }

        const char* ScalarCpp(HostScalar scalar)
        {
            switch (scalar)
            {
            case HostScalar::F32:
                return "float";
            case HostScalar::F64:
                return "double";
            case HostScalar::I32:
                return "std::int32_t";
            case HostScalar::U32:
                return "std::uint32_t";
            }
            return "float";
        }

        const char* ScalarSlang(HostScalar scalar)
        {
            switch (scalar)
            {
            case HostScalar::F32:
                return "float";
            case HostScalar::F64:
                return "double";
            case HostScalar::I32:
                return "int";
            case HostScalar::U32:
                return "uint";
            }
            return "float";
        }

        // The spelling the Slang C++ prelude uses for a scalar or vector.
        string PreludeCpp(const HostType& type)
        {
            const char* scalar = type.Scalar == HostScalar::I32   ? "int32_t"
                                 : type.Scalar == HostScalar::U32 ? "uint32_t"
                                                                  : ScalarCpp(type.Scalar);
            if (type.Kind == HostKind::Scalar)
            {
                return scalar;
            }
            return fmt::format("Vector<{}, {}>", scalar, type.Count);
        }

        // The C++ type a host caller passes; a struct is prefixed with `scope`.
        string GlmCpp(const HostType& type, const vector<HostStruct>& structs,
                      const string& scope = "")
        {
            switch (type.Kind)
            {
            case HostKind::Scalar:
                return ScalarCpp(type.Scalar);
            case HostKind::Vector:
            {
                const char* prefix = type.Scalar == HostScalar::F32   ? ""
                                     : type.Scalar == HostScalar::F64 ? "d"
                                     : type.Scalar == HostScalar::I32 ? "i"
                                                                      : "u";
                return fmt::format("glm::{}vec{}", prefix, type.Count);
            }
            case HostKind::Matrix:
            {
                // glm names a matrix columns first: Slang's float3x2 (3 rows) is glm::mat2x3.
                const char* prefix = type.Scalar == HostScalar::F64 ? "d" : "";
                if (type.Rows == type.Columns)
                {
                    return fmt::format("glm::{}mat{}", prefix, type.Rows);
                }
                return fmt::format("glm::{}mat{}x{}", prefix, type.Columns, type.Rows);
            }
            case HostKind::Array:
                return fmt::format("std::array<{}, {}>", GlmCpp(type.Element[0], structs, scope),
                                   type.Count);
            case HostKind::Struct:
                return scope + structs[type.Struct].Name;
            }
            return "float";
        }

        string SlangSpelling(const HostType& type, const vector<HostStruct>& structs)
        {
            switch (type.Kind)
            {
            case HostKind::Scalar:
                return ScalarSlang(type.Scalar);
            case HostKind::Vector:
                return fmt::format("{}{}", ScalarSlang(type.Scalar), type.Count);
            case HostKind::Matrix:
                return fmt::format("{}{}x{}", ScalarSlang(type.Scalar), type.Rows, type.Columns);
            case HostKind::Array:
                return fmt::format("{}[{}]", SlangSpelling(type.Element[0], structs), type.Count);
            case HostKind::Struct:
                return structs[type.Struct].Name;
            }
            return "float";
        }

        constexpr std::array<const char*, 4> Components{"x", "y", "z", "w"};

        // `value` (a glm vector or scalar expression) converted to the prelude's type.
        string ToPrelude(const HostType& type, const string& value)
        {
            if (type.Kind == HostKind::Scalar)
            {
                return value;
            }
            string args;
            for (u32 i = 0; i < type.Count; ++i)
            {
                args += fmt::format("{}{}.{}", i == 0 ? "" : ", ", value, Components[i]);
            }
            return fmt::format("{}({})", PreludeCpp(type), args);
        }

        // `value` (a prelude vector or scalar expression) converted to the glm type.
        string ToGlm(const HostType& type, const string& value, const vector<HostStruct>& structs)
        {
            if (type.Kind == HostKind::Scalar)
            {
                return value;
            }
            string args;
            for (u32 i = 0; i < type.Count; ++i)
            {
                args += fmt::format("{}{}.{}", i == 0 ? "" : ", ", value, Components[i]);
            }
            return fmt::format("{}({})", GlmCpp(type, structs), args);
        }

        string HeaderSignature(const HostFunction& function, const vector<HostStruct>& structs)
        {
            string params;
            for (const HostParameter& parameter : function.Parameters)
            {
                if (!params.empty())
                {
                    params += ", ";
                }
                const bool byValue =
                    parameter.Direction == HostDirection::In && IsSimple(parameter.Type);
                if (parameter.Direction == HostDirection::In && !byValue)
                {
                    params += "const ";
                }
                params += GlmCpp(parameter.Type, structs);
                params += byValue ? " " : "& ";
                params += parameter.Name;
            }
            return fmt::format("{} {}({})",
                               function.Return ? GlmCpp(*function.Return, structs) : "void",
                               function.Name, params);
        }

        string SlangSignature(const HostFunction& function, const vector<HostStruct>& structs)
        {
            string params;
            for (const HostParameter& parameter : function.Parameters)
            {
                if (!params.empty())
                {
                    params += ", ";
                }
                if (parameter.Direction == HostDirection::Out)
                {
                    params += "out ";
                }
                else if (parameter.Direction == HostDirection::InOut)
                {
                    params += "inout ";
                }
                params += SlangSpelling(parameter.Type, structs);
                params += ' ';
                params += parameter.Name;
            }
            return fmt::format("{} {}({})",
                               function.Return ? SlangSpelling(*function.Return, structs) : "void",
                               function.Name, params);
        }

        string BuildHeader(const string& name, const path& module,
                           const vector<HostStruct>& structs, const vector<HostFunction>& functions)
        {
            string out = fmt::format("#pragma once\n"
                                     "\n"
                                     "// Generated by vengc compile-host from {}. Do not edit.\n"
                                     "\n"
                                     "#include <array>\n"
                                     "#include <cstdint>\n"
                                     "\n"
                                     "#include <glm/glm.hpp>\n"
                                     "\n"
                                     "/// @brief The host build of the Slang module {}.\n"
                                     "namespace {}\n"
                                     "{{\n",
                                     module.filename().string(), module.filename().string(), name);
            for (const HostStruct& host : structs)
            {
                out +=
                    fmt::format("    /// @brief The host form of the Slang struct `{}`: its fields "
                                "in order, under their Slang names.\n"
                                "    struct {}\n"
                                "    {{\n",
                                host.Name, host.Name);
                for (const HostField& field : host.Fields)
                {
                    out += fmt::format("        /// @brief The Slang field `{} {}`.\n"
                                       "        {} {}{{}};\n",
                                       SlangSpelling(field.Type, structs), field.Name,
                                       GlmCpp(field.Type, structs), field.Name);
                }
                out += "    };\n\n";
            }
            for (usize i = 0; i < functions.size(); ++i)
            {
                const HostFunction& function = functions[i];
                out += fmt::format("{}    /// @brief The host build of `{}`.\n"
                                   "    {}{};\n",
                                   i == 0 ? "" : "\n", SlangSignature(function, structs),
                                   function.Return ? "[[nodiscard]] " : "",
                                   HeaderSignature(function, structs));
            }
            out += "}\n";
            return out;
        }

        // The member-wise conversions between the header's types and the prelude's. The prelude
        // type of a struct, an array or a matrix is never named: it is deduced from the export's
        // own signature, and a struct's fields are reached positionally through a structured
        // binding, since Slang mangles both the struct's and its fields' names. A matrix crosses
        // transposed, so the host's glm column c, row r is Slang's m[r][c], as a matrix uploaded
        // from glm reads on the GPU under the cook's column-major layout.
        string BuildConversions(const string& bridgeNamespace, const string& name,
                                const vector<HostStruct>& structs)
        {
            string out = fmt::format(
                "namespace {}\n"
                "{{\n"
                "    template <class F> struct Signature;\n"
                "    template <class R, class... A> struct Signature<R (*)(A...)>\n"
                "    {{\n"
                "        template <std::size_t I> using Param = std::tuple_element_t<I, "
                "std::tuple<A...>>;\n"
                "    }};\n"
                "    template <class F, std::size_t I>\n"
                "    using Param = typename Signature<F>::template Param<I>;\n"
                "    template <class P> using ParamValue = "
                "std::remove_cvref_t<std::remove_pointer_t<P>>;\n"
                "    template <class P, class V> P Pass(V& value)\n"
                "    {{\n"
                "        if constexpr (std::is_pointer_v<P>) {{ return &value; }}\n"
                "        else {{ return value; }}\n"
                "    }}\n"
                "\n"
                "    template <class T> requires std::is_arithmetic_v<T>\n"
                "    void ToPrelude(const T& from, T& to) {{ to = from; }}\n"
                "    template <class T> requires std::is_arithmetic_v<T>\n"
                "    void FromPrelude(const T& from, T& to) {{ to = from; }}\n"
                "    template <glm::length_t N, class T, glm::qualifier Q, class P>\n"
                "    void ToPrelude(const glm::vec<N, T, Q>& from, P& to)\n"
                "    {{\n"
                "        for (glm::length_t i = 0; i < N; ++i) {{ to[i] = from[i]; }}\n"
                "    }}\n"
                "    template <glm::length_t N, class T, glm::qualifier Q, class P>\n"
                "    void FromPrelude(const P& from, glm::vec<N, T, Q>& to)\n"
                "    {{\n"
                "        for (glm::length_t i = 0; i < N; ++i) {{ to[i] = from[i]; }}\n"
                "    }}\n"
                "    template <glm::length_t C, glm::length_t R, class T, glm::qualifier Q, class "
                "P>\n"
                "    void ToPrelude(const glm::mat<C, R, T, Q>& from, P& to)\n"
                "    {{\n"
                "        for (glm::length_t r = 0; r < R; ++r)\n"
                "        {{\n"
                "            for (glm::length_t c = 0; c < C; ++c) {{ to[r][c] = from[c][r]; }}\n"
                "        }}\n"
                "    }}\n"
                "    template <glm::length_t C, glm::length_t R, class T, glm::qualifier Q, class "
                "P>\n"
                "    void FromPrelude(const P& from, glm::mat<C, R, T, Q>& to)\n"
                "    {{\n"
                "        for (glm::length_t r = 0; r < R; ++r)\n"
                "        {{\n"
                "            for (glm::length_t c = 0; c < C; ++c) {{ to[c][r] = from[r][c]; }}\n"
                "        }}\n"
                "    }}\n",
                bridgeNamespace);
            for (const HostStruct& host : structs)
            {
                out += fmt::format("    template <class P> void ToPrelude(const ::{}::{}& from, P& "
                                   "to);\n"
                                   "    template <class P> void FromPrelude(const P& from, "
                                   "::{}::{}& to);\n",
                                   name, host.Name, name, host.Name);
            }
            out += "    template <class T, std::size_t N, class P>\n"
                   "    void ToPrelude(const std::array<T, N>& from, P& to)\n"
                   "    {\n"
                   "        for (std::size_t i = 0; i < N; ++i) { ToPrelude(from[i], to[i]); }\n"
                   "    }\n"
                   "    template <class T, std::size_t N, class P>\n"
                   "    void FromPrelude(const P& from, std::array<T, N>& to)\n"
                   "    {\n"
                   "        for (std::size_t i = 0; i < N; ++i) { FromPrelude(from[i], to[i]); }\n"
                   "    }\n";
            for (const HostStruct& host : structs)
            {
                string bindings;
                string toBody;
                string fromBody;
                for (usize f = 0; f < host.Fields.size(); ++f)
                {
                    bindings += fmt::format("{}f{}", f == 0 ? "" : ", ", f);
                    toBody +=
                        fmt::format("        ToPrelude(from.{}, f{});\n", host.Fields[f].Name, f);
                    fromBody +=
                        fmt::format("        FromPrelude(f{}, to.{});\n", f, host.Fields[f].Name);
                }
                out += fmt::format("    template <class P> void ToPrelude(const ::{0}::{1}& from, "
                                   "P& to)\n"
                                   "    {{\n"
                                   "        auto& [{2}] = to;\n"
                                   "{3}"
                                   "    }}\n"
                                   "    template <class P> void FromPrelude(const P& from, "
                                   "::{0}::{1}& to)\n"
                                   "    {{\n"
                                   "        const auto& [{2}] = from;\n"
                                   "{4}"
                                   "    }}\n",
                                   name, host.Name, bindings, toBody, fromBody);
            }
            out += "}\n\n";
            return out;
        }

        string BuildBridge(const string& name, const string& preludeNamespace,
                           const string& moduleNamespace, const string& bridgeNamespace,
                           const vector<HostStruct>& structs, const vector<HostFunction>& functions)
        {
            string out = BuildConversions(bridgeNamespace, name, structs);
            out += fmt::format("namespace {}\n{{\n", name);
            for (const HostFunction& function : functions)
            {
                // The module's code sees the prelude's types through a using-directive inside its
                // own namespace; the wrapper opens them for its body alone.
                string body = fmt::format("        using namespace {};\n", preludeNamespace);
                const bool deduces =
                    std::ranges::any_of(function.Parameters, [](const HostParameter& parameter)
                                        { return !IsSimple(parameter.Type); });
                if (deduces)
                {
                    body += fmt::format("        using HostCall = decltype(&{}::{});\n",
                                        moduleNamespace, function.Name);
                }
                string args;
                string writeBack;
                for (usize index = 0; index < function.Parameters.size(); ++index)
                {
                    const HostParameter& parameter = function.Parameters[index];
                    if (!args.empty())
                    {
                        args += ", ";
                    }
                    const string local = "hostArg_" + parameter.Name;
                    if (!IsSimple(parameter.Type))
                    {
                        const string param =
                            fmt::format("{}::Param<HostCall, {}>", bridgeNamespace, index);
                        body += fmt::format("        {}::ParamValue<{}> {}{{}};\n", bridgeNamespace,
                                            param, local);
                        if (parameter.Direction != HostDirection::Out)
                        {
                            body += fmt::format("        {}::ToPrelude({}, {});\n", bridgeNamespace,
                                                parameter.Name, local);
                        }
                        args += fmt::format("{}::Pass<{}>({})", bridgeNamespace, param, local);
                        if (parameter.Direction != HostDirection::In)
                        {
                            writeBack += fmt::format("        {}::FromPrelude({}, {});\n",
                                                     bridgeNamespace, local, parameter.Name);
                        }
                        continue;
                    }
                    if (parameter.Direction == HostDirection::In)
                    {
                        args += ToPrelude(parameter.Type, parameter.Name);
                        continue;
                    }
                    if (parameter.Type.Kind == HostKind::Scalar)
                    {
                        // The prelude's scalar is the glm scalar, so it is written in place.
                        args += "&" + parameter.Name;
                        continue;
                    }
                    if (parameter.Direction == HostDirection::InOut)
                    {
                        body += fmt::format("        {} {} = {};\n", PreludeCpp(parameter.Type),
                                            local, ToPrelude(parameter.Type, parameter.Name));
                    }
                    else
                    {
                        body +=
                            fmt::format("        {} {}{{}};\n", PreludeCpp(parameter.Type), local);
                    }
                    args += "&" + local;
                    writeBack += fmt::format("        {} = {};\n", parameter.Name,
                                             ToGlm(parameter.Type, local, structs));
                }
                const string call = fmt::format("{}::{}({})", moduleNamespace, function.Name, args);
                if (function.Return && IsSimple(*function.Return))
                {
                    body += fmt::format("        const {} hostResult = {};\n",
                                        PreludeCpp(*function.Return), call);
                    body += writeBack;
                    body += fmt::format("        return {};\n",
                                        ToGlm(*function.Return, "hostResult", structs));
                }
                else if (function.Return)
                {
                    body += fmt::format("        const auto hostResult = {};\n", call);
                    body += writeBack;
                    body += fmt::format("        {} result{{}};\n"
                                        "        {}::FromPrelude(hostResult, result);\n"
                                        "        return result;\n",
                                        GlmCpp(*function.Return, structs), bridgeNamespace);
                }
                else
                {
                    body += fmt::format("        {};\n", call);
                    body += writeBack;
                }
                out += fmt::format("    {}\n    {{\n{}    }}\n\n",
                                   HeaderSignature(function, structs), body);
            }
            out += "}\n";
            return out;
        }

        // Whether the emitted C++ defines `name` under its own name, which `__extern_cpp` keeps.
        bool DefinesUnmangled(const string& code, const string& name)
        {
            const string needle = name + "(";
            for (usize at = code.find(needle); at != string::npos; at = code.find(needle, at + 1))
            {
                if (at > 0 && (code[at - 1] == ' ' || code[at - 1] == '*' || code[at - 1] == '&'))
                {
                    return true;
                }
            }
            return false;
        }
    }

    struct HostShaderCompiler::Impl
    {
        ComPtr<slang::IGlobalSession> Global;
        string DefaultPrelude;
    };

    HostShaderCompiler::HostShaderCompiler(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl)) {}
    HostShaderCompiler::~HostShaderCompiler() = default;
    HostShaderCompiler::HostShaderCompiler(HostShaderCompiler&&) noexcept = default;
    HostShaderCompiler& HostShaderCompiler::operator=(HostShaderCompiler&&) noexcept = default;

    Result<HostShaderCompiler> HostShaderCompiler::Create()
    {
        auto impl = std::make_unique<Impl>();
        if (SLANG_FAILED(slang::createGlobalSession(impl->Global.writeRef())))
        {
            return std::unexpected("compile-host: failed to create Slang global session");
        }
        ComPtr<ISlangBlob> prelude;
        impl->Global->getLanguagePrelude(SLANG_SOURCE_LANGUAGE_CPP, prelude.writeRef());
        if (prelude)
        {
            impl->DefaultPrelude.assign(static_cast<const char*>(prelude->getBufferPointer()),
                                        prelude->getBufferSize());
        }
        return HostShaderCompiler(std::move(impl));
    }

    string HostShaderCompiler::GetSlangVersion() const
    {
        return m_Impl->Global->getBuildTagString();
    }

    Result<HostShaderModuleOutput> HostShaderCompiler::Compile(const HostShaderModuleInfo& info)
    {
        const string where = fmt::format("compile-host: '{}'", info.Module.string());
        if (!IsIdentifier(info.Name))
        {
            return std::unexpected(
                fmt::format("{}: --name '{}' is not a C++ identifier", where, info.Name));
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(info.Module, ec))
        {
            return std::unexpected(fmt::format("{}: no such file", where));
        }

        // The prelude's types go in one namespace and the module's code in another, both private
        // to this module, so two host modules in one binary define nothing in common. The export
        // macro is emptied so an exported function keeps C++ linkage inside that namespace.
        const string preludeNamespace = "VengHostPrelude_" + info.Name;
        const string moduleNamespace = "VengHostModule_" + info.Name;
        const string prelude =
            fmt::format("#define SLANG_PRELUDE_NAMESPACE {}\n{}\n"
                        "#undef SLANG_PRELUDE_EXPORT\n"
                        "#define SLANG_PRELUDE_EXPORT\n"
                        "namespace {}\n{{\n",
                        preludeNamespace, m_Impl->DefaultPrelude, moduleNamespace);
        m_Impl->Global->setLanguagePrelude(SLANG_SOURCE_LANGUAGE_CPP, prelude.c_str());

        slang::TargetDesc targetDesc{};
        targetDesc.format = SLANG_CPP_SOURCE;
        slang::SessionDesc sessionDesc{};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        const std::vector<std::string> searchPaths =
            BuildSlangSearchPaths(info.Module.parent_path(), info.ShaderIncludeDir);
        std::vector<const char*> searchPathPtrs;
        ApplySlangSearchPaths(sessionDesc, searchPaths, searchPathPtrs);
        // The cook's matrix layout, so a shared file reads a matrix the same way on both targets.
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

        ComPtr<slang::ISession> session;
        if (SLANG_FAILED(m_Impl->Global->createSession(sessionDesc, session.writeRef())))
        {
            return std::unexpected(fmt::format("{}: failed to create Slang session", where));
        }

        ComPtr<slang::IBlob> diagnostics;
        slang::IModule* module = LoadSlangModule(*session, SlangModuleSource{.Path = info.Module},
                                                 diagnostics.writeRef());
        if (!module)
        {
            return std::unexpected(
                fmt::format("{}: failed to compile: {}", where, DiagnosticsText(diagnostics)));
        }

        HostShaderModuleOutput output;
        for (SlangInt32 i = 0; i < module->getDependencyFileCount(); ++i)
        {
            const path dependency(module->getDependencyFilePath(i));
            if (std::filesystem::exists(dependency, ec))
            {
                output.Dependencies.push_back(dependency);
            }
        }

        if (module->getDefinedEntryPointCount() > 0)
        {
            return std::unexpected(fmt::format(
                "{}: declares an entry point; a host module is a library of functions", where));
        }
        slang::ProgramLayout* layout = module->getLayout();
        if (layout && layout->getParameterCount() > 0)
        {
            slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(0);
            return std::unexpected(fmt::format(
                "{}: declares the global shader parameter '{}'; a host module must be pure, "
                "taking everything it reads as function arguments",
                where, parameter && parameter->getName() ? parameter->getName() : "?"));
        }

        vector<HostStruct> structs;
        vector<HostFunction> functions;
        constexpr const char* Crossable =
            "float, double, int or uint scalars and 2- to 4-vectors, float and double matrices, "
            "fixed-size arrays of these, and structs of these";
        slang::DeclReflection* moduleDecl = module->getModuleReflection();
        for (u32 i = 0; moduleDecl && i < moduleDecl->getChildrenCount(); ++i)
        {
            slang::DeclReflection* child = moduleDecl->getChild(i);
            if (child->getKind() != slang::DeclReflection::Kind::Func)
            {
                continue;
            }
            slang::FunctionReflection* reflected = child->asFunction();
            if (!reflected || !reflected->findModifier(slang::Modifier::Export))
            {
                continue;
            }
            HostFunction function{.Name = reflected->getName()};
            slang::TypeReflection* returnType = reflected->getReturnType();
            if (!IsVoid(returnType))
            {
                Result<HostType> mapped = MapType(returnType, structs);
                if (mapped && mapped->Kind == HostKind::Array)
                {
                    // Slang lowers an array return to a trailing out pointer.
                    return std::unexpected(fmt::format(
                        "{}: function '{}' returns the array '{}'; return an array through an out "
                        "parameter instead",
                        where, function.Name, TypeDisplayName(returnType)));
                }
                if (!mapped)
                {
                    const string detail =
                        mapped.error().empty() ? "" : fmt::format(" ({})", mapped.error());
                    return std::unexpected(fmt::format(
                        "{}: function '{}' returns '{}', which cannot cross to the host{}; return "
                        "one of {}, or an array through an out parameter",
                        where, function.Name, TypeDisplayName(returnType), detail, Crossable));
                }
                function.Return = std::move(*mapped);
            }
            for (u32 p = 0; p < reflected->getParameterCount(); ++p)
            {
                slang::VariableReflection* variable = reflected->getParameterByIndex(p);
                HostParameter parameter{.Name = variable->getName() ? variable->getName() : ""};
                Result<HostType> type = MapType(variable->getType(), structs);
                if (!type)
                {
                    const string detail =
                        type.error().empty() ? "" : fmt::format(" ({})", type.error());
                    return std::unexpected(fmt::format(
                        "{}: function '{}': parameter '{}' has type '{}', which cannot cross to "
                        "the host{}; use {}",
                        where, function.Name, parameter.Name, TypeDisplayName(variable->getType()),
                        detail, Crossable));
                }
                parameter.Type = std::move(*type);
                if (variable->findModifier(slang::Modifier::InOut))
                {
                    parameter.Direction = HostDirection::InOut;
                }
                else if (variable->findModifier(slang::Modifier::Out))
                {
                    parameter.Direction = HostDirection::Out;
                }
                function.Parameters.push_back(std::move(parameter));
            }
            functions.push_back(std::move(function));
        }
        // The bridge deduces a struct, array or matrix parameter's prelude type from the export's
        // address, which an overloaded name does not have.
        for (const HostFunction& function : functions)
        {
            const bool deduces =
                std::ranges::any_of(function.Parameters, [](const HostParameter& parameter)
                                    { return !IsSimple(parameter.Type); });
            const auto overloads = std::ranges::count_if(functions, [&](const HostFunction& other)
                                                         { return other.Name == function.Name; });
            if (deduces && overloads > 1)
            {
                return std::unexpected(fmt::format(
                    "{}: function '{}' is overloaded and takes a struct, array or matrix; give "
                    "each overload its own name",
                    where, function.Name));
            }
        }
        if (functions.empty())
        {
            return std::unexpected(
                fmt::format("{}: exports no function; declare each host entry as "
                            "`export __extern_cpp`",
                            where));
        }

        ComPtr<slang::IComponentType> linked;
        diagnostics = nullptr;
        if (SLANG_FAILED(module->link(linked.writeRef(), diagnostics.writeRef())))
        {
            return std::unexpected(
                fmt::format("{}: failed to link: {}", where, DiagnosticsText(diagnostics)));
        }
        ComPtr<slang::IBlob> code;
        diagnostics = nullptr;
        if (SLANG_FAILED(linked->getTargetCode(0, code.writeRef(), diagnostics.writeRef())))
        {
            return std::unexpected(
                fmt::format("{}: C++ codegen failed: {}", where, DiagnosticsText(diagnostics)));
        }
        const string emitted(static_cast<const char*>(code->getBufferPointer()),
                             code->getBufferSize());
        for (const HostFunction& function : functions)
        {
            if (!DefinesUnmangled(emitted, function.Name))
            {
                return std::unexpected(fmt::format(
                    "{}: function '{}' is exported without __extern_cpp, so its emitted name is "
                    "mangled; declare it `export __extern_cpp`",
                    where, function.Name));
            }
        }

        // The header comes first so glm is parsed before the prelude's using-directive opens its
        // names into the global namespace.
        output.Source = fmt::format("// Generated by vengc compile-host from {}. Do not edit.\n"
                                    "#include \"{}.h\"\n\n"
                                    "#include <cstddef>\n"
                                    "#include <tuple>\n"
                                    "#include <type_traits>\n\n",
                                    info.Module.filename().string(), info.Name);
        output.Source += emitted;
        output.Source += fmt::format("\n}} // namespace {}\n\n", moduleNamespace);
        const usize line = static_cast<usize>(std::ranges::count(output.Source, '\n')) + 2;
        output.Source += fmt::format("#line {} \"{}.cpp\"\n", line, info.Name);
        output.Source += BuildBridge(info.Name, preludeNamespace, moduleNamespace,
                                     "VengHostBridge_" + info.Name, structs, functions);
        output.Header = BuildHeader(info.Name, info.Module, structs, functions);
        return output;
    }

    VoidResult WriteHostShaderModule(const HostShaderModuleOutput& output, const string& name,
                                     const path& outDir, const std::optional<path>& depfile)
    {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec)
        {
            return std::unexpected(
                fmt::format("compile-host: '{}': {}", outDir.string(), ec.message()));
        }
        const auto write = [](const path& file, const string& text)
        {
            return WriteFileAtomic(
                file, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
        };
        const path source = outDir / (name + ".cpp");
        if (const VoidResult written = write(source, output.Source); !written)
        {
            return written;
        }
        if (const VoidResult written = write(outDir / (name + ".h"), output.Header); !written)
        {
            return written;
        }
        if (depfile)
        {
            return WriteDepfile(*depfile, source, output.Dependencies);
        }
        return {};
    }
}
