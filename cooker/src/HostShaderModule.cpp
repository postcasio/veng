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

        /// @brief A type that may cross the host boundary: a scalar or a 2- to 4-vector of one.
        struct HostType
        {
            /// @brief The scalar, or the vector's element.
            HostScalar Scalar = HostScalar::F32;
            /// @brief 1 for a scalar, else the vector's component count.
            u32 Count = 1;
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

        optional<HostType> MapType(slang::TypeReflection* type)
        {
            if (!type)
            {
                return std::nullopt;
            }
            if (type->getKind() == slang::TypeReflection::Kind::Scalar)
            {
                const optional<HostScalar> scalar = MapScalar(type->getScalarType());
                if (!scalar)
                {
                    return std::nullopt;
                }
                return HostType{.Scalar = *scalar, .Count = 1};
            }
            if (type->getKind() == slang::TypeReflection::Kind::Vector)
            {
                const usize count = type->getElementCount();
                if (count < 2 || count > 4)
                {
                    return std::nullopt;
                }
                const optional<HostScalar> scalar =
                    MapScalar(type->getElementType()->getScalarType());
                if (!scalar)
                {
                    return std::nullopt;
                }
                return HostType{.Scalar = *scalar, .Count = static_cast<u32>(count)};
            }
            return std::nullopt;
        }

        bool IsVoid(slang::TypeReflection* type)
        {
            return type && type->getKind() == slang::TypeReflection::Kind::Scalar &&
                   type->getScalarType() == slang::TypeReflection::ScalarType::Void;
        }

        string TypeDisplayName(slang::TypeReflection* type)
        {
            if (!type || !type->getName())
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

        // The spelling the Slang C++ prelude uses for the type.
        string PreludeCpp(const HostType& type)
        {
            const char* scalar = type.Scalar == HostScalar::I32   ? "int32_t"
                                 : type.Scalar == HostScalar::U32 ? "uint32_t"
                                                                  : ScalarCpp(type.Scalar);
            if (type.Count == 1)
            {
                return scalar;
            }
            return fmt::format("Vector<{}, {}>", scalar, type.Count);
        }

        string GlmCpp(const HostType& type)
        {
            if (type.Count == 1)
            {
                return ScalarCpp(type.Scalar);
            }
            const char* prefix = type.Scalar == HostScalar::F32   ? ""
                                 : type.Scalar == HostScalar::F64 ? "d"
                                 : type.Scalar == HostScalar::I32 ? "i"
                                                                  : "u";
            return fmt::format("glm::{}vec{}", prefix, type.Count);
        }

        string SlangSpelling(const HostType& type)
        {
            if (type.Count == 1)
            {
                return ScalarSlang(type.Scalar);
            }
            return fmt::format("{}{}", ScalarSlang(type.Scalar), type.Count);
        }

        constexpr std::array<const char*, 4> Components{"x", "y", "z", "w"};

        // `value` (a glm vector or scalar expression) converted to the prelude's type.
        string ToPrelude(const HostType& type, const string& value)
        {
            if (type.Count == 1)
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
        string ToGlm(const HostType& type, const string& value)
        {
            if (type.Count == 1)
            {
                return value;
            }
            string args;
            for (u32 i = 0; i < type.Count; ++i)
            {
                args += fmt::format("{}{}.{}", i == 0 ? "" : ", ", value, Components[i]);
            }
            return fmt::format("{}({})", GlmCpp(type), args);
        }

        string HeaderSignature(const HostFunction& function)
        {
            string params;
            for (const HostParameter& parameter : function.Parameters)
            {
                if (!params.empty())
                {
                    params += ", ";
                }
                params += GlmCpp(parameter.Type);
                params += parameter.Direction == HostDirection::In ? " " : "& ";
                params += parameter.Name;
            }
            return fmt::format("{} {}({})", function.Return ? GlmCpp(*function.Return) : "void",
                               function.Name, params);
        }

        string SlangSignature(const HostFunction& function)
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
                params += SlangSpelling(parameter.Type);
                params += ' ';
                params += parameter.Name;
            }
            return fmt::format("{} {}({})",
                               function.Return ? SlangSpelling(*function.Return) : "void",
                               function.Name, params);
        }

        string BuildHeader(const string& name, const path& module,
                           const vector<HostFunction>& functions)
        {
            string out = fmt::format("#pragma once\n"
                                     "\n"
                                     "// Generated by vengc compile-host from {}. Do not edit.\n"
                                     "\n"
                                     "#include <cstdint>\n"
                                     "\n"
                                     "#include <glm/glm.hpp>\n"
                                     "\n"
                                     "/// @brief The host build of the Slang module {}.\n"
                                     "namespace {}\n"
                                     "{{\n",
                                     module.filename().string(), module.filename().string(), name);
            for (usize i = 0; i < functions.size(); ++i)
            {
                const HostFunction& function = functions[i];
                out +=
                    fmt::format("{}    /// @brief The host build of `{}`.\n"
                                "    {}{};\n",
                                i == 0 ? "" : "\n", SlangSignature(function),
                                function.Return ? "[[nodiscard]] " : "", HeaderSignature(function));
            }
            out += "}\n";
            return out;
        }

        string BuildBridge(const string& name, const string& preludeNamespace,
                           const string& moduleNamespace, const vector<HostFunction>& functions)
        {
            string out = fmt::format("namespace {}\n{{\n", name);
            for (const HostFunction& function : functions)
            {
                // The module's code sees the prelude's types through a using-directive inside its
                // own namespace; the wrapper opens them for its body alone.
                string body = fmt::format("        using namespace {};\n", preludeNamespace);
                string args;
                string writeBack;
                for (const HostParameter& parameter : function.Parameters)
                {
                    if (!args.empty())
                    {
                        args += ", ";
                    }
                    if (parameter.Direction == HostDirection::In)
                    {
                        args += ToPrelude(parameter.Type, parameter.Name);
                        continue;
                    }
                    if (parameter.Type.Count == 1)
                    {
                        // The prelude's scalar is the glm scalar, so it is written in place.
                        args += "&" + parameter.Name;
                        continue;
                    }
                    const string local = "hostArg_" + parameter.Name;
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
                                             ToGlm(parameter.Type, local));
                }
                const string call = fmt::format("{}::{}({})", moduleNamespace, function.Name, args);
                if (function.Return)
                {
                    body += fmt::format("        const {} hostResult = {};\n",
                                        PreludeCpp(*function.Return), call);
                    body += writeBack;
                    body +=
                        fmt::format("        return {};\n", ToGlm(*function.Return, "hostResult"));
                }
                else
                {
                    body += fmt::format("        {};\n", call);
                    body += writeBack;
                }
                out += fmt::format("    {}\n    {{\n{}    }}\n\n", HeaderSignature(function), body);
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

        vector<HostFunction> functions;
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
                function.Return = MapType(returnType);
                if (!function.Return)
                {
                    return std::unexpected(fmt::format(
                        "{}: function '{}' returns '{}', which cannot cross to the host; return "
                        "a float, double, int or uint scalar or vector, and anything more "
                        "through out parameters",
                        where, function.Name, TypeDisplayName(returnType)));
                }
            }
            for (u32 p = 0; p < reflected->getParameterCount(); ++p)
            {
                slang::VariableReflection* variable = reflected->getParameterByIndex(p);
                HostParameter parameter{.Name = variable->getName() ? variable->getName() : ""};
                const optional<HostType> type = MapType(variable->getType());
                if (!type)
                {
                    return std::unexpected(fmt::format(
                        "{}: function '{}': parameter '{}' has type '{}', which cannot cross to "
                        "the host; use a float, double, int or uint scalar or vector",
                        where, function.Name, parameter.Name,
                        TypeDisplayName(variable->getType())));
                }
                parameter.Type = *type;
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
                                    "#include \"{}.h\"\n\n",
                                    info.Module.filename().string(), info.Name);
        output.Source += emitted;
        output.Source += fmt::format("\n}} // namespace {}\n\n", moduleNamespace);
        const usize line = static_cast<usize>(std::ranges::count(output.Source, '\n')) + 2;
        output.Source += fmt::format("#line {} \"{}.cpp\"\n", line, info.Name);
        output.Source += BuildBridge(info.Name, preludeNamespace, moduleNamespace, functions);
        output.Header = BuildHeader(info.Name, info.Module, functions);
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
