#include "SlangReflect.h"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include "SlangSession.h"

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

            return string(static_cast<const char*>(diagnostics->getBufferPointer()),
                          diagnostics->getBufferSize());
        }

        // Maps a reflected field's scalar/vector type to (componentCount, isFloat).
        // Only float/floatN and uint/uintN are supported — a material's params are
        // floats and its texture/sampler handles are uints.
        Result<ReflectedStructField> ReflectField(slang::VariableLayoutReflection* field,
                                                  std::string_view structName)
        {
            slang::TypeReflection* type = field->getType();
            const slang::TypeReflection::ScalarType scalar = type->getScalarType();

            const bool isFloat = scalar == slang::TypeReflection::ScalarType::Float32;
            const bool isUint = scalar == slang::TypeReflection::ScalarType::UInt32;
            if (!isFloat && !isUint)
            {
                return std::unexpected(fmt::format(
                    "material importer: struct '{}': field '{}' has an unsupported scalar type "
                    "(only float/floatN and uint/uintN are supported)",
                    structName, field->getName()));
            }

            u32 componentCount = 1;
            const slang::TypeReflection::Kind kind = type->getKind();
            if (kind == slang::TypeReflection::Kind::Vector)
            {
                componentCount = static_cast<u32>(type->getColumnCount());
            }
            else if (kind != slang::TypeReflection::Kind::Scalar)
            {
                return std::unexpected(fmt::format(
                    "material importer: struct '{}': field '{}' is not a scalar or vector",
                    structName, field->getName()));
            }

            ReflectedStructField result;
            result.Name = field->getName();
            result.Offset = static_cast<u32>(field->getOffset(slang::ParameterCategory::Uniform));
            result.Size = static_cast<u32>(
                field->getTypeLayout()->getSize(slang::ParameterCategory::Uniform));
            result.ComponentCount = componentCount;
            result.IsFloat = isFloat;
            return result;
        }

        // Reflects one varying-output variable's scalar/vector type into a
        // ReflectedFragmentOutput. The SV_Target index comes from the semantic the
        // caller already read off the variable layout.
        Result<ReflectedFragmentOutput> ReflectOutputType(slang::TypeReflection* type,
                                                          u32 targetIndex, std::string_view entry)
        {
            const slang::TypeReflection::ScalarType scalar = type->getScalarType();
            const bool isFloat = scalar == slang::TypeReflection::ScalarType::Float32;
            const bool isUint = scalar == slang::TypeReflection::ScalarType::UInt32;
            if (!isFloat && !isUint)
            {
                return std::unexpected(
                    fmt::format("material importer: entry point '{}': SV_Target{} has an "
                                "unsupported scalar type "
                                "(only float/floatN and uint/uintN are supported)",
                                entry, targetIndex));
            }

            u32 componentCount = 1;
            const slang::TypeReflection::Kind kind = type->getKind();
            if (kind == slang::TypeReflection::Kind::Vector)
            {
                componentCount = static_cast<u32>(type->getColumnCount());
            }
            else if (kind != slang::TypeReflection::Kind::Scalar)
            {
                return std::unexpected(fmt::format(
                    "material importer: entry point '{}': SV_Target{} is not a scalar or vector",
                    entry, targetIndex));
            }

            ReflectedFragmentOutput output;
            output.TargetIndex = targetIndex;
            output.ComponentCount = componentCount;
            output.IsFloat = isFloat;
            return output;
        }

        // Slang normalizes any SV_TargetN semantic to "SV_TARGET" (uppercase, no
        // trailing digit); the index comes from getSemanticIndex(), not the name.
        bool IsTargetSemantic(const char* semantic)
        {
            if (!semantic)
            {
                return false;
            }
            static constexpr std::string_view Prefix = "SV_TARGET";
            const std::string_view s(semantic);
            if (s.size() < Prefix.size())
            {
                return false;
            }
            for (usize i = 0; i < Prefix.size(); ++i)
            {
                if (std::toupper(static_cast<unsigned char>(s[i])) != Prefix[i])
                {
                    return false;
                }
            }
            return true;
        }

        // Walks the entry result layout — either a bare SV_TargetN or a struct of
        // varyings (MRT) — and collects each render target. Missing SV_Target is a
        // located error.
        Result<vector<ReflectedFragmentOutput>>
        CollectOutputs(slang::VariableLayoutReflection* result, std::string_view entry)
        {
            vector<ReflectedFragmentOutput> outputs;
            if (!result)
            {
                return outputs;
            }

            slang::TypeLayoutReflection* typeLayout = result->getTypeLayout();
            if (typeLayout && typeLayout->getKind() == slang::TypeReflection::Kind::Struct)
            {
                for (unsigned i = 0; i < typeLayout->getFieldCount(); ++i)
                {
                    slang::VariableLayoutReflection* field = typeLayout->getFieldByIndex(i);
                    if (!IsTargetSemantic(field->getSemanticName()))
                    {
                        return std::unexpected(
                            fmt::format("material importer: entry point '{}': output '{}' carries "
                                        "no SV_Target semantic",
                                        entry, field->getName() ? field->getName() : "?"));
                    }

                    // The varying's declared type comes through the type layout —
                    // a struct field's own getType() is empty for an entry result.
                    const Result<ReflectedFragmentOutput> output =
                        ReflectOutputType(field->getTypeLayout()->getType(),
                                          static_cast<u32>(field->getSemanticIndex()), entry);
                    if (!output)
                    {
                        return std::unexpected(output.error());
                    }
                    outputs.push_back(*output);
                }
            }
            else
            {
                if (!IsTargetSemantic(result->getSemanticName()))
                {
                    return std::unexpected(
                        fmt::format("material importer: entry point '{}': fragment output carries "
                                    "no SV_Target semantic",
                                    entry));
                }

                const Result<ReflectedFragmentOutput> output = ReflectOutputType(
                    typeLayout->getType(), static_cast<u32>(result->getSemanticIndex()), entry);
                if (!output)
                {
                    return std::unexpected(output.error());
                }
                outputs.push_back(*output);
            }

            std::ranges::sort(outputs,
                              [](const ReflectedFragmentOutput& a, const ReflectedFragmentOutput& b)
                              { return a.TargetIndex < b.TargetIndex; });
            return outputs;
        }
    }

    Result<ReflectedStruct> ReflectStructLayout(const path& slangSource,
                                                std::string_view structName,
                                                const path& shaderIncludeDir, bool optional)
    {
        ComPtr<slang::IGlobalSession> globalSession;
        if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
        {
            return std::unexpected("material importer: failed to create Slang global session");
        }

        slang::TargetDesc targetDesc{};
        targetDesc.format = SLANG_SPIRV;
        targetDesc.profile = globalSession->findProfile("spirv_1_5");

        slang::SessionDesc sessionDesc{};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        const std::vector<std::string> searchPaths =
            BuildSlangSearchPaths(slangSource.parent_path(), shaderIncludeDir);
        std::vector<const char*> searchPathPtrs;
        ApplySlangSearchPaths(sessionDesc, searchPaths, searchPathPtrs);
        // Match the compile session so reflected matrix-member offsets agree with
        // the column-major layout the SPIR-V is generated for.
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

        ComPtr<slang::ISession> session;
        if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
        {
            return std::unexpected("material importer: failed to create Slang session");
        }

        const string moduleName = slangSource.stem().string();

        ComPtr<slang::IBlob> diagnostics;
        slang::IModule* module = session->loadModule(moduleName.c_str(), diagnostics.writeRef());
        if (!module)
        {
            return std::unexpected(fmt::format("material importer: '{}': failed to compile: {}",
                                               slangSource.string(), DiagnosticsText(diagnostics)));
        }

        slang::IComponentType* const components[] = {module};
        ComPtr<slang::IComponentType> program;
        diagnostics = nullptr;
        if (SLANG_FAILED(session->createCompositeComponentType(components, 1, program.writeRef(),
                                                               diagnostics.writeRef())))
        {
            return std::unexpected(fmt::format("material importer: '{}': failed to compose: {}",
                                               slangSource.string(), DiagnosticsText(diagnostics)));
        }

        ComPtr<slang::IComponentType> linkedProgram;
        diagnostics = nullptr;
        if (SLANG_FAILED(program->link(linkedProgram.writeRef(), diagnostics.writeRef())))
        {
            return std::unexpected(fmt::format("material importer: '{}': failed to link: {}",
                                               slangSource.string(), DiagnosticsText(diagnostics)));
        }

        slang::ProgramLayout* layout = linkedProgram->getLayout();
        if (!layout)
        {
            return std::unexpected(
                fmt::format("material importer: '{}': no reflection layout", slangSource.string()));
        }

        const string structNameStr(structName);
        slang::TypeReflection* type = layout->findTypeByName(structNameStr.c_str());
        if (!type)
        {
            if (optional)
            {
                return ReflectedStruct{};
            }
            return std::unexpected(fmt::format("material importer: '{}': struct '{}' not found (is "
                                               "it declared and used in the shader?)",
                                               slangSource.string(), structName));
        }

        slang::TypeLayoutReflection* typeLayout = layout->getTypeLayout(type);
        if (!typeLayout || typeLayout->getKind() != slang::TypeReflection::Kind::Struct)
        {
            return std::unexpected(fmt::format("material importer: '{}': '{}' is not a struct",
                                               slangSource.string(), structName));
        }

        ReflectedStruct result;
        result.Size = static_cast<u32>(typeLayout->getSize(slang::ParameterCategory::Uniform));
        result.Fields.reserve(typeLayout->getFieldCount());
        for (unsigned i = 0; i < typeLayout->getFieldCount(); ++i)
        {
            const Result<ReflectedStructField> field =
                ReflectField(typeLayout->getFieldByIndex(i), structName);
            if (!field)
            {
                return std::unexpected(field.error());
            }
            result.Fields.push_back(*field);
        }

        return result;
    }

    Result<vector<ReflectedFragmentOutput>> ReflectFragmentOutputs(const path& slangSource,
                                                                   std::string_view entry,
                                                                   const path& shaderIncludeDir)
    {
        ComPtr<slang::IGlobalSession> globalSession;
        if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
        {
            return std::unexpected("material importer: failed to create Slang global session");
        }

        slang::TargetDesc targetDesc{};
        targetDesc.format = SLANG_SPIRV;
        targetDesc.profile = globalSession->findProfile("spirv_1_5");

        slang::SessionDesc sessionDesc{};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        const std::vector<std::string> searchPaths =
            BuildSlangSearchPaths(slangSource.parent_path(), shaderIncludeDir);
        std::vector<const char*> searchPathPtrs;
        ApplySlangSearchPaths(sessionDesc, searchPaths, searchPathPtrs);
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

        ComPtr<slang::ISession> session;
        if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
        {
            return std::unexpected("material importer: failed to create Slang session");
        }

        const string moduleName = slangSource.stem().string();
        const string entryStr(entry);

        ComPtr<slang::IBlob> diagnostics;
        slang::IModule* module = session->loadModule(moduleName.c_str(), diagnostics.writeRef());
        if (!module)
        {
            return std::unexpected(fmt::format("material importer: '{}': failed to compile: {}",
                                               slangSource.string(), DiagnosticsText(diagnostics)));
        }

        // Composing with the entry point is what exposes its result var layout —
        // the per-entry render targets the domain contract validates.
        ComPtr<slang::IEntryPoint> entryPoint;
        if (SLANG_FAILED(module->findEntryPointByName(entryStr.c_str(), entryPoint.writeRef())))
        {
            return std::unexpected(
                fmt::format("material importer: '{}': entry point '{}' not found",
                            slangSource.string(), entry));
        }

        slang::IComponentType* const components[] = {module, entryPoint};
        ComPtr<slang::IComponentType> program;
        diagnostics = nullptr;
        if (SLANG_FAILED(session->createCompositeComponentType(components, 2, program.writeRef(),
                                                               diagnostics.writeRef())))
        {
            return std::unexpected(fmt::format("material importer: '{}': failed to compose: {}",
                                               slangSource.string(), DiagnosticsText(diagnostics)));
        }

        ComPtr<slang::IComponentType> linkedProgram;
        diagnostics = nullptr;
        if (SLANG_FAILED(program->link(linkedProgram.writeRef(), diagnostics.writeRef())))
        {
            return std::unexpected(fmt::format("material importer: '{}': failed to link: {}",
                                               slangSource.string(), DiagnosticsText(diagnostics)));
        }

        slang::ProgramLayout* layout = linkedProgram->getLayout();
        if (!layout || layout->getEntryPointCount() != 1)
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': entry point '{}': unexpected reflection layout",
                slangSource.string(), entry));
        }

        slang::EntryPointReflection* entryLayout = layout->getEntryPointByIndex(0);
        if (entryLayout->getStage() != SLANG_STAGE_FRAGMENT)
        {
            return std::unexpected(
                fmt::format("material importer: '{}': entry point '{}' is not a fragment stage",
                            slangSource.string(), entry));
        }

        return CollectOutputs(entryLayout->getResultVarLayout(), entry);
    }
}
