#include <Veng/Renderer/ShaderModule.h>

#include <fstream>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    ShaderModule::Native& ShaderModule::GetNative() const
    {
        return *m_Native;
    }

    Result<Ref<ShaderModule>> ShaderModule::Create(Context& context, const ShaderModuleInfo& info)
    {
        const path filePath = std::filesystem::absolute(info.Path);
        std::ifstream file(filePath, std::ios::ate | std::ios::binary);
        if (!file.is_open())
        {
            return std::unexpected(fmt::format("Can't open shader file: {}", filePath.string()));
        }
        const auto fileSize = static_cast<usize>(file.tellg());
        vector<u8> buffer(fileSize);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));

        // A file of Metal source compiles at load; anything else is a compiled library.
        const ShaderCodeKind kind = filePath.extension() == ".metal" ? ShaderCodeKind::MetalSource
                                                                     : ShaderCodeKind::MetalLibrary;
        return Ref<ShaderModule>(new ShaderModule(context, ShaderModuleBinaryInfo{
                                                               .Name = info.Name,
                                                               .Binary = buffer,
                                                               .Kind = kind,
                                                               .EntryPoint = info.EntryPoint,
                                                           }));
    }

    ShaderModule::ShaderModule(Context& context, const ShaderModuleBinaryInfo& info)
        : m_Context(context), m_Name(info.Name), m_EntryPoint(info.EntryPoint),
          m_ThreadgroupSize(info.ThreadgroupSize), m_Native(CreateUnique<Native>())
    {
        const id<MTLDevice> device = context.GetNative().Device;
        NSError* error = nil;
        switch (info.Kind)
        {
        case ShaderCodeKind::MetalLibrary:
        {
            const dispatch_data_t data =
                dispatch_data_create(info.Binary.data(), info.Binary.size_bytes(), nullptr,
                                     DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            m_Native->Library = [device newLibraryWithData:data error:&error];
            break;
        }
        case ShaderCodeKind::MetalSource:
        {
            MTLCompileOptions* const options = [[MTLCompileOptions alloc] init];
            options.languageVersion = MTLLanguageVersion4_0;
            // The cooked libraries are compiled with fast math, and so is run-time source.
            options.mathMode = MTLMathModeFast;
            NSString* const source = [[NSString alloc] initWithBytes:info.Binary.data()
                                                              length:info.Binary.size_bytes()
                                                            encoding:NSUTF8StringEncoding];
            m_Native->Library = [device newLibraryWithSource:source options:options error:&error];
            break;
        }
        case ShaderCodeKind::SpirV:
            VE_ASSERT(
                false,
                "ShaderModule '{}': the Metal backend creates modules from a Metal library or "
                "Metal source, not SPIR-V",
                info.Name);
        }
        VE_ASSERT(m_Native->Library != nil, "ShaderModule '{}': Metal refused the code: {}",
                  info.Name, Metal::Describe(error));
        [m_Native->Library setLabel:Metal::ToNSString(info.Name)];
    }

    ShaderModule::~ShaderModule()
    {
        m_Context.GetNative().Retire(m_Native->Library);
        m_Native->Library = nil;
    }
}
