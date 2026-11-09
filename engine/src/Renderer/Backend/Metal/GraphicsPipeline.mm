#include <Veng/Renderer/GraphicsPipeline.h>

#include <cstdlib>

#include <Veng/Renderer/Context.h>

#include "Natives.h"
#include "TypeMapping.h"

namespace Veng::Renderer
{
    namespace
    {
        MTL4LibraryFunctionDescriptor* FunctionOf(const ShaderModule& module)
        {
            MTL4LibraryFunctionDescriptor* const function =
                [[MTL4LibraryFunctionDescriptor alloc] init];
            function.library = module.GetNative().Library;
            function.name = Metal::ToNSString(module.GetEntryPoint());
            return function;
        }

        // Vertex buffer binding N is read from buffer slot VertexBufferBase + N, where
        // BindVertexBuffer and BindInstanceBuffer put it.
        MTLVertexDescriptor* VertexDescriptorOf(const GraphicsPipelineInfo& info)
        {
            namespace Slots = Cook::MetalBindingSlots;
            const VertexBufferLayout& layout = *info.VertexBufferLayout;
            MTLVertexDescriptor* const descriptor = [MTLVertexDescriptor vertexDescriptor];
            const vector<VertexBufferElement>& elements = layout.GetElements();
            for (usize i = 0; i < elements.size(); ++i)
            {
                MTLVertexAttributeDescriptor* const attribute = descriptor.attributes[i];
                attribute.format = Metal::ToMetalVertexFormat(elements[i].Type);
                attribute.offset = elements[i].Offset;
                attribute.bufferIndex = Slots::VertexBufferBase;
            }
            MTLVertexBufferLayoutDescriptor* const vertices =
                descriptor.layouts[Slots::VertexBufferBase];
            vertices.stride = layout.GetStride();
            vertices.stepFunction = MTLVertexStepFunctionPerVertex;

            if (info.InstanceCandidateId)
            {
                MTLVertexAttributeDescriptor* const attribute =
                    descriptor.attributes[elements.size()];
                attribute.format = MTLVertexFormatUInt;
                attribute.offset = 0;
                attribute.bufferIndex = Slots::VertexBufferBase + 1;
                MTLVertexBufferLayoutDescriptor* const instances =
                    descriptor.layouts[Slots::VertexBufferBase + 1];
                instances.stride = sizeof(u32);
                instances.stepFunction = MTLVertexStepFunctionPerInstance;
                instances.stepRate = 1;
            }
            return descriptor;
        }
    }

    GraphicsPipeline::Native& GraphicsPipeline::GetNative() const
    {
        return *m_Native;
    }

    GraphicsPipeline::GraphicsPipeline(Context& context, const GraphicsPipelineInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()),
          m_PipelineLayout(info.PipelineLayout), m_DepthAttachmentFormat(info.DepthAttachmentFormat)
    {
        VE_ASSERT(m_PipelineLayout != nullptr, "GraphicsPipeline '{}' requires a PipelineLayout",
                  m_Name);
        VE_ASSERT(info.PolygonMode != PolygonMode::Point,
                  "GraphicsPipeline '{}': Metal rasterizes triangles filled or as lines, not as "
                  "points",
                  m_Name);

        MTL4RenderPipelineDescriptor* const descriptor =
            [[MTL4RenderPipelineDescriptor alloc] init];
        descriptor.label = Metal::ToNSString(m_Name);
        for (const PipelineShaderStageInfo& stage : info.ShaderStages)
        {
            switch (stage.Stage)
            {
            case ShaderStage::Vertex:
                descriptor.vertexFunctionDescriptor = FunctionOf(*stage.Module);
                break;
            case ShaderStage::Fragment:
                descriptor.fragmentFunctionDescriptor = FunctionOf(*stage.Module);
                break;
            default:
                VE_ASSERT(false, "GraphicsPipeline '{}': stage {} is not a render stage", m_Name,
                          static_cast<u32>(stage.Stage));
            }
        }
        VE_ASSERT(descriptor.vertexFunctionDescriptor != nil,
                  "GraphicsPipeline '{}' requires a vertex stage", m_Name);
        if (info.VertexBufferLayout.has_value())
        {
            descriptor.vertexDescriptor = VertexDescriptorOf(info);
        }
        descriptor.inputPrimitiveTopology = Metal::ToMetalClass(info.Topology);
        descriptor.rasterSampleCount = 1;
        // A count-driven draw executes an indirect command buffer that inherits the pipeline.
        descriptor.supportIndirectCommandBuffers = MTL4IndirectCommandBufferSupportStateEnabled;
        // Culling both faces draws nothing, which a pipeline that rasterizes nothing does.
        descriptor.rasterizationEnabled = info.CullMode != CullMode::FrontAndBack;

        m_ColorAttachmentFormats.reserve(info.ColorAttachments.size());
        for (usize i = 0; i < info.ColorAttachments.size(); ++i)
        {
            const PipelineAttachmentInfo& attachment = info.ColorAttachments[i];
            m_ColorAttachmentFormats.push_back(attachment.Format);
            MTL4RenderPipelineColorAttachmentDescriptor* const color =
                descriptor.colorAttachments[i];
            color.pixelFormat = Metal::RequireMetal(
                attachment.Format, fmt::format("GraphicsPipeline '{}' attachment {}", m_Name, i));
            // An unbound slot takes nothing, whatever its blend says.
            const bool writes = attachment.Write && attachment.Format != Format::Undefined;
            color.writeMask = writes ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            const BlendState& blend = attachment.Blend;
            color.blendingState =
                writes && blend.Enable ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
            color.sourceRGBBlendFactor = Metal::ToMetal(blend.SrcColorFactor);
            color.destinationRGBBlendFactor = Metal::ToMetal(blend.DstColorFactor);
            color.rgbBlendOperation = Metal::ToMetal(blend.ColorOp);
            color.sourceAlphaBlendFactor = Metal::ToMetal(blend.SrcAlphaFactor);
            color.destinationAlphaBlendFactor = Metal::ToMetal(blend.DstAlphaFactor);
            color.alphaBlendOperation = Metal::ToMetal(blend.AlphaOp);
        }

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

        Context::Native& native = context.GetNative();
        NSError* error = nil;
        m_Native->State = native.CreateRenderPipeline(descriptor, &error);
        VE_ASSERT(m_Native->State != nil, "GraphicsPipeline '{}': Metal refused the pipeline: {}",
                  m_Name, Metal::Describe(error));

        MTLDepthStencilDescriptor* const depth = [[MTLDepthStencilDescriptor alloc] init];
        depth.depthCompareFunction =
            info.DepthTestEnable ? Metal::ToMetal(info.DepthCompareOp) : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = info.DepthTestEnable && info.DepthWriteEnable;
        depth.label = Metal::ToNSString(m_Name);
        m_Native->DepthStencil = [native.Device newDepthStencilStateWithDescriptor:depth];

        m_Native->Primitive = Metal::ToMetal(info.Topology);
        m_Native->Cull = Metal::ToMetal(info.CullMode);
        m_Native->Fill = info.PolygonMode == PolygonMode::Line ? MTLTriangleFillModeLines
                                                               : MTLTriangleFillModeFill;
        m_Native->DepthClip = info.DepthClampEnable ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
    }

    GraphicsPipeline::~GraphicsPipeline()
    {
        Context::Native& native = m_Context.GetNative();
        native.Retire(m_Native->State);
        native.Retire(m_Native->DepthStencil);
        m_Native->State = nil;
        m_Native->DepthStencil = nil;
    }
}
