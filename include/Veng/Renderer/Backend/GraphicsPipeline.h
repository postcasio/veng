#pragma once

#include <Veng/Renderer/Backend/PipelineLayout.h>
#include <Veng/Renderer/Backend/RenderPass.h>
#include <Veng/Renderer/Backend/Shader.h>
#include <Veng/Renderer/Backend/VertexBufferLayout.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct GraphicsPipelineInfo
    {
        string Name;

        optional<VertexBufferLayout> VertexBufferLayout;
        Ref<PipelineLayout> PipelineLayout;

        vector<PipelineShaderStageInfo> ShaderStages = {};
        vk::PolygonMode PolygonMode = vk::PolygonMode::eFill;
        vk::CullModeFlags CullMode = vk::CullModeFlagBits::eBack;
        bool DepthTestEnable = true;
        bool DepthWriteEnable = true;
        vk::CompareOp DepthCompareOp = vk::CompareOp::eLessOrEqual;
        vector<vk::PipelineColorBlendAttachmentState> ColorBlendAttachments = {};

        Ref<RenderPass> RenderPass;
    };

    class GraphicsPipeline
    {
    public:
        static Ref<GraphicsPipeline> Create(const GraphicsPipelineInfo& info)
        {
            return CreateRef<GraphicsPipeline>(info);
        }

        explicit GraphicsPipeline(const GraphicsPipelineInfo& info);
        ~GraphicsPipeline();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] vk::Pipeline GetVkPipeline() const { return m_VkPipeline; }
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }
        [[nodiscard]] Ref<RenderPass> GetRenderPass() const { return m_RenderPass; }

    private:
        string m_Name;
        vk::Pipeline m_VkPipeline;
        Ref<PipelineLayout> m_PipelineLayout;
        Ref<RenderPass> m_RenderPass;
    };
}
