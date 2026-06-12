#include <Veng/Renderer/Backend/DynamicGraphicsPipeline.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer
{
    DynamicGraphicsPipeline::DynamicGraphicsPipeline(const DynamicPipelineInfo& info) : m_Name(info.Name),
        m_PipelineLayout(info.PipelineLayout)
    {
        vector<vk::Format> colorAttachmentFormats;
        colorAttachmentFormats.reserve(info.ColorAttachments.size());

        for (auto& attachment : info.ColorAttachments)
        {
            colorAttachmentFormats.push_back(attachment.Format);
        }

        const vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo = {
            .viewMask = 0,
            .colorAttachmentCount = static_cast<u32>(colorAttachmentFormats.size()),
            .pColorAttachmentFormats = colorAttachmentFormats.data(),
            .depthAttachmentFormat = info.DepthAttachmentFormat,
            .stencilAttachmentFormat = info.StencilAttachmentFormat,
        };

        vector<vk::PipelineShaderStageCreateInfo> shaderStages;
        shaderStages.reserve(info.ShaderStages.size());

        for (const auto& shaderStage : info.ShaderStages)
        {
            shaderStages.push_back({
                .stage = shaderStage.Stage,
                .module = shaderStage.Module.GetVkModule(),
                .pName = shaderStage.Module.GetEntryPoint().c_str()
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
                    .format = VertexElementDataTypeToVulkanFormat(element.Type),
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
            .polygonMode = info.PolygonMode,
            .cullMode = info.CullMode,
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
            .depthCompareOp = info.DepthCompareOp,
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
            blendAttachments.push_back(attachment.BlendMode);
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
            .layout = m_PipelineLayout->GetVkPipelineLayout(),
            .renderPass = nullptr,
            .subpass = 0,
            .basePipelineHandle = nullptr,
            .basePipelineIndex = 0,
        };

        m_VkPipeline = Context::Instance().GetVkDevice().createGraphicsPipeline(nullptr, pipelineInfo).value;

        DebugMarkers::MarkPipeline(m_VkPipeline, m_Name);
    }

    DynamicGraphicsPipeline::~DynamicGraphicsPipeline()
    {
        Context::Instance().GetVkDevice().destroyPipeline(m_VkPipeline);
    }
}
