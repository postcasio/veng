#include "SlangReflect.h"

#include <fmt/format.h>

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

namespace Veng::Cook
{
    namespace
    {
        using Slang::ComPtr;

        string DiagnosticsText(slang::IBlob* diagnostics)
        {
            if (!diagnostics || diagnostics->getBufferSize() == 0)
                return "";

            return string(static_cast<const char*>(diagnostics->getBufferPointer()), diagnostics->getBufferSize());
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
                componentCount = static_cast<u32>(type->getColumnCount());
            else if (kind != slang::TypeReflection::Kind::Scalar)
            {
                return std::unexpected(fmt::format(
                    "material importer: struct '{}': field '{}' is not a scalar or vector",
                    structName, field->getName()));
            }

            ReflectedStructField result;
            result.Name = field->getName();
            result.Offset = static_cast<u32>(field->getOffset(slang::ParameterCategory::Uniform));
            result.Size = static_cast<u32>(field->getTypeLayout()->getSize(slang::ParameterCategory::Uniform));
            result.ComponentCount = componentCount;
            result.IsFloat = isFloat;
            return result;
        }
    }

    Result<ReflectedStruct> ReflectStructLayout(
        const path& slangSource, std::string_view structName)
    {
        ComPtr<slang::IGlobalSession> globalSession;
        if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
            return std::unexpected("material importer: failed to create Slang global session");

        slang::TargetDesc targetDesc{};
        targetDesc.format = SLANG_SPIRV;
        targetDesc.profile = globalSession->findProfile("spirv_1_5");

        const string searchPath = slangSource.parent_path().string();
        const char* searchPaths[] = {searchPath.c_str()};

        slang::SessionDesc sessionDesc{};
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        sessionDesc.searchPaths = searchPaths;
        sessionDesc.searchPathCount = 1;
        // Match the compile session so reflected matrix-member offsets agree with
        // the column-major layout the SPIR-V is generated for.
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

        ComPtr<slang::ISession> session;
        if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
            return std::unexpected("material importer: failed to create Slang session");

        const string moduleName = slangSource.stem().string();

        ComPtr<slang::IBlob> diagnostics;
        slang::IModule* module = session->loadModule(moduleName.c_str(), diagnostics.writeRef());
        if (!module)
        {
            return std::unexpected(fmt::format("material importer: '{}': failed to compile: {}",
                slangSource.string(), DiagnosticsText(diagnostics)));
        }

        slang::IComponentType* components[] = {module};
        ComPtr<slang::IComponentType> program;
        diagnostics = nullptr;
        if (SLANG_FAILED(session->createCompositeComponentType(components, 1, program.writeRef(), diagnostics.writeRef())))
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
            return std::unexpected(fmt::format("material importer: '{}': no reflection layout",
                slangSource.string()));
        }

        const string structNameStr(structName);
        slang::TypeReflection* type = layout->findTypeByName(structNameStr.c_str());
        if (!type)
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': struct '{}' not found (is it declared and used in the shader?)",
                slangSource.string(), structName));
        }

        slang::TypeLayoutReflection* typeLayout = layout->getTypeLayout(type);
        if (!typeLayout || typeLayout->getKind() != slang::TypeReflection::Kind::Struct)
        {
            return std::unexpected(fmt::format(
                "material importer: '{}': '{}' is not a struct", slangSource.string(), structName));
        }

        ReflectedStruct result;
        result.Size = static_cast<u32>(typeLayout->getSize(slang::ParameterCategory::Uniform));
        result.Fields.reserve(typeLayout->getFieldCount());
        for (unsigned i = 0; i < typeLayout->getFieldCount(); ++i)
        {
            const Result<ReflectedStructField> field = ReflectField(typeLayout->getFieldByIndex(i), structName);
            if (!field)
                return std::unexpected(field.error());
            result.Fields.push_back(*field);
        }

        return result;
    }
}
