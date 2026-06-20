#include <Veng/Renderer/GraphicsPipeline.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer
{
    /// @brief Returns the backend-native pipeline handle.
    GraphicsPipeline::Native& GraphicsPipeline::GetNative() const { return *m_Native; }

    /// @brief Constructs and compiles a Vulkan graphics pipeline from the given info.
    ///
    /// Uses dynamic viewport/scissor state and dynamic rendering (no render pass object).
    /// @param context  The owning render context.
    /// @param info     Pipeline configuration including shaders, attachments, and rasterizer state.
    GraphicsPipeline::GraphicsPipeline(Context& context, const GraphicsPipelineInfo& info) : m_Context(context), m_Name(info.Name),
        m_Native(CreateUnique<Native>()), m_PipelineLayout(info.PipelineLayout),
        m_DepthAttachmentFormat(info.DepthAttachmentFormat)
    {
        vector<vk::Format> colorAttachmentFormats;
        colorAttachmentFormats.reserve(info.ColorAttachments.size());

        m_ColorAttachmentFormats.reserve(info.ColorAttachments.size());

        for (auto& attachment : info.ColorAttachments)
        {
            colorAttachmentFormats.push_back(ToVk(attachment.Format));
            m_ColorAttachmentFormats.push_back(attachment.Format);
        }

        const vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo = {
            .viewMask = 0,
            .colorAttachmentCount = static_cast<u32>(colorAttachmentFormats.size()),
            .pColorAttachmentFormats = colorAttachmentFormats.data(),
            .depthAttachmentFormat = ToVk(info.DepthAttachmentFormat),
            .stencilAttachmentFormat = ToVk(info.StencilAttachmentFormat),
        };

        vector<vk::PipelineShaderStageCreateInfo> shaderStages;
        shaderStages.reserve(info.ShaderStages.size());

        for (const auto& shaderStage : info.ShaderStages)
        {
            shaderStages.push_back({
                .stage = ToVkBit(shaderStage.Stage),
                .module = shaderStage.Module->GetNative().Module,
                .pName = shaderStage.Module->GetEntryPoint().c_str()
            });
        }

        vk::PipelineVertexInputStateCreateInfo vertexInputInfo;
        vk::VertexInputBindingDescription bindingDescription;
        vector<vk::VertexInputAttributeDescription> attributeDescriptions;

        if (info.VertexBufferLayout.has_value())
        {
            auto& vertexBufferLayout = info.VertexBufferLayout.value();

            bindingDescription = {
                .binding = 0,
                .stride = vertexBufferLayout.GetStride(),
                .inputRate = vk::VertexInputRate::eVertex,
            };

            auto& elements = vertexBufferLayout.GetElements();

            for (u32 i = 0; i < elements.size(); i++)
            {
                auto& element = elements[i];

                vk::VertexInputAttributeDescription attributeDescription = {
                    .location = i,
                    .binding = 0,
                    .format = ToVk(element.Type),
                    .offset = element.Offset,
                };

                attributeDescriptions.push_back(attributeDescription);
            }

            vertexInputInfo = {
                .vertexBindingDescriptionCount = 1,
                .pVertexBindingDescriptions = &bindingDescription,
                .vertexAttributeDescriptionCount = static_cast<u32>(attributeDescriptions.size()),
                .pVertexAttributeDescriptions = attributeDescriptions.data(),
            };
        }
        else
        {
            vertexInputInfo = {
                .vertexBindingDescriptionCount = 0,
                .pVertexBindingDescriptions = nullptr,
                .vertexAttributeDescriptionCount = 0,
                .pVertexAttributeDescriptions = nullptr
            };
        }

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = VK_FALSE
        };

        vk::PipelineTessellationStateCreateInfo tessellationState{
            .patchControlPoints = 0
        };

        vk::PipelineViewportStateCreateInfo viewportState{
            .viewportCount = 1,
            .scissorCount = 1
        };

        vk::PipelineRasterizationStateCreateInfo rasterizerState = {
            .depthClampEnable = vk::False,
            .rasterizerDiscardEnable = vk::False,
            .polygonMode = ToVk(info.PolygonMode),
            .cullMode = ToVk(info.CullMode),
            .frontFace = vk::FrontFace::eCounterClockwise,
            .depthBiasEnable = vk::False,
            .lineWidth = 1.0f,
        };

        vk::PipelineMultisampleStateCreateInfo multisampleState = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = vk::False
        };

        vk::PipelineDepthStencilStateCreateInfo depthStencilState = {
            .depthTestEnable = VK_BOOL(info.DepthTestEnable),
            .depthWriteEnable = VK_BOOL(info.DepthWriteEnable),
            .depthCompareOp = ToVk(info.DepthCompareOp),
            .depthBoundsTestEnable = vk::False,
            .stencilTestEnable = vk::False,
            .front = {},
            .back = {},
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f
        };

        vector<vk::PipelineColorBlendAttachmentState> blendAttachments;
        blendAttachments.reserve(info.ColorAttachments.size());

        for (auto& attachment : info.ColorAttachments)
        {
            blendAttachments.push_back(ToVk(attachment.Blend));
        }

        vk::PipelineColorBlendStateCreateInfo colorBlendState = {
            .logicOpEnable = vk::False,
            .logicOp = vk::LogicOp::eCopy,
            .attachmentCount = static_cast<u32>(blendAttachments.size()),
            .pAttachments = blendAttachments.data(),
            .blendConstants = {{0, 0, 0, 0,}},
        };

        vector<vk::DynamicState> dynamicStates = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor
        };

        vk::PipelineDynamicStateCreateInfo dynamicState = {
            .dynamicStateCount = static_cast<u32>(dynamicStates.size()),
            .pDynamicStates = dynamicStates.data()
        };

        const vk::GraphicsPipelineCreateInfo pipelineInfo = {
            .pNext = &pipelineRenderingCreateInfo,
            .stageCount = static_cast<u32>(shaderStages.size()),
            .pStages = shaderStages.data(),
            .pVertexInputState = &vertexInputInfo,
            .pInputAssemblyState = &inputAssembly,
            .pTessellationState = &tessellationState,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizerState,
            .pMultisampleState = &multisampleState,
            .pDepthStencilState = &depthStencilState,
            .pColorBlendState = &colorBlendState,
            .pDynamicState = &dynamicState,
            .layout = m_PipelineLayout->GetNative().Layout,
            .renderPass = nullptr,
            .subpass = 0,
            .basePipelineHandle = nullptr,
            .basePipelineIndex = 0,
        };

        m_Native->Pipeline = GetVkDevice(m_Context).createGraphicsPipeline(GetVkPipelineCache(m_Context), pipelineInfo).value;

        DebugMarkers::MarkPipeline(GetVkDevice(m_Context), m_Native->Pipeline, m_Name);
    }

    /// @brief Defers destruction of the Vulkan pipeline handle until the GPU is done with it.
    GraphicsPipeline::~GraphicsPipeline()
    {
        m_Context.GetNative().Retire(m_Native->Pipeline);
    }
}
