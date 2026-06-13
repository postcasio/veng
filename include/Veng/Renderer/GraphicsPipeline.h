#pragma once

#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderPass.h>
#include <Veng/Renderer/Shader.h>
#include <Veng/Renderer/VertexBufferLayout.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    struct GraphicsPipelineInfo
    {
        string Name;

        optional<VertexBufferLayout> VertexBufferLayout;
        Ref<PipelineLayout> PipelineLayout;

        vector<PipelineShaderStageInfo> ShaderStages = {};
        PolygonMode PolygonMode = PolygonMode::Fill;
        CullMode CullMode = CullMode::Back;
        bool DepthTestEnable = true;
        bool DepthWriteEnable = true;
        CompareOp DepthCompareOp = CompareOp::LessOrEqual;
        vector<BlendState> ColorBlendAttachments = {};

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
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }
        [[nodiscard]] Ref<RenderPass> GetRenderPass() const { return m_RenderPass; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
        Ref<PipelineLayout> m_PipelineLayout;
        Ref<RenderPass> m_RenderPass;
    };
}
