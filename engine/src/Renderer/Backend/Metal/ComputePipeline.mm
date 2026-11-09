#include <Veng/Renderer/ComputePipeline.h>

#include <cstdlib>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    ComputePipeline::Native& ComputePipeline::GetNative() const
    {
        return *m_Native;
    }

    ComputePipeline::ComputePipeline(Context& context, const ComputePipelineInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()),
          m_PipelineLayout(info.PipelineLayout)
    {
        VE_ASSERT(m_PipelineLayout != nullptr, "ComputePipeline '{}' requires a PipelineLayout",
                  m_Name);
        VE_ASSERT(info.ShaderStage.Stage == ShaderStage::Compute,
                  "ComputePipeline '{}' requires a compute shader stage", m_Name);

        const ShaderModule& module = *info.ShaderStage.Module;
        MTL4LibraryFunctionDescriptor* const function =
            [[MTL4LibraryFunctionDescriptor alloc] init];
        function.library = module.GetNative().Library;
        function.name = Metal::ToNSString(module.GetEntryPoint());

        // A Metal dispatch names its threadgroup size, and the library does not report the
        // kernel's, so the module carries it from the shader's reflection.
        const uvec3 size = module.GetThreadgroupSize();
        VE_ASSERT(size.x * size.y * size.z > 0,
                  "ComputePipeline '{}': module '{}' carries no threadgroup size; a compute "
                  "module on Metal is created with its [numthreads]",
                  m_Name, module.GetName());
        m_Native->ThreadsPerThreadgroup = MTLSizeMake(size.x, size.y, size.z);

        MTL4ComputePipelineDescriptor* const descriptor =
            [[MTL4ComputePipelineDescriptor alloc] init];
        descriptor.computeFunctionDescriptor = function;
        descriptor.requiredThreadsPerThreadgroup = m_Native->ThreadsPerThreadgroup;
        descriptor.label = Metal::ToNSString(m_Name);
#ifdef VE_ENABLE_VALIDATION_LAYERS
        // A pipeline served from the shader cache is not instrumented for shader validation unless
        // it asks to be, so a process run under validation asks for every pipeline.
        if (Metal::IsEnvironmentSwitchOn("MTL_SHADER_VALIDATION"))
        {
            MTL4PipelineOptions* const options = [[MTL4PipelineOptions alloc] init];
            options.shaderValidation = MTLShaderValidationEnabled;
            descriptor.options = options;
        }
#endif

        NSError* error = nil;
        m_Native->State = context.GetNative().CreateComputePipeline(descriptor, &error);
        VE_ASSERT(m_Native->State != nil, "ComputePipeline '{}': Metal refused '{}' in '{}': {}",
                  m_Name, module.GetEntryPoint(), module.GetName(), Metal::Describe(error));
    }

    ComputePipeline::~ComputePipeline()
    {
        m_Context.GetNative().Retire(m_Native->State);
        m_Native->State = nil;
    }
}
