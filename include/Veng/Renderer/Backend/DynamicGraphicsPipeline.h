#pragma once

#include <Veng/Renderer/Backend/PipelineLayout.h>
#include <Veng/Renderer/Backend/Shader.h>
#include <Veng/Renderer/Backend/VertexBufferLayout.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct DynamicPipelineInfo
    {
        string Name;

        vector<PipelineAttachmentInfo> ColorAttachments = {};
        vk::Format DepthAttachmentFormat = vk::Format::eUndefined;
        vk::Format StencilAttachmentFormat = vk::Format::eUndefined;

        optional<VertexBufferLayout> VertexBufferLayout;
        Ref<PipelineLayout> PipelineLayout;

        vector<PipelineShaderStageInfo> ShaderStages = {};
        vk::PolygonMode PolygonMode = vk::PolygonMode::eFill;
        vk::CullModeFlags CullMode = vk::CullModeFlagBits::eNone;
        bool DepthTestEnable = false;
        bool DepthWriteEnable = false;
        vk::CompareOp DepthCompareOp = vk::CompareOp::eLessOrEqual;
    };

    class DynamicGraphicsPipeline
    {
    public:
        static Ref<DynamicGraphicsPipeline> Create(const DynamicPipelineInfo& info)
        {
            return CreateRef<DynamicGraphicsPipeline>(info);
        }

        explicit DynamicGraphicsPipeline(const DynamicPipelineInfo& info);
        ~DynamicGraphicsPipeline();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] vk::Pipeline GetVkPipeline() const { return m_VkPipeline; }
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

    private:
        string m_Name;
        vk::Pipeline m_VkPipeline;
        Ref<PipelineLayout> m_PipelineLayout;
    };
}
